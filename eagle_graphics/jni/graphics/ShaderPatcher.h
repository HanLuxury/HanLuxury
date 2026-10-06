#pragma once
// EAGLE graphics engine - shadow receiver integration into GTA's own shaders.
//
// GTA SA 2.10 generates every world shader at runtime (RQShader::BuildSource)
// and compiles it on the RenderQueue thread in ES2Shader::Build(ps, vs). This
// client keeps the native generator (see game/hooks.cpp); the engine hooks
// ES2Shader::Build instead and injects the receiver code into the generated
// text:
//   VS: world position, view depth, world normal (+ direct sun term for lit
//       shaders) as varyings.
//   PS: the receiver snippet before main(), SG_InitUser(); SG_Apply(fcolor);
//       before spec/fog. The snippet is glShader/Entity/Building.shader,
//       Vehicle.shader or Character.shader (with their #includes), chosen per
//       shader by Classify().
// Only 3D world shaders are touched (they carry Out_FogAmt and ViewPos);
// HUD/2D/sphere-map shaders are left alone.
//
// Fail-safe: if the patched program fails to compile/link, the native sources
// are compiled instead (the game never sees a failure). Every patched source is
// also checked against the 520-byte statement buffer of ES2Shader::CheckCompile
// (libGTASA 0x2616A0), which would overflow on a failing long statement.
//
// Uniforms of the patched programs are uploaded lazily on the GL thread when
// rqSelectShader makes the program current (RenderQueueBridge observer).

#include <GLES3/gl3.h>
#include <string>

namespace gfx {

struct ShadowUniforms {
    float vp[4][16];     // SG_VP     world -> atlas (u, v, depth)
    float tile[4][4];    // SG_Tile   u0 v0 u1 v1 per cascade
    float split[4];      // SG_Split  far view depth of each cascade
    float cfg[4];        // SG_Cfg    1/atlasW, 1/atlasH, fade start, fade end
    float bias[4];       // SG_Bias   normal offset (texels), blend band, pcf spread, caster alpha cutoff
    float dbias[4];      // SG_DBias  normalized depth bias per cascade
    float texel[4];      // SG_Texel  metres per texel per cascade
    float light[4];      // SG_Light  direction to sun + strength
    float tint[4];       // SG_Tint   shadow colour + prelit direct share
    float boost[4];      // SG_Boost  sun boost colour + debug cascade flag
};

struct PatchOptions {
    bool hardwareCompare = false; // sampler2DShadow via GL_EXT_shadow_samplers
    int taps = 9;                 // 1, 4, 9
    bool blend = true;
    bool water = false;
};

enum class ReceiverEntity : int { Building = 0, Vehicle = 1, Character = 2 };

// Sanitized receiver snippets (GlShader::BuildReceiver output), one per entity class.
struct ReceiverSnippets {
    std::string snippet[3];
    const std::string& For(ReceiverEntity e) const { return snippet[static_cast<int>(e)]; }
};

struct PatchInfo {
    ReceiverEntity entity = ReceiverEntity::Building;
    bool lit = false;
    bool alpha = false;
    bool hardwareCompare = false;
    int taps = 9;
};

class ShaderPatcher {
public:
    static constexpr int kShadowTextureUnit = 7; // GTA uses units 0-2 (+5 for uploads), never 7
    enum class Mode { Disabled, Caster, Receive };
    using BuildFn = bool (*)(void* es2Shader, const char* ps, const char* vs);

    // Game thread (startup/reload) or the Java thread (settings request); thread-safe.
    // Each snippet must pass ValidateSnippet(); an empty one leaves that entity unpatched.
    static void Configure(bool enabled, bool allowHardwareCompare, int pcfLevel, bool blend, bool water,
                          const ReceiverSnippets& snippets);
    static bool Enabled();

    // Pure text transformation (host-testable). Returns false if the shader is
    // not a world shader or cannot be patched safely.
    static bool PatchSources(const std::string& ps, const std::string& vs, const ReceiverSnippets& snippets,
                             const PatchOptions& options, std::string& outPs, std::string& outVs, PatchInfo& info);
    // Skinned -> Character, lit with specular -> Vehicle, everything else -> Building.
    static ReceiverEntity Classify(const std::string& ps, const std::string& vs);
    static const char* EntityName(ReceiverEntity e);
    // Contract of a receiver snippet (after SanitizeSnippet): SG_Apply, SG_SpecVis,
    // SG_InitUser and every statement below the CheckCompile buffer limit.
    static bool ValidateSnippet(const std::string& clean, std::string& why);
    static std::string SanitizeSnippet(const std::string& text);
    static size_t MaxStatementLength(const std::string& source);

    // ES2Shader layout (libGTASA DWARF): GLuint fullProgram at +0x3EC. The
    // project's graphics/RQShader.h struct is static_asserted against it in
    // GraphicsHooks.cpp.
    static constexpr size_t kEs2ShaderProgramOffset = 0x3EC;
    // &ES2Shader::activeShader, resolved by GameRenderBridge.
    static void SetActiveShaderSlot(void** slot);

    // ---- GL thread
    static bool OnBuild(void* es2Shader, const char* ps, const char* vs, BuildFn original);
    static void OnSelect(void* es2Shader);
    static void SetMode(Mode mode, const ShadowUniforms* uniforms);
    static bool EnsureFeatures();           // GL thread: detects caps once, returns false without ES 3.0
    static bool HardwareCompare();          // decided at the first shader build (EnsureFeatures)
    static bool FeaturesResolved();
    static int ReceiverCount();
    static void ForgetAll();                // context loss
};

} // namespace gfx
