#include "WorldMsaa.h"
#include "GraphicsSettings.h"
#include "RenderThread.h"
#include "../modloader/HookScope.h"
#include "../util/patch.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <dlfcn.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#define MSAA_LOG(...) __android_log_print(ANDROID_LOG_INFO, "GfxWorldMsaa", __VA_ARGS__)

namespace WorldMsaa {
namespace {
constexpr GLenum kMaxSamplesExt = 0x8D57; // GL_MAX_SAMPLES_EXT

using RenderbufferStorageFn = void (GL_APIENTRYP)(GLenum, GLenum, GLsizei, GLsizei);
using FramebufferTexture2DFn = void (GL_APIENTRYP)(GLenum, GLenum, GLenum, GLuint, GLint);
using RenderbufferStorageMsFn = void (GL_APIENTRYP)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
using FramebufferTexture2DMsFn = void (GL_APIENTRYP)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
RenderbufferStorageFn originalRenderbufferStorage = nullptr;
FramebufferTexture2DFn originalFramebufferTexture2D = nullptr;
RenderbufferStorageMsFn renderbufferStorageMs = nullptr;
FramebufferTexture2DMsFn framebufferTexture2DMs = nullptr;
void (*originalSetAltRenderTarget)(int, int) = nullptr;

std::atomic<int> g_support{-1};     // -1 unknown, 0 no, 1 yes
std::atomic<int> g_maxSamples{0};
std::atomic<int> g_wanted{0};       // 0, 2 or 4 (game thread request)
std::atomic<int> g_creating{0};     // samples while rqTargetCreate runs (render thread)

// Render thread only, between BeginCreate and EndCreate.
struct Storage { GLuint renderbuffer; GLenum format; GLsizei width, height; };
std::vector<Storage> g_multisampled;
bool g_colourAttached = false;

bool Active()
{
    return g_creating.load(std::memory_order_relaxed) > 1 && GraphicsRenderThread::OnRenderThread();
}

void RevertRenderbuffers()
{
    if (g_multisampled.empty() || !originalRenderbufferStorage) return;
    GLint previous = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous);
    for (const Storage& s : g_multisampled) {
        glBindRenderbuffer(GL_RENDERBUFFER, s.renderbuffer);
        originalRenderbufferStorage(GL_RENDERBUFFER, s.format, s.width, s.height);
    }
    glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previous));
    g_multisampled.clear();
}

void Disable(const char* why)
{
    g_support.store(0);
    GraphicsSettings::SetMsaaSupport(false);
    MSAA_LOG("MSAA off for this device: %s", why);
}

void GL_APIENTRY HookRenderbufferStorage(GLenum target, GLenum format, GLsizei width, GLsizei height)
{
    ML_HOOK_SCOPE();
    if (!originalRenderbufferStorage) return;
    if (target == GL_RENDERBUFFER && Active() && renderbufferStorageMs) {
        GLint renderbuffer = 0;
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
        renderbufferStorageMs(target, g_creating.load(), format, width, height);
        g_multisampled.push_back({static_cast<GLuint>(renderbuffer), format, width, height});
        return;
    }
    originalRenderbufferStorage(target, format, width, height);
}

void GL_APIENTRY HookFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
    ML_HOOK_SCOPE();
    if (!originalFramebufferTexture2D) return;
    if (Active() && framebufferTexture2DMs && attachment == GL_COLOR_ATTACHMENT0 &&
        textarget == GL_TEXTURE_2D && level == 0 && texture != 0) {
        framebufferTexture2DMs(target, attachment, textarget, texture, level, g_creating.load());
        if (glCheckFramebufferStatus(target) == GL_FRAMEBUFFER_COMPLETE) {
            g_colourAttached = true;
            return;
        }
        // Back to exactly what GTA asked for.
        originalFramebufferTexture2D(target, attachment, textarget, texture, level);
        RevertRenderbuffers();
        Disable("world target incomplete with multisampling");
        return;
    }
    originalFramebufferTexture2D(target, attachment, textarget, texture, level);
}

void BeginCreate(void* samples)
{
    g_multisampled.clear();
    g_colourAttached = false;
    g_creating.store(g_support.load() == 1 ? static_cast<int>(reinterpret_cast<intptr_t>(samples)) : 0);
}

void EndCreate(void*)
{
    // Multisampled depth without a multisampled colour texture (a target with
    // a colour renderbuffer): keep everything single-sampled.
    if (!g_colourAttached) RevertRenderbuffers();
    else MSAA_LOG("world target created with %dx MSAA", g_creating.load());
    g_multisampled.clear();
    g_colourAttached = false;
    g_creating.store(0);
}

