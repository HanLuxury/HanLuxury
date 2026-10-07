#include "TextureFilter.h"
#include "GraphicsSettings.h"
#include "RenderThread.h"
#include "../modloader/HookScope.h"
#include <GLES3/gl3.h>
#include <android/log.h>
#include <dlfcn.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_set>

#define TF_LOG(...) __android_log_print(ANDROID_LOG_INFO, "GfxTextureFilter", __VA_ARGS__)

namespace TextureFilter {
namespace {
constexpr GLenum kMaxAnisotropy = 0x84FE;      // GL_TEXTURE_MAX_ANISOTROPY_EXT
constexpr GLenum kMaxAnisotropyLimit = 0x84FF; // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT

using TexParameteriFn = void (GL_APIENTRYP)(GLenum, GLenum, GLint);
TexParameteriFn originalTexParameteri = nullptr;

std::atomic<int> g_support{-1};       // -1 unknown, 0 no, 1 yes
std::atomic<float> g_deviceMax{1.0f};
std::atomic<float> g_wanted{1.0f};    // requested samples (1 = off)
std::atomic<int> g_level{0};
// Render thread only: GTA textures that use mipmaps, for a level change.
std::unordered_set<GLuint> g_mipTextures;

bool IsMipmapFilter(GLint filter)
{
    return filter == GL_LINEAR_MIPMAP_LINEAR || filter == GL_LINEAR_MIPMAP_NEAREST ||
           filter == GL_NEAREST_MIPMAP_LINEAR || filter == GL_NEAREST_MIPMAP_NEAREST;
}

float Effective()
{
    return std::clamp(g_wanted.load(std::memory_order_relaxed), 1.0f,
                      g_deviceMax.load(std::memory_order_relaxed));
}

void GL_APIENTRY HookTexParameteri(GLenum target, GLenum pname, GLint param)
{
    ML_HOOK_SCOPE();
    if (!originalTexParameteri) return;
    originalTexParameteri(target, pname, param);
    if (target != GL_TEXTURE_2D || pname != GL_TEXTURE_MIN_FILTER || !IsMipmapFilter(param)) return;
    if (g_support.load(std::memory_order_relaxed) != 1) return;
    if (!GraphicsRenderThread::OnRenderThread()) return; // GTA's context only
    glTexParameterf(GL_TEXTURE_2D, kMaxAnisotropy, Effective());
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    if (bound > 0) g_mipTextures.insert(static_cast<GLuint>(bound));
}

// Render thread: apply a new level to every mipmapped texture seen so far.
void ApplyToKnownTextures(void*)
{
    if (g_support.load() != 1 || g_mipTextures.empty()) return;
    GLint active = GL_TEXTURE0, bound = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    const float value = Effective();
    for (auto it = g_mipTextures.begin(); it != g_mipTextures.end();) {
        if (!glIsTexture(*it)) { it = g_mipTextures.erase(it); continue; }
        glBindTexture(GL_TEXTURE_2D, *it);
        glTexParameterf(GL_TEXTURE_2D, kMaxAnisotropy, value);
        ++it;
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
    glActiveTexture(static_cast<GLenum>(active));
}
} // namespace

bool InstallHooks(HookBackend backend)
{
    static std::once_flag once;
    static bool installed = false;
    std::call_once(once, [&] {
        if (!backend) return;
        void* gl = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
        void* symbol = gl ? dlsym(gl, "glTexParameteri") : nullptr;
        if (!symbol) { TF_LOG("glTexParameteri not found, anisotropic filtering off"); return; }
        installed = backend(symbol, reinterpret_cast<void*>(&HookTexParameteri),
                            reinterpret_cast<void**>(&originalTexParameteri)) && originalTexParameteri;
        TF_LOG(installed ? "texture filter hook installed" : "texture filter hook failed");
    });
    return installed;
}

void CheckDevice()
{
    if (g_support.load() != -1) return;
    bool supported = false;
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count && !supported; ++i) {
        const char* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        supported = name && std::strcmp(name, "GL_EXT_texture_filter_anisotropic") == 0;
    }
    float limit = 1.0f;
    if (supported) glGetFloatv(kMaxAnisotropyLimit, &limit);
    g_deviceMax.store(std::max(1.0f, limit));
    g_support.store(supported && limit > 1.0f ? 1 : 0);
    GraphicsSettings::SetAnisotropicSupport(g_support.load() == 1);
    TF_LOG("anisotropic filtering: %s (max %.0fx)", g_support.load() == 1 ? "yes" : "no", limit);
}

void SetLevel(int level)
{
    level = std::clamp(level, 0, 4);
    static constexpr float kSamples[5] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
    g_wanted.store(kSamples[level]);
    if (g_level.exchange(level) == level) return;
    if (GraphicsRenderThread::Ready()) GraphicsRenderThread::Enqueue(&ApplyToKnownTextures, nullptr);
}
} // namespace TextureFilter
