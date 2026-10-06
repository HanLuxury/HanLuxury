#include "ShaderPatcher.h"
#include "GLCaps.h"
#include "GraphicsLog.h"
#include "ShaderUniforms.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace gfx {
namespace {

constexpr char kTag[] = "ShaderPatch";
constexpr size_t kStatementLimit = 440; // ES2Shader::CheckCompile buffer is 0x208 bytes
constexpr int kMaxFailuresBeforeDisable = 12;

struct Receiver {
    GLuint program = 0;
    GLint map = -1, vp = -1, tile = -1, split = -1, cfg = -1, bias = -1, dbias = -1, texel = -1;
    GLint light = -1, tint = -1, boost = -1, user = -1;
    uint32_t stamp = 0;
    bool lit = false;
};

// Configuration (written on the game thread, read on the GL thread under the mutex).
std::mutex g_configMutex;
bool g_enabled = false;
bool g_allowHardware = true;
int g_pcfLevel = 2;
bool g_blend = true;
bool g_water = false;
ReceiverSnippets g_snippets; // sanitized + validated

// GL-thread state.
std::atomic<bool> g_featuresResolved{false};
std::atomic<bool> g_hardwareCompare{false};
std::atomic<bool> g_patchingAllowed{true};
std::atomic<int> g_receiverCount{0};
int g_failures = 0;
int g_patched = 0;
int g_patchedPerEntity[3] = {};
std::unordered_map<void*, Receiver> g_receivers;
ShaderPatcher::Mode g_mode = ShaderPatcher::Mode::Disabled;
ShadowUniforms g_uniforms{};
float g_user[ShaderUniforms::kSlotCount] = {};
uint32_t g_stamp = 1;
void** g_activeShaderSlot = nullptr;

GLuint ProgramOf(void* shader) {
    GLuint program = 0;
    std::memcpy(&program, static_cast<const char*>(shader) + ShaderPatcher::kEs2ShaderProgramOffset, sizeof(program));
    return program;
}

int TapsForLevel(int level) { return level <= 0 ? 1 : (level == 1 ? 4 : 9); }

bool Contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

void Upload(Receiver& r) {
    switch (g_mode) {
        case ShaderPatcher::Mode::Disabled:
            glUniform4f(r.light, 0.0f, 0.0f, 0.0f, 0.0f);
            break;
        case ShaderPatcher::Mode::Caster:
            glUniform4f(r.light, 0.0f, 0.0f, 0.0f, -1.0f);
            glUniform4fv(r.bias, 1, g_uniforms.bias);
            break;
        case ShaderPatcher::Mode::Receive:
            glUniformMatrix4fv(r.vp, 4, GL_FALSE, &g_uniforms.vp[0][0]);
            glUniform4fv(r.tile, 4, &g_uniforms.tile[0][0]);
            glUniform4fv(r.split, 1, g_uniforms.split);
            glUniform4fv(r.cfg, 1, g_uniforms.cfg);
            glUniform4fv(r.bias, 1, g_uniforms.bias);
            glUniform4fv(r.dbias, 1, g_uniforms.dbias);
            glUniform4fv(r.texel, 1, g_uniforms.texel);
            glUniform4fv(r.tint, 1, g_uniforms.tint);
            glUniform4fv(r.boost, 1, g_uniforms.boost);
            glUniform4fv(r.light, 1, g_uniforms.light);
            if (r.user >= 0) glUniform4fv(r.user, ShaderUniforms::kVec4Count, g_user);
            break;
    }
    r.stamp = g_stamp;
}

void LogShaderLog(const char* what, GLuint object, bool program) {
    char log[1536] = {};
    if (program) glGetProgramInfoLog(object, sizeof(log) - 1, nullptr, log);
    else glGetShaderInfoLog(object, sizeof(log) - 1, nullptr, log);
    const char* p = log;
    for (int line = 0; *p && line < 12; ++line) {
        const char* nl = std::strchr(p, '\n');
        const int len = nl ? static_cast<int>(nl - p) : static_cast<int>(std::strlen(p));
        if (len > 0) GFX_LOGE(kTag, "%s: %.*s", what, len, p);
        if (!nl) break;
        p = nl + 1;
    }
}

// Compiles the failing patched sources again only to get readable errors.
void DiagnoseFailure(const std::string& ps, const std::string& vs) {
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    const char* vsrc = vs.c_str();
    const char* fsrc = ps.c_str();
    glShaderSource(v, 1, &vsrc, nullptr);
    glShaderSource(f, 1, &fsrc, nullptr);
    glCompileShader(v);
    glCompileShader(f);
    GLint okV = 0, okF = 0;
    glGetShaderiv(v, GL_COMPILE_STATUS, &okV);
    glGetShaderiv(f, GL_COMPILE_STATUS, &okF);
    if (!okV) LogShaderLog("world.vert (patched)", v, false);
    if (!okF) LogShaderLog("world.frag (patched)", f, false);
    if (okV && okF) {
        GLuint p = glCreateProgram();
        glAttachShader(p, v);
        glAttachShader(p, f);
        glLinkProgram(p);
        GLint okP = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &okP);
        if (!okP) LogShaderLog("world program (patched)", p, true);
        glDeleteProgram(p);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}
}