void SetAltRenderTarget_hook(int width, int height)
{
    ML_HOOK_SCOPE();
    if (!originalSetAltRenderTarget) return;
    static int applied = 0;
    static bool haveTarget = false;
    // Until CheckDevice ran (support unknown) the target stays single-sampled;
    // once MSAA is known to work, wanted != applied recreates it once.
    const int wanted = g_support.load() == 1 ? g_wanted.load() : 0;
    if (width > 0 && height > 0 && haveTarget && wanted != applied) {
        originalSetAltRenderTarget(0, 0); // GTA's own delete path (screen fade)
        haveTarget = false;
    }
    bool armed = false;
    if (wanted > 1 && width > 0 && height > 0 && !haveTarget && GraphicsRenderThread::Ready())
        armed = GraphicsRenderThread::Enqueue(&BeginCreate, reinterpret_cast<void*>(static_cast<intptr_t>(wanted)));
    originalSetAltRenderTarget(width, height);
    if (armed && !GraphicsRenderThread::Enqueue(&EndCreate, nullptr))
        GraphicsRenderThread::Enqueue(&EndCreate, nullptr);
    if (width > 0 && height > 0) {
        if (!haveTarget) applied = armed ? wanted : 0;
        haveTarget = true;
    } else {
        haveTarget = false;
    }
}
} // namespace

bool InstallHooks(HookBackend backend)
{
    static std::once_flag once;
    static bool installed = false;
    std::call_once(once, [&] {
        if (!backend) return;
        void* gl = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
        void* storage = gl ? dlsym(gl, "glRenderbufferStorage") : nullptr;
        void* attach = gl ? dlsym(gl, "glFramebufferTexture2D") : nullptr;
        if (!storage || !attach || storage == attach) { MSAA_LOG("GL symbols missing, MSAA off"); return; }
        if (!backend(storage, reinterpret_cast<void*>(&HookRenderbufferStorage),
                     reinterpret_cast<void**>(&originalRenderbufferStorage)) || !originalRenderbufferStorage) {
            MSAA_LOG("glRenderbufferStorage hook failed, MSAA off");
            return;
        }
        if (!backend(attach, reinterpret_cast<void*>(&HookFramebufferTexture2D),
                     reinterpret_cast<void**>(&originalFramebufferTexture2D)) || !originalFramebufferTexture2D) {
            MSAA_LOG("glFramebufferTexture2D hook failed, MSAA off");
            return;
        }
        CHook::InlineHook("_Z22emu_SetAltRenderTargetii", &SetAltRenderTarget_hook, &originalSetAltRenderTarget);
        installed = originalSetAltRenderTarget != nullptr;
        MSAA_LOG(installed ? "world MSAA hooks installed" : "emu_SetAltRenderTarget hook failed, MSAA off");
    });
    return installed;
}

void CheckDevice()
{
    if (g_support.load() != -1) return;
    bool extension = false;
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count && !extension; ++i) {
        const char* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        extension = name && std::strcmp(name, "GL_EXT_multisampled_render_to_texture") == 0;
    }
    if (extension) {
        renderbufferStorageMs = reinterpret_cast<RenderbufferStorageMsFn>(
            eglGetProcAddress("glRenderbufferStorageMultisampleEXT"));
        framebufferTexture2DMs = reinterpret_cast<FramebufferTexture2DMsFn>(
            eglGetProcAddress("glFramebufferTexture2DMultisampleEXT"));
    }
    GLint maxSamples = 0;
    if (extension) glGetIntegerv(kMaxSamplesExt, &maxSamples);
    const bool ok = extension && renderbufferStorageMs && framebufferTexture2DMs && maxSamples >= 2 &&
                    originalSetAltRenderTarget;
    g_maxSamples.store(maxSamples);
    g_support.store(ok ? 1 : 0);
    GraphicsSettings::SetMsaaSupport(ok);
    MSAA_LOG("EXT_multisampled_render_to_texture: %s (max %d samples)", ok ? "yes" : "no", maxSamples);
}

void SetLevel(int level)
{
    level = std::clamp(level, 0, 2);
    int samples = level == 0 ? 0 : (level == 1 ? 2 : 4);
    const int limit = g_maxSamples.load();
    if (limit > 0) samples = std::min(samples, limit);
    g_wanted.store(samples >= 2 ? samples : 0);
}
} // namespace WorldMsaa
