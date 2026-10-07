#include "ShadowManager.h"
#include "DebugOverlay.h"
#include "FrameBuffer.h"
#include "GLCaps.h"
#include "GLStateBackup.h"
#include "GameRenderBridge.h"
#include "GraphicsLog.h"
#include "RenderQueueBridge.h"
#include "ShaderManager.h"

#include "../game/common.h"
#include "../game/Entity/Entity.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

namespace gfx {
namespace {

constexpr char kTag[] = "Shadow";

ShadowFrame g_frames[ShadowManager::kFrameSlots];

// ------------------------------------------------------------- GL thread state
struct GpuState {
    EGLContext context = EGL_NO_CONTEXT;
    DepthFrameBuffer atlas;
    LitDummyTexture dummy;
    bool hardware = false;
    int failedWidth = 0, failedHeight = 0;
    bool failedDepth24 = false;
    GLStateBackup cascadeState;
    bool cascadeOpen = false;
    bool loggedReady = false;
};
GpuState g_gpu;
std::atomic<bool> g_gpuFailed{false};

bool EnsureContext() {
    const EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return false;
    if (ctx != g_gpu.context) {
        if (g_gpu.context != EGL_NO_CONTEXT) {
            // The previous context (and every handle created in it) is gone.
            GFX_LOGW(kTag, "GL context changed (%p -> %p): GPU resources will be recreated", g_gpu.context, ctx);
            g_gpu.atlas.Forget();
            g_gpu.dummy.Forget();
            g_gpu.cascadeOpen = false;
            ShaderPatcher::ForgetAll();
            ShaderManager::Get().ForgetAll();
            DebugOverlay::ForgetGpu();
        }
        g_gpu.context = ctx;
        g_gpu.failedWidth = g_gpu.failedHeight = 0;
        g_gpuFailed.store(false);
    }
    return true;
}

bool EnsureResources(const ShadowFrame& f) {
    if (!EnsureContext()) return false;
    if (!ShaderPatcher::EnsureFeatures()) {
        if (GraphicsLog::Once("shadow-no-es3")) GFX_LOGE(kTag, "OpenGL ES 3.0 required for shadow maps: disabled");
        g_gpuFailed.store(true);
        return false;
    }
    const bool hw = ShaderPatcher::HardwareCompare();
    if (g_gpu.hardware != hw) {
        g_gpu.atlas.Destroy();
        g_gpu.dummy.Destroy();
        g_gpu.hardware = hw;
    }
    if (!g_gpu.dummy.Texture() && !g_gpu.dummy.Create(hw)) {
        g_gpuFailed.store(true);
        return false;
    }

    const DepthFrameBuffer::Desc& cur = g_gpu.atlas.Description();
    if (g_gpu.atlas.Valid() && cur.width == f.atlasWidth && cur.height == f.atlasHeight && cur.depth24 == f.depth24)
        return true;
    if (f.atlasWidth == g_gpu.failedWidth && f.atlasHeight == g_gpu.failedHeight && f.depth24 == g_gpu.failedDepth24)
        return false; // same request already failed, wait for a settings change

    const GLCaps& caps = GLCaps::Get();
    const int limit = std::min({caps.maxTextureSize, caps.maxRenderbufferSize > 0 ? caps.maxRenderbufferSize : 1 << 14,
                                caps.maxViewportDims[0] > 0 ? caps.maxViewportDims[0] : 1 << 14});
    if (f.atlasWidth > limit || f.atlasHeight > limit) {
        GFX_LOGE(kTag, "shadow atlas %dx%d exceeds GPU limit %d: lower [shadow] resolution", f.atlasWidth,
                 f.atlasHeight, limit);
        g_gpu.failedWidth = f.atlasWidth;
        g_gpu.failedHeight = f.atlasHeight;
        g_gpu.failedDepth24 = f.depth24;
        return false;
    }

    DepthFrameBuffer::Desc desc;
    desc.width = f.atlasWidth;
    desc.height = f.atlasHeight;
    desc.depth24 = f.depth24;
    desc.hardwareCompare = hw;
    if (!g_gpu.atlas.Create(desc)) {
        g_gpu.failedWidth = f.atlasWidth;
        g_gpu.failedHeight = f.atlasHeight;
        g_gpu.failedDepth24 = f.depth24;
        return false;
    }
    GFX_LOGI(kTag, "Shadow resolution: %d per cascade, atlas %dx%d, Shadow cascades: %d", f.viewports[0][2],
             f.atlasWidth, f.atlasHeight, f.count);
    return true;
}

void BindShadowUnit(GLuint texture) {
    GLint active = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0 + ShaderPatcher::kShadowTextureUnit);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindSampler(ShaderPatcher::kShadowTextureUnit, 0);
    glActiveTexture(static_cast<GLenum>(active));
}

void CloseCascade() {
    if (!g_gpu.cascadeOpen) return;
    g_gpu.cascadeState.Restore();
    g_gpu.cascadeOpen = false;
}

} // namespace