void ResolveFeatures() {
    if (g_featuresResolved.load(std::memory_order_acquire)) return;
    GLCaps& caps = GLCaps::Get();
    const bool es3 = caps.Detect();
    bool allowHw;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        allowHw = g_allowHardware;
    }
    g_hardwareCompare.store(es3 && allowHw && caps.shadowSamplers, std::memory_order_release);
    if (!es3) {
        g_patchingAllowed.store(false);
        GFX_LOGE(kTag, "OpenGL ES 3.0 not available: shadow receivers disabled");
    }
    if (!caps.fragmentHighp) GFX_LOGW(kTag, "fragment highp not reported; receivers may lose precision");
    GFX_LOGI(kTag, "receiver path: %s compare", g_hardwareCompare.load() ? "hardware (sampler2DShadow)" : "manual");
    g_featuresResolved.store(true, std::memory_order_release);
}

void Register(void* shader, const PatchInfo& info) {
    const GLuint program = ProgramOf(shader);
    if (!program) return;
    Receiver r;
    r.program = program;
    r.lit = info.lit;
    r.map = glGetUniformLocation(program, "SG_ShadowMap");
    r.vp = glGetUniformLocation(program, "SG_VP");
    r.tile = glGetUniformLocation(program, "SG_Tile");
    r.split = glGetUniformLocation(program, "SG_Split");
    r.cfg = glGetUniformLocation(program, "SG_Cfg");
    r.bias = glGetUniformLocation(program, "SG_Bias");
    r.dbias = glGetUniformLocation(program, "SG_DBias");
    r.texel = glGetUniformLocation(program, "SG_Texel");
    r.light = glGetUniformLocation(program, "SG_Light");
    r.tint = glGetUniformLocation(program, "SG_Tint");
    r.boost = glGetUniformLocation(program, "SG_Boost");
    r.user = glGetUniformLocation(program, "SG_User");
    if (r.light < 0 || r.map < 0) {
        GFX_LOGW(kTag, "program %u patched but SG uniforms inactive, not registered", program);
        return;
    }
    GLint current = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current);
    glUseProgram(program);
    glUniform1i(r.map, ShaderPatcher::kShadowTextureUnit);
    glUniform4f(r.light, 0.0f, 0.0f, 0.0f, 0.0f);
    glUseProgram(static_cast<GLuint>(current));
    r.stamp = 0; // force an upload on first select
    g_receivers[shader] = r;
    g_receiverCount.store(static_cast<int>(g_receivers.size()), std::memory_order_relaxed);
}

} // namespace

bool ShaderPatcher::ValidateSnippet(const std::string& clean, std::string& why) {
    if (!Contains(clean, "void SG_Apply(")) { why = "SG_Apply missing"; return false; }
    if (!Contains(clean, "SG_SpecVis")) { why = "SG_SpecVis missing"; return false; }
    if (!Contains(clean, "void SG_InitUser(")) { why = "SG_InitUser missing"; return false; }
    const size_t longest = MaxStatementLength(clean);
    if (longest > kStatementLimit) {
        why = "a statement has " + std::to_string(longest) + " chars (limit " + std::to_string(kStatementLimit) + ")";
        return false;
    }
    return true;
}

