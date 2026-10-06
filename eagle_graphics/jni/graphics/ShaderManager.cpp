#include "ShaderManager.h"
#include "GraphicsLog.h"
#include "GraphicsPaths.h"

#include <cstdio>
#include <cstring>

namespace gfx {
namespace {
constexpr char kTag[] = "Shader";

// Writes a multi-line info log as separate log lines ("world.frag: 0:43: ...").
void LogInfoLog(const std::string& name, const char* text) {
    if (!text || !*text) return;
    const char* p = text;
    int lines = 0;
    while (*p && lines < 40) {
        const char* nl = std::strchr(p, '\n');
        const size_t len = nl ? static_cast<size_t>(nl - p) : std::strlen(p);
        if (len > 0) GFX_LOGE(kTag, "%s: %.*s", name.c_str(), static_cast<int>(len), p);
        if (!nl) break;
        p = nl + 1;
        ++lines;
    }
}
} // namespace

bool ReadTextFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char chunk[4096];
    size_t n;
    bool ok = true;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        out.append(chunk, n);
        if (out.size() > (1u << 20)) { ok = false; break; }
    }
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
}

ShaderManager& ShaderManager::Get() {
    static ShaderManager instance;
    return instance;
}

void ShaderManager::SetBaseDir(const std::string& dir) {
    m_baseDir = dir;
    if (!m_baseDir.empty() && m_baseDir.back() != '/') m_baseDir += '/';
}

void ShaderManager::RegisterEmbedded(const std::string& name, const char* source) {
    if (source) m_embedded[name] = source;
}

bool ShaderManager::LoadShaderFile(const std::string& name, std::string& out, bool* fromFile) const {
    const std::string base = m_baseDir.empty() ? std::string(paths::kShaders) : m_baseDir;
    if (ReadTextFile(base + name, out) && !out.empty()) {
        if (fromFile) *fromFile = true;
        return true;
    }
    auto it = m_embedded.find(name);
    if (it == m_embedded.end()) {
        if (fromFile) *fromFile = false;
        return false;
    }
    out = it->second;
    if (fromFile) *fromFile = false;
    return true;
}

GLuint ShaderManager::CompileShader(GLenum type, const std::string& source, const std::string& debugName) {
    GFX_LOGI(kTag, "Loading %s", debugName.c_str());
    GLuint shader = glCreateShader(type);
    if (!shader) {
        GFX_LOGE(kTag, "%s: glCreateShader failed (0x%x)", debugName.c_str(), glGetError());
        return 0;
    }
    const char* src = source.c_str();
    const GLint len = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &src, &len);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[2048] = {};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        GFX_LOGE(kTag, "[Shader ERROR] %s", debugName.c_str());
        LogInfoLog(debugName, log);
        glDeleteShader(shader);
        return 0;
    }
    GFX_LOGI(kTag, "%s: Compile OK", debugName.c_str());
    return shader;
}

GLuint ShaderManager::CreateProgram(GLuint vs, GLuint fs, const std::string& debugName,
                                    const AttribBinding* bindings, int bindingCount) {
    if (!vs || !fs) return 0;
    GLuint program = glCreateProgram();
    if (!program) {
        GFX_LOGE(kTag, "%s: glCreateProgram failed", debugName.c_str());
        return 0;
    }
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    for (int i = 0; i < bindingCount && bindings; ++i) glBindAttribLocation(program, bindings[i].index, bindings[i].name);
    glLinkProgram(program);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    glDetachShader(program, vs);
    glDetachShader(program, fs);
    if (ok != GL_TRUE) {
        char log[2048] = {};
        glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
        GFX_LOGE(kTag, "[Shader ERROR] link %s", debugName.c_str());
        LogInfoLog(debugName, log);
        glDeleteProgram(program);
        return 0;
    }
    GFX_LOGI(kTag, "%s: Link OK (program %u)", debugName.c_str(), program);
    return program;
}

GLuint ShaderManager::GetProgram(const std::string& key, const std::string& vsFile, const std::string& fsFile,
                                 const AttribBinding* bindings, int bindingCount) {
    Entry& e = m_programs[key];
    if (e.program || e.failed) return e.program;

    std::string vsSrc, fsSrc;
    bool vsFromFile = false, fsFromFile = false;
    if (!LoadShaderFile(vsFile, vsSrc, &vsFromFile) || !LoadShaderFile(fsFile, fsSrc, &fsFromFile)) {
        GFX_LOGE(kTag, "%s: source missing (%s / %s), effect disabled", key.c_str(), vsFile.c_str(), fsFile.c_str());
        e.failed = true;
        return 0;
    }
    GFX_LOGI(kTag, "%s: %s (%s), %s (%s)", key.c_str(), vsFile.c_str(), vsFromFile ? "file" : "embedded",
             fsFile.c_str(), fsFromFile ? "file" : "embedded");
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vsSrc, vsFile);
    GLuint fs = vs ? CompileShader(GL_FRAGMENT_SHADER, fsSrc, fsFile) : 0;
    e.program = CreateProgram(vs, fs, key, bindings, bindingCount);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    e.failed = e.program == 0;
    return e.program;
}

void ShaderManager::ReloadAll() {
    for (auto& kv : m_programs)
        if (kv.second.program) glDeleteProgram(kv.second.program);
    m_programs.clear();
    ++m_generation;
    GFX_LOGI(kTag, "shader programs reloaded (generation %u)", m_generation);
}

void ShaderManager::ForgetAll() {
    m_programs.clear();
    ++m_generation;
}

} // namespace gfx
