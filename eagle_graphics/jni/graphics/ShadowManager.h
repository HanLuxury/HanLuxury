#pragma once
// EAGLE graphics engine - real-time directional shadow pass.
//
// Frame flow (all GTA calls on the game thread, all GL on the RQ thread):
//
//   Idle()
//    ...CRenderer::ConstructRenderList / PreRender / ProcessPedsAfterPreRender
//    CRealTimeShadowManager::Update()      <- hook: ShadowManager::RenderShadowPass
//       [RQ] BeginShadowPass   : create/validate atlas, receivers -> caster mode,
//                                dummy "lit" texture on unit 7 (no feedback loop)
//       for each cascade:
//          RwCameraBeginUpdate(lightCam)    (GTA uploads light ViewMatrix/ProjMatrix)
//          [RQ] BeginCascade   : 1st: bind atlas FBO, clear, slope bias; all: tile viewport/scissor
//          CRenderer::RenderOneNonRoad(caster) for every caster (GTA's own shaders:
//                                skinning, alpha test, vertex formats all native)
//          CVisibilityPlugins::RenderWeaponPedsForPC + trim of ms_weaponPedsForPC
//       [RQ] EndCascade        : restore FBO/viewport/raster state exactly
//       RwCameraEndUpdate(lightCam)      (single EndUpdate: one render pass for the atlas)
//       [RQ] EndShadowPass     : receivers -> receive mode (matrices), atlas on unit 7
//    DoRWStuffStartOfFrame -> RenderScene (world receives shadows) -> RenderEffects
//    Render2dStuff()                       <- ShadowManager::OnEndWorld
//       [RQ] EndWorld          : receivers off, dummy on unit 7 (HUD never shadowed)
//
// The light RwCamera shares the main camera raster, so RwCameraBeginUpdate
// only re-selects the main target; the caster pass is redirected to the
// atlas FBO by the BeginCascade callback and the binding is restored by
// EndCascade before RwCameraEndUpdate, keeping RQ's target cache coherent.

#include "CascadeShadow.h"
#include "GraphicsConfig.h"
#include "ShaderPatcher.h"
#include "ShadowCasters.h"
#include "SunManager.h"
#include "TimeCycleFX.h"

#include <cstdint>

struct RwCamera;

namespace gfx {

struct CameraSnapshot;
struct ShadowFrame;

class ShadowManager {
public:
    static constexpr int kFrameSlots = 3;

    bool Init();      // game thread: light RwCamera
    void Shutdown();  // game thread: releases the light camera, schedules GPU release
    void ApplySettings(const ShadowSettings& settings);

    // Game thread, from the CRealTimeShadowManager::Update hook.
    bool RenderShadowPass(const SunLight& sun, const TimeOfDayState& tod, const CameraSnapshot& camera,
                          const GraphicsConfig& config);
    // Game thread, at the start of Render2dStuff (3D -> HUD boundary).
    void OnEndWorld(const GraphicsConfig& config);

    // CSM steps (public for debugging / JNI status).
    void CalculateCascadeSplits(float cameraNear) { m_csm.CalculateCascadeSplits(cameraNear); }
    bool UpdateLightMatrices(const CameraSnapshot& camera, const Vec3& lightDirection);
    void RenderShadowCasters(ShadowFrame& frame);
    void UploadCascadeUniforms(ShadowFrame& frame, const SunLight& sun, const TimeOfDayState& tod,
                               const GraphicsConfig& config);

    bool ReceiversActive() const { return m_receiversActive; }
    const CascadeShadow& Cascades() const { return m_csm; }
    const CasterStats& Stats() const { return m_casters.Stats(); }
    float LastCpuMs() const { return m_lastCpuMs; }
    void SetDistanceScale(float scale) { m_distanceScale = scale; }

private:
    void SetupLightCamera(const Cascade& cascade);

    RwCamera* m_lightCamera = nullptr;
    CascadeShadow m_csm;
    ShadowCasterCollector m_casters;
    ShadowSettings m_settings;
    bool m_settingsValid = false;
    bool m_receiversActive = false;
    int m_slot = 0;
    float m_distanceScale = 1.0f;
    float m_lastCpuMs = 0.0f;
};

// ---- RQ (GL) thread side -------------------------------------------------

struct CascadeTask {
    ShadowFrame* frame = nullptr;
    int index = 0;
};

struct ShadowFrame {
    // Atlas description (decides re-creation on the GL thread).
    int atlasWidth = 0, atlasHeight = 0;
    bool depth24 = false;
    int count = 0;
    int viewports[kMaxCascades][4] = {};
    float slopeBias = 1.6f, slopeUnits = 4.0f;
    ShadowUniforms uniforms{};
    ShadowUniforms casterUniforms{}; // only bias.w (alpha cutoff) is used in caster mode
    CascadeTask tasks[kMaxCascades];
    bool debugOverlay = false;
    // Written on the GL thread during the pass.
    bool gpuReady = false;
    bool cleared = false;
};

namespace ShadowGpu {
void BeginShadowPass(void* frame);
void BeginCascade(void* task);
void EndCascade(void* task);
void EndShadowPass(void* frame);
void EndWorld(void* debugOverlay); // arg != nullptr -> draw the atlas preview
void Release(void*);
GLuint AtlasTexture();
bool Failed();
void ResetFailure();
} // namespace ShadowGpu

} // namespace gfx