void ShaderPatcher::Configure(bool enabled, bool allowHardwareCompare, int pcfLevel, bool blend, bool water,
                              const ReceiverSnippets& snippets) {
    ReceiverSnippets clean;
    for (int i = 0; i < 3; ++i) {
        clean.snippet[i] = SanitizeSnippet(snippets.snippet[i]);
        std::string why;
        if (!ValidateSnippet(clean.snippet[i], why)) {
            GFX_LOGE(kTag, "%s receiver rejected (%s): those shaders stay unpatched",
                     EntityName(static_cast<ReceiverEntity>(i)), why.c_str());
            clean.snippet[i].clear();
        }
    }
    std::lock_guard<std::mutex> lock(g_configMutex);
    g_enabled = enabled;
    g_allowHardware = allowHardwareCompare;
    g_pcfLevel = pcfLevel;
    g_blend = blend;
    g_water = water;
    g_snippets = clean;
}

bool ShaderPatcher::Enabled() {
    std::lock_guard<std::mutex> lock(g_configMutex);
    return g_enabled;
}

ReceiverEntity ShaderPatcher::Classify(const std::string& ps, const std::string& vs) {
    if (Contains(vs, "BoneToLocal")) return ReceiverEntity::Character;                 // skinned: peds
    if (Contains(vs, "DirLightDirection") && Contains(ps, "Out_Spec")) return ReceiverEntity::Vehicle; // lit + specular
    return ReceiverEntity::Building;                                                  // prelit world, trees, objects
}

const char* ShaderPatcher::EntityName(ReceiverEntity e) {
    switch (e) {
        case ReceiverEntity::Vehicle: return "Vehicle";
        case ReceiverEntity::Character: return "Character";
        default: return "Building";
    }
}

std::string ShaderPatcher::SanitizeSnippet(const std::string& text) {
    // 1) strip comments
    std::string code;
    code.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            code += '\n';
        } else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) ++i;
            ++i;
            code += ' ';
        } else if (text[i] != '\r') {
            code += text[i];
        }
    }
    // 2) trim lines, drop empty ones, collapse indentation
    std::string out;
    size_t pos = 0;
    while (pos <= code.size()) {
        size_t end = code.find('\n', pos);
        if (end == std::string::npos) end = code.size();
        size_t b = pos, e = end;
        while (b < e && (code[b] == ' ' || code[b] == '\t')) ++b;
        while (e > b && (code[e - 1] == ' ' || code[e - 1] == '\t')) --e;
        if (e > b) {
            out.append(code, b, e - b);
            out += '\n';
        }
        pos = end + 1;
    }
    return out;
}

size_t ShaderPatcher::MaxStatementLength(const std::string& source) {
    // Same segmentation as ES2Shader::CheckCompile: pieces end at ';', '{' or '}'.
    size_t longest = 0, current = 0;
    for (char c : source) {
        ++current;
        if (c == ';' || c == '{' || c == '}') {
            longest = std::max(longest, current);
            current = 0;
        }
    }
    return std::max(longest, current);
}