// ================================================================ GL thread

namespace ShadowGpu {

void BeginShadowPass(void* arg) {
    auto* f = static_cast<ShadowFrame*>(arg);
    f->cleared = false;
    f->gpuReady = EnsureResources(*f);
    if (f->gpuReady && !g_gpu.loggedReady) {
        g_gpu.loggedReady = true;
        GFX_LOGI(kTag, "first shadow pass on the GL thread (receivers: %d)", ShaderPatcher::ReceiverCount());
    }
    // Receivers must not sample the atlas while it is the depth attachment.
    ShaderPatcher::SetMode(ShaderPatcher::Mode::Caster, &f->casterUniforms);
    BindShadowUnit(g_gpu.dummy.Texture());
}

void BeginCascade(void* arg) {
    auto* task = static_cast<CascadeTask*>(arg);
    ShadowFrame* f = task->frame;
    if (!g_gpu.cascadeOpen) {
        // First cascade: take over from the RQ target selected by RwCameraBeginUpdate.
        g_gpu.cascadeState.Save(GLStateBackup::kFramebuffer | GLStateBackup::kViewport | GLStateBackup::kDepth |
                                GLStateBackup::kRaster);
        g_gpu.cascadeOpen = true;
        if (!f->gpuReady || !g_gpu.atlas.Valid()) {
            // The draws of this pass are already queued: make them harmless.
            glEnable(GL_RASTERIZER_DISCARD);
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, g_gpu.atlas.Fbo());
        glDepthRangef(0.0f, 1.0f);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(f->slopeBias, f->slopeUnits);
    } else if (!f->gpuReady || !g_gpu.atlas.Valid()) {
        return; // rasterizer discard is still on
    }
    if (!f->cleared) {
        GLboolean depthMask = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, f->atlasWidth, f->atlasHeight);
        glDepthMask(GL_TRUE);
        glClearDepthf(1.0f);
        glClear(GL_DEPTH_BUFFER_BIT);
        glDepthMask(depthMask); // RQ owns the depth mask during the caster draws
        f->cleared = true;
    }
    const int* vp = f->viewports[task->index];
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glScissor(vp[0], vp[1], vp[2], vp[3]);
    glEnable(GL_SCISSOR_TEST);
}

void EndCascade(void*) { CloseCascade(); }

void EndShadowPass(void* arg) {
    auto* f = static_cast<ShadowFrame*>(arg);
    CloseCascade();
    if (f->gpuReady && g_gpu.atlas.Valid()) {
        ShaderPatcher::SetMode(ShaderPatcher::Mode::Receive, &f->uniforms);
        BindShadowUnit(g_gpu.atlas.Texture());
    } else {
        ShaderPatcher::SetMode(ShaderPatcher::Mode::Disabled, nullptr);
        BindShadowUnit(g_gpu.dummy.Texture());
    }
}

void EndWorld(void* debugOverlay) {
    CloseCascade();
    if (!EnsureContext()) return;
    ShaderPatcher::SetMode(ShaderPatcher::Mode::Disabled, nullptr);
    if (g_gpu.dummy.Texture()) BindShadowUnit(g_gpu.dummy.Texture());
    if (debugOverlay && g_gpu.atlas.Valid())
        DebugOverlay::DrawDepthTexture(g_gpu.atlas.Texture(), g_gpu.atlas.Width(), g_gpu.atlas.Height());
}

