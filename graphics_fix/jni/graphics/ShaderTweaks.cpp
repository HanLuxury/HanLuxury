#include "ShaderTweaks.h"
#include "RQShader.h"
#include "../modloader/HookScope.h"
#include "../util/patch.h"
#include <android/log.h>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>

#define ST_LOG(...) __android_log_print(ANDROID_LOG_INFO, "GfxShaderTweaks", __VA_ARGS__)

namespace ShaderTweaks {
namespace {
constexpr size_t kNativeBuffer = 8193; // _ZL8pixelSrc / _ZL9vertexSrc (libGTASA 2.10)
std::mutex g_mutex;
Vehicle g_vehicle;
bool (*BuildSource_orig)(uint32, const char**, const char**) = nullptr;

// Inserts text right after the first occurrence of anchor. False if absent.
bool InsertAfter(std::string& source, const char* anchor, const std::string& text)
{
    const size_t at = source.find(anchor);
    if (at == std::string::npos) return false;
    source.insert(at + std::char_traits<char>::length(anchor), text);
    return true;
}

std::string Format(const char* pattern, float value)
{
    char line[256];
    std::snprintf(line, sizeof(line), pattern, value);
    return line;
}

bool PatchEnvMapped(uint32 flags, const Vehicle& v, std::string& pixel, std::string& vertex)
{
    // GTA's generator: FLAG_ENVMAP wins when both bits are set.
    const bool sphere = !(flags & FLAG_ENVMAP);
    // Vertex: N.V before reflection (reflVector is still the view ray there).
    static const char* kView = "vec3 reflVector = normalize(WorldPos.xyz - CameraPosition.xyz);";
    const char* written = sphere ? "Out_Refl = reflVector;"
                                 : "Out_Tex1 = vec2(length(reflVector.xy), (reflVector.z * 0.5) + 0.25);";
    const char* vertexDecl = sphere ? "varying mediump vec3 Out_Refl;" : "varying mediump vec2 Out_Tex1;";
    if (vertex.find("vec3 WorldNormal = vec3(0.0, 0.0, 0.0);") != std::string::npos) return false;
    if (vertex.find(kView) == std::string::npos || vertex.find(written) == std::string::npos ||
        vertex.find(vertexDecl) == std::string::npos || vertex.find("WorldNormal") == std::string::npos)
        return false;
    // Pixel anchors, all required before anything is changed.
    static const char* kPixelDecl = "uniform lowp float EnvMapCoefficient;";
    const char* mix = sphere ? "fcolor.xyz = mix(fcolor.xyz,ReflTexture.xyz, EnvMapCoefficient);"
                             : "fcolor.xyz = mix(fcolor.xyz, texture2D(EnvMap, Out_Tex1).xyz, EnvMapCoefficient);";
    if (pixel.find(kPixelDecl) == std::string::npos || pixel.find(mix) == std::string::npos) return false;

    std::string vs = vertex, ps = pixel;
    InsertAfter(vs, vertexDecl, "varying lowp float Out_Fresnel;");
    InsertAfter(vs, kView, "float vehicleNdotV = abs(dot(reflVector, WorldNormal));");
    InsertAfter(vs, written, "Out_Fresnel = pow(1.0 - clamp(vehicleNdotV, 0.0, 1.0), 5.0);");
    if (v.glint > 0.0f && vs.find("uniform lowp float EnvMapCoefficient;") != std::string::npos) {
        char line[256];
        std::snprintf(line, sizeof(line),
                      "Out_Spec += pow(max(dot(reflVector, DirLightDirection), 0.0), %.1f) * EnvMapCoefficient * %.3f * DirLightDiffuseColor;",
                      v.gloss, v.glint);
        InsertAfter(vs, "Out_Spec = specAmt * DirLightDiffuseColor;", line);
    }

    InsertAfter(ps, kPixelDecl, "varying lowp float Out_Fresnel;");
    const std::string source = sphere ? "ReflTexture.xyz" : "texture2D(EnvMap, Out_Tex1).xyz";
    char line[320];
    std::snprintf(line, sizeof(line),
                  "fcolor.xyz = mix(fcolor.xyz, %s, clamp(EnvMapCoefficient * %.3f * Out_Fresnel * %.3f, 0.0, 0.65));",
                  source.c_str(), v.fresnel, v.reflection);
    InsertAfter(ps, mix, line);
    if (v.specular > 0.0f)
        InsertAfter(ps, "fcolor.xyz += Out_Spec;", Format("fcolor.xyz += Out_Spec * %.3f;", v.specular));

    vertex.swap(vs);
    pixel.swap(ps);
    return true;
}

bool BuildSource_hook(uint32 flags, const char** pixelSource, const char** vertexSource)
{
    ML_HOOK_SCOPE();
    if (!BuildSource_orig) return false;
    const bool built = BuildSource_orig(flags, pixelSource, vertexSource);
    if (!built || !pixelSource || !vertexSource || !*pixelSource || !*vertexSource) return built;
    if (!(flags & (FLAG_ENVMAP | FLAG_SPHERE_ENVMAP)) || (flags & (FLAG_SPHERE_XFORM | FLAG_WATER))) return built;
    Vehicle v;
    { std::lock_guard<std::mutex> lock(g_mutex); v = g_vehicle; }
    if (!v.enabled) return built;
    // Callers copy the strings at once (strdup); one buffer pair per thread.
    thread_local std::string pixel, vertex;
    pixel = *pixelSource;
    vertex = *vertexSource;
    if (!PatchEnvMapped(flags, v, pixel, vertex)) return built;
    // GTA's own buffers hold 8193 bytes and emu_ShaderGetCurSource strcpy()s
    // the source into buffers of that size: never hand out anything longer.
    if (pixel.size() >= kNativeBuffer || vertex.size() >= kNativeBuffer) return built;
    *pixelSource = pixel.c_str();
    *vertexSource = vertex.c_str();
    static int reported = 0;
    if (reported < 4) { ++reported; ST_LOG("car paint shader patched (flags 0x%08X)", flags); }
    return built;
}
} // namespace

void SetVehicle(const Vehicle& vehicle)
{
    Vehicle v = vehicle;
    v.fresnel = std::clamp(v.fresnel, 0.0f, 1.5f);
    v.reflection = std::clamp(v.reflection, 0.0f, 2.0f);
    v.specular = std::clamp(v.specular, 0.0f, 2.0f);
    v.glint = std::clamp(v.glint, 0.0f, 2.0f);
    v.gloss = std::clamp(v.gloss, 16.0f, 256.0f);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_vehicle = v;
}

void InstallHooks()
{
    static std::once_flag once;
    std::call_once(once, [] {
        CHook::InlineHook("_ZN8RQShader11BuildSourceEjPPKcS2_", &BuildSource_hook, &BuildSource_orig);
        ST_LOG(BuildSource_orig ? "shader tweaks installed" : "RQShader::BuildSource hook failed");
    });
}
} // namespace ShaderTweaks