bool ShaderPatcher::PatchSources(const std::string& ps, const std::string& vs, const ReceiverSnippets& snippets,
                                 const PatchOptions& options, std::string& outPs, std::string& outVs, PatchInfo& info) {
    // ---- eligibility: 3D world shaders only
    const size_t vsMain = vs.find("void main() {");
    if (vsMain == std::string::npos) return false;
    if (!Contains(vs, "vec4 WorldPos = ObjMatrix")) return false;
    if (!Contains(vs, "vec4 ViewPos = ViewMatrix * WorldPos;")) return false; // excludes sphere-map passes
    const size_t vsEnd = vs.rfind('}');
    if (vsEnd == std::string::npos || vsEnd < vsMain) return false;

    const size_t psMain = ps.find("void main()");
    if (psMain == std::string::npos) return false;
    if (!Contains(ps, "Out_FogAmt")) return false;                 // HUD/2D never use fog
    if (!Contains(ps, "gl_FragColor = fcolor;")) return false;
    if (!options.water && Contains(ps, "Out_WaterDetail")) return false;
    if (Contains(ps, "SG_") || Contains(vs, "SG_")) return false;  // already patched
    if (Contains(ps, "#version") || Contains(ps, "#extension")) return false;

    info = PatchInfo{};
    info.entity = Classify(ps, vs);
    const std::string& snippet = snippets.For(info.entity);
    if (snippet.empty()) return false; // this entity's receiver was rejected
    info.hardwareCompare = options.hardwareCompare;
    info.taps = options.taps <= 1 ? 1 : (options.taps <= 4 ? 4 : 9);
    info.alpha = Contains(ps, "discard;");
    const bool vsDirect = Contains(vs, "max(dot(DirLightDirection, WorldNormal), 0.0)");
    info.lit = vsDirect && Contains(ps, "varying lowp vec4 Out_Color;") &&
               Contains(vs, "uniform lowp vec4 MaterialDiffuse;") && Contains(vs, "uniform lowp vec3 DirLightDiffuseColor;");

    // ---- world normal source
    std::string normal;
    const bool fakeNormal = Contains(vs, "vec3 WorldNormal = normalize(vec3(WorldPos.xy - CameraPosition.xy");
    if (Contains(vs, "vec3 WorldNormal = mat3(ObjMatrix) * (Normal * mat3(BoneToLocal));") ||
        Contains(vs, "vec3 WorldNormal = (ObjMatrix * vec4(Normal,0.0)).xyz;")) {
        normal = "WorldNormal";
    } else if (fakeNormal || !Contains(vs, "attribute vec3 Normal;")) {
        normal = "vec3(0.0)"; // camera-facing tree normals / no normal stream: shadow map only
    } else if (Contains(vs, "BoneToLocal")) {
        normal = "mat3(ObjMatrix) * (Normal * mat3(BoneToLocal))";
    } else {
        normal = "(ObjMatrix * vec4(Normal, 0.0)).xyz";
    }

    // ---- vertex shader
    std::string vsDecl =
        "varying highp vec3 SG_vWorld;\n"
        "varying highp float SG_vDepth;\n"
        "varying mediump vec3 SG_vNormal;\n";
    std::string vsBody =
        "SG_vWorld = WorldPos.xyz;\n"
        "SG_vDepth = -ViewPos.z;\n"
        "SG_vNormal = " + normal + ";\n";
    if (info.lit) {
        vsDecl += "varying lowp vec3 SG_vDirect;\n";
        vsBody += "SG_vDirect = max(dot(DirLightDirection, WorldNormal), 0.0) * DirLightDiffuseColor * MaterialDiffuse.xyz;\n";
    }
    outVs.clear();
    outVs.reserve(vs.size() + vsDecl.size() + vsBody.size() + 8);
    outVs.append(vs, 0, vsMain);
    outVs += vsDecl;
    outVs.append(vs, vsMain, vsEnd - vsMain);
    outVs += vsBody;
    outVs.append(vs, vsEnd, std::string::npos);

    // ---- pixel shader
    static const char* const kAnchors[] = {
        "fcolor.xyz += Out_Spec;",
        "fcolor.xyz = mix(fcolor.xyz, FogColor, Out_FogAmt);",
        "fcolor.xyz += fcolor.xyz * 0.5;",
        "gl_FragColor = fcolor;",
    };
    size_t anchor = std::string::npos;
    for (const char* a : kAnchors) {
        const size_t p = ps.find(a, psMain);
        if (p != std::string::npos && p < anchor) anchor = p;
    }
    if (anchor == std::string::npos) return false;

    std::string header;
    if (info.hardwareCompare) header += "#extension GL_EXT_shadow_samplers : enable\n";
    header += "#define SG_HW ";
    header += info.hardwareCompare ? "1\n" : "0\n";
    header += "#define SG_TAPS " + std::to_string(info.taps) + "\n";
    header += "#define SG_BLEND ";
    header += options.blend ? "1\n" : "0\n";
    header += "#define SG_LIT ";
    header += info.lit ? "1\n" : "0\n";
    header += "#define SG_ALPHA ";
    header += info.alpha ? "1\n" : "0\n";

    outPs.clear();
    outPs.reserve(ps.size() + snippet.size() + header.size() + 64);
    outPs += header;
    outPs.append(ps, 0, psMain);
    outPs += '\n';
    outPs += snippet;
    outPs.append(ps, psMain, anchor - psMain);
    outPs += "SG_InitUser();SG_Apply(fcolor);";
    std::string tail = ps.substr(anchor);
    const std::string spec = "fcolor.xyz += Out_Spec;";
    const size_t specPos = tail.find(spec);
    if (specPos != std::string::npos) tail.replace(specPos, spec.size(), "fcolor.xyz += Out_Spec * SG_SpecVis;");
    outPs += tail;

    if (MaxStatementLength(outPs) > kStatementLimit || MaxStatementLength(outVs) > kStatementLimit) return false;
    return true;
}