void Release(void*) {
    CloseCascade();
    if (eglGetCurrentContext() == g_gpu.context && g_gpu.context != EGL_NO_CONTEXT) {
        ShaderPatcher::SetMode(ShaderPatcher::Mode::Disabled, nullptr);
        BindShadowUnit(0);
        g_gpu.atlas.Destroy();
        g_gpu.dummy.Destroy();
        DebugOverlay::ReleaseGpu();
        GFX_LOGI(kTag, "shadow GPU resources released");
    }
}

GLuint AtlasTexture() { return g_gpu.atlas.Texture(); }
bool Failed() { return g_gpuFailed.load(); }
void ResetFailure() { g_gpuFailed.store(false); }

} // namespace ShadowGpu

// ============================================================== game thread

bool ShadowManager::Init() {
    if (m_lightCamera) return true;
    RwCamera* cam = RwCameraCreate();
    if (!cam) {
        GFX_LOGE(kTag, "RwCameraCreate failed");
        return false;
    }
    RwFrame* frame = RwFrameCreate();
    if (!frame) {
        GFX_LOGE(kTag, "RwFrameCreate failed");
        RwCameraDestroy(cam);
        return false;
    }
    rwObjectHasFrameSetFrame(cam, frame);
    RwCameraSetProjection(cam, rwPARALLEL);
    cam->frameBuffer = nullptr;
    cam->zBuffer = nullptr;
    m_lightCamera = cam;
    GFX_LOGI(kTag, "light camera created (parallel RwCamera %p)", static_cast<void*>(cam));
    return true;
}

void ShadowManager::Shutdown() {
    if (m_receiversActive) {
        RenderQueueBridge::Enqueue(&ShadowGpu::EndWorld, nullptr);
        m_receiversActive = false;
    }
    RenderQueueBridge::Enqueue(&ShadowGpu::Release, nullptr);
    if (m_lightCamera) {
        RwCamera* cam = m_lightCamera;
        m_lightCamera = nullptr;
        cam->frameBuffer = nullptr;
        cam->zBuffer = nullptr;
        RwFrame* frame = RwCameraGetFrame(cam);
        if (frame) {
            rwObjectHasFrameSetFrame(cam, nullptr);
            RwFrameDestroy(frame);
        }
        RwCameraDestroy(cam);
    }
}

void ShadowManager::ApplySettings(const ShadowSettings& s) {
    ShadowSettings next = s;
    next.distance = std::max(20.0f, s.distance * m_distanceScale);
    const bool layoutChanged = !m_settingsValid || next.cascades != m_settings.cascades ||
                               next.resolution != m_settings.resolution || next.distance != m_settings.distance ||
                               next.splitLambda != m_settings.splitLambda ||
                               next.casterExtend != m_settings.casterExtend || next.depthBias != m_settings.depthBias;
    const bool capacityChanged = !m_settingsValid || next.maxCasters != m_settings.maxCasters;
    m_settings = next;
    if (layoutChanged)
        m_csm.Configure(m_settings.cascades, m_settings.resolution, m_settings.distance, m_settings.splitLambda,
                        m_settings.casterExtend, m_settings.depthBias);
    if (capacityChanged) m_casters.Reserve(m_settings.maxCasters);
    if (layoutChanged && m_settingsValid)
        GFX_LOGI(kTag, "cascades=%d resolution=%d distance=%.0fm (atlas %dx%d)", m_csm.Count(), m_csm.Resolution(),
                 m_csm.Distance(), m_csm.AtlasWidth(), m_csm.AtlasHeight());
    m_settingsValid = true;
}

bool ShadowManager::UpdateLightMatrices(const CameraSnapshot& camera, const Vec3& lightDirection) {
    CameraFrustum f;
    f.pos = camera.pos;
    f.right = camera.right;
    f.up = camera.up;
    f.at = camera.at;
    f.tanX = camera.viewWindowX;
    f.tanY = camera.viewWindowY;
    f.nearPlane = camera.nearPlane;
    return m_csm.UpdateLightMatrices(f, lightDirection);
}

