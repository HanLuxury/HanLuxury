#pragma once
// EAGLE graphics engine - entry point.
//
//   GraphicsEngine::Get().EarlyInit()    JNI_OnLoad / InjectHooks (no GL here)
//   OnShadowPoint()                      game thread, CRealTimeShadowManager::Update hook
//   OnEndWorld()                         game thread, start of Render2dStuff (3D -> HUD)
//
// Control functions (Set*, Request*) are thread-safe: they only store a
// request that the game thread applies at the next frame, so JNI/UI threads
// never touch RenderWare or the RenderQueue.

#include "GraphicsConfig.h"
#include "ShadowManager.h"
#include "SunManager.h"
#include "TimeCycleFX.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

namespace gfx {

class GraphicsEngine {
public:
    static GraphicsEngine& Get();

    // Loads Config.ini/Advanced.ini/shaderUniform.ini, opens logOutput.log, configures
    // the shader patcher and installs the hooks. Must run before GTA compiles its shaders.
    void EarlyInit();
    void Shutdown();

    // ---- game thread
    void OnShadowPoint();
    void OnEndWorld();

    // ---- any thread
    void RequestReloadConfig();
    void RequestReloadShaders();
    void SetQuality(int quality);
    void SetEnabled(bool enabled);
    void SetShadowEnabled(bool enabled);
    void SetShadowDistance(float metres);
    void SetBloom(bool enabled, float intensity);
    void SetDebugFlag(const std::string& name, bool value);
    // Settings as they will be applied on the next frame (requests included).
    GraphicsConfig ConfigSnapshot();
    bool GetDebugFlag(const std::string& name);
    std::string StatusString();

    bool ShouldSuppressGtaShadows() const { return m_suppressGtaShadows.load(std::memory_order_relaxed); }
    bool Initialized() const { return m_initialized; }

private:
    GraphicsEngine() = default;
    void ApplyPendingRequests();
    void ApplyConfig(bool logSummary);
    void ArmReceivers();
    void UpdateAdaptiveQuality(float frameMs);
    void LogPerformance(float shadowCpuMs);

    bool m_initialized = false;
    GraphicsConfig m_config;
    std::mutex m_requestMutex;
    GraphicsConfig m_pendingConfig;     // guarded by m_requestMutex
    bool m_hasPendingConfig = false;     // guarded by m_requestMutex
    std::atomic<bool> m_reloadConfig{false};
    std::atomic<bool> m_reloadShaders{false};
    std::atomic<bool> m_suppressGtaShadows{false};

    TimeCycleFX m_timeCycle;
    SunManager m_sun;
    ShadowManager m_shadows;
    TimeOfDayState m_tod;

    // timing / adaptive quality
    std::chrono::steady_clock::time_point m_lastFrame{};
    bool m_haveLastFrame = false;
    float m_avgFrameMs = 16.6f;
    std::atomic<float> m_statusFrameMs{16.6f};   // copy readable from JNI threads
    std::atomic<float> m_statusScale{1.0f};
    float m_lowTimer = 0.0f, m_highTimer = 0.0f;
    float m_distanceScale = 1.0f;
    float m_perfTimer = 0.0f;
    int m_frames = 0;
    bool m_loggedFirstFrame = false;
};

} // namespace gfx