void ShaderPatcher::SetActiveShaderSlot(void** slot) { g_activeShaderSlot = slot; }

bool ShaderPatcher::OnBuild(void* shader, const char* ps, const char* vs, BuildFn original) {
    if (!original) return false;
    if (!shader) return original(shader, ps, vs);

    // The ES2Shader object may be reused for a new program: drop stale state.
    auto old = g_receivers.find(shader);
    if (old != g_receivers.end()) {
        g_receivers.erase(old);
        g_receiverCount.store(static_cast<int>(g_receivers.size()), std::memory_order_relaxed);
    }

    bool enabled;
    PatchOptions options;
    ReceiverSnippets snippets;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        enabled = g_enabled;
        options.taps = TapsForLevel(g_pcfLevel);
        options.blend = g_blend;
        options.water = g_water;
        if (enabled) snippets = g_snippets;
    }
    if (!enabled || !ps || !vs || !g_patchingAllowed.load(std::memory_order_relaxed))
        return original(shader, ps, vs);

    ResolveFeatures();
    if (!g_patchingAllowed.load(std::memory_order_relaxed)) return original(shader, ps, vs);
    options.hardwareCompare = g_hardwareCompare.load(std::memory_order_acquire);

    std::string patchedPs, patchedVs;
    PatchInfo info;
    if (!PatchSources(ps, vs, snippets, options, patchedPs, patchedVs, info)) return original(shader, ps, vs);

    if (original(shader, patchedPs.c_str(), patchedVs.c_str())) {
        Register(shader, info);
        const int perEntity = ++g_patchedPerEntity[static_cast<int>(info.entity)];
        if (++g_patched <= 3 || perEntity == 1 || GraphicsLog::DebugEnabled())
            GFX_LOGI(kTag, "receiver #%d (%s #%d): program %u lit=%d alpha=%d taps=%d", g_patched,
                     EntityName(info.entity), perEntity, ProgramOf(shader), info.lit, info.alpha, info.taps);
        return true;
    }

    ++g_failures;
    if (g_failures <= 3) {
        GFX_LOGE(kTag, "patched world shader failed to build, falling back to the native shader");
        DiagnoseFailure(patchedPs, patchedVs);
    }
    if (g_failures >= kMaxFailuresBeforeDisable && g_patchingAllowed.exchange(false))
        GFX_LOGE(kTag, "%d patched shaders failed: receiver patching disabled for this session", g_failures);
    return original(shader, ps, vs);
}

void ShaderPatcher::OnSelect(void* shader) {
    if (g_receivers.empty() || !shader) return;
    auto it = g_receivers.find(shader);
    if (it == g_receivers.end()) return;
    Receiver& r = it->second;
    if (ProgramOf(shader) != r.program) { // object reused without our Build hook seeing it
        g_receivers.erase(it);
        return;
    }
    if (r.stamp != g_stamp) Upload(r);
}

void ShaderPatcher::SetMode(Mode mode, const ShadowUniforms* uniforms) {
    g_mode = mode;
    if (uniforms) g_uniforms = *uniforms;
    if (mode == Mode::Receive) ShaderUniforms::Get().Snapshot(g_user); // shaderUniform.ini values, once per frame
    ++g_stamp;
    if (!g_activeShaderSlot || g_receivers.empty()) return;
    void* active = *g_activeShaderSlot;
    if (!active) return;
    auto it = g_receivers.find(active);
    if (it == g_receivers.end()) return;
    GLint current = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current);
    if (static_cast<GLuint>(current) == it->second.program) Upload(it->second);
}

bool ShaderPatcher::EnsureFeatures() {
    ResolveFeatures();
    return g_patchingAllowed.load(std::memory_order_relaxed) && GLCaps::Get().es3;
}

bool ShaderPatcher::HardwareCompare() { return g_hardwareCompare.load(std::memory_order_acquire); }

bool ShaderPatcher::FeaturesResolved() { return g_featuresResolved.load(std::memory_order_acquire); }

int ShaderPatcher::ReceiverCount() { return g_receiverCount.load(std::memory_order_relaxed); }

void ShaderPatcher::ForgetAll() {
    g_receivers.clear();
    g_receiverCount.store(0);
    g_featuresResolved.store(false);
    GLCaps::Get().Invalidate();
}

} // namespace gfx