void ShadowManager::SetupLightCamera(const Cascade& c) {
    RwCamera* cam = m_lightCamera;
    RwFrame* frame = RwCameraGetFrame(cam);
    if (!frame) return;
    RwMatrix* m = &frame->modelling;
    m->right = RwV3d{c.lightRight.x, c.lightRight.y, c.lightRight.z};
    m->up = RwV3d{c.lightUp.x, c.lightUp.y, c.lightUp.z};
    m->at = RwV3d{c.lightAt.x, c.lightAt.y, c.lightAt.z};
    m->pos = RwV3d{c.lightPos.x, c.lightPos.y, c.lightPos.z};
    RwMatrixUpdate(m);
    RwFrameUpdateObjects(frame);
    const RwV2d window{c.viewWindow, c.viewWindow};
    RwCameraSetViewWindow(cam, &window);
    RwCameraSetNearClipPlane(cam, c.nearPlane);
    RwCameraSetFarClipPlane(cam, c.farPlane);
}

void ShadowManager::UploadCascadeUniforms(ShadowFrame& frame, const SunLight& sun, const TimeOfDayState& tod,
                                          const GraphicsConfig& config) {
    const ShadowSettings& s = m_settings;
    ShadowUniforms& u = frame.uniforms;
    const int count = m_csm.Count();
    const float lastFar = m_csm.Get(count - 1).splitFar;
    for (int i = 0; i < kMaxCascades; ++i) {
        // Unused slots repeat the last cascade so blending into them is a no-op.
        const Cascade& c = m_csm.Get(std::min(i, count - 1));
        std::memcpy(u.vp[i], c.atlas.m, sizeof(u.vp[i]));
        std::memcpy(u.tile[i], c.tileRect, sizeof(u.tile[i]));
        u.split[i] = i < count ? c.splitFar : lastFar + 1.0f;
        u.dbias[i] = c.depthBias;
        u.texel[i] = c.texelWorld;
    }
    const float fadeEnd = lastFar;
    const float fadeStart = fadeEnd * (1.0f - s.fadeRange);
    u.cfg[0] = 1.0f / static_cast<float>(m_csm.AtlasWidth());
    u.cfg[1] = 1.0f / static_cast<float>(m_csm.AtlasHeight());
    u.cfg[2] = fadeStart;
    u.cfg[3] = fadeEnd;
    u.bias[0] = s.normalBias;
    u.bias[1] = s.cascadeBlend ? std::max(s.blendBand, 0.001f) : 0.001f;
    u.bias[2] = s.pcfSpread;
    u.bias[3] = s.alphaCutoff;
    u.light[0] = sun.toSun.x;
    u.light[1] = sun.toSun.y;
    u.light[2] = sun.toSun.z;
    u.light[3] = sun.shadowStrength;
    const Vec3 tint = s.tintOverride ? Vec3{s.tint[0], s.tint[1], s.tint[2]} : tod.look.shadowTint;
    u.tint[0] = tint.x;
    u.tint[1] = tint.y;
    u.tint[2] = tint.z;
    u.tint[3] = s.prelitDirectShare;
    const float boost = s.sunBoost * tod.look.sunBoost * sun.intensity;
    u.boost[0] = sun.color.x * boost;
    u.boost[1] = sun.color.y * boost;
    u.boost[2] = sun.color.z * boost;
    u.boost[3] = config.debug.showCascade ? 1.0f : 0.0f;

    frame.casterUniforms = u;
    frame.casterUniforms.light[3] = -1.0f;
}

void ShadowManager::RenderShadowCasters(ShadowFrame& frame) {
    // One RwCameraBeginUpdate per cascade but a single EndUpdate at the end:
    // _rwOpenGLCameraBeginUpdate (0x23EDF4) skips RQRenderTarget::Select when
    // the camera is already current and only reloads the projection/view, so
    // the atlas FBO stays bound for all cascades = one render pass on tilers.
    RwCamera* cam = m_lightCamera;
    void* weaponMark = GameRenderBridge::WeaponPedListMark();
    bool begun = false;
    bool open = false;
    for (int i = 0; i < m_csm.Count(); ++i) {
        SetupLightCamera(m_csm.Get(i));
        if (!RwCameraBeginUpdate(cam)) break;
        begun = true;
        // Every cascade is opened (even without casters) so the atlas is always cleared.
        if (!RenderQueueBridge::Enqueue(&ShadowGpu::BeginCascade, &frame.tasks[i])) break;
        open = true;
        GameRenderBridge::DefinedState();
        RwRenderStateSet(rwRENDERSTATEZTESTENABLE, RWRSTATE(TRUE));
        RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, RWRSTATE(TRUE));
        RwRenderStateSet(rwRENDERSTATEFOGENABLE, RWRSTATE(TRUE)); // same (patched) programs as the main pass
        RwRenderStateSet(rwRENDERSTATECULLMODE, RWRSTATE(rwCULLMODECULLNONE));
        for (CEntity* entity : m_casters.List(i)) GameRenderBridge::RenderEntity(entity);
        // Weapons in ped hands are drawn by RenderWeaponPedsForPC, not by CPed::Render.
        if (m_settings.weapons) GameRenderBridge::RenderWeaponPeds();
        // Remove the ms_weaponPedsForPC entries this cascade added (main pass owns the list).
        GameRenderBridge::TrimWeaponPedList(weaponMark);
    }
    // Restore the RQ target before EndUpdate re-selects the default target.
    if (open) RenderQueueBridge::Enqueue(&ShadowGpu::EndCascade, &frame);
    if (begun) RwCameraEndUpdate(cam);
}

bool ShadowManager::RenderShadowPass(const SunLight& sun, const TimeOfDayState& tod, const CameraSnapshot& camera,
                                     const GraphicsConfig& config) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!sun.castsShadows || !camera.valid || ShadowGpu::Failed()) return false;
    if (!GameRenderBridge::CanRenderEntities() || !RenderQueueBridge::IsReady()) return false;
    if (!m_lightCamera && !Init()) return false;

    ApplySettings(config.shadow);
    m_csm.CalculateCascadeSplits(camera.nearPlane);
    const bool frozen = config.debug.freezeShadowCamera && m_csm.Valid();
    if (!frozen && !UpdateLightMatrices(camera, sun.direction)) return false;
    if (!m_csm.Valid()) return false;

    m_casters.Collect(m_csm, m_settings, camera.pos);

    ShadowFrame& frame = g_frames[m_slot];
    m_slot = (m_slot + 1) % kFrameSlots;
    frame.atlasWidth = m_csm.AtlasWidth();
    frame.atlasHeight = m_csm.AtlasHeight();
    frame.depth24 = m_settings.depth24;
    frame.count = m_csm.Count();
    frame.slopeBias = m_settings.slopeBias;
    frame.slopeUnits = m_settings.slopeUnits;
    for (int i = 0; i < kMaxCascades; ++i) {
        frame.tasks[i].frame = &frame;
        frame.tasks[i].index = i;
        const Cascade& c = m_csm.Get(std::min(i, frame.count - 1));
        std::memcpy(frame.viewports[i], c.viewport, sizeof(frame.viewports[i]));
    }
    UploadCascadeUniforms(frame, sun, tod, config);

    // The light camera renders into the main raster's target; the RQ callback
    // redirects the draws to the atlas FBO.
    m_lightCamera->frameBuffer = camera.frameBuffer;
    m_lightCamera->zBuffer = camera.zBuffer;

    if (!RenderQueueBridge::Enqueue(&ShadowGpu::BeginShadowPass, &frame)) return false;
    RenderShadowCasters(frame);
    RenderQueueBridge::Enqueue(&ShadowGpu::EndShadowPass, &frame);
    m_receiversActive = true;

    m_lastCpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

void ShadowManager::OnEndWorld(const GraphicsConfig& config) {
    if (!RenderQueueBridge::IsReady()) return;
    const bool overlay = config.debug.showShadowMap;
    if (!m_receiversActive && !overlay) return;
    static int overlayFlag = 1;
    RenderQueueBridge::Enqueue(&ShadowGpu::EndWorld, overlay ? &overlayFlag : nullptr);
    m_receiversActive = false;
}

} // namespace gfx
