#include "GraphicsEngine.h"
#include "EmbeddedShaders.h"
#include "GameRenderBridge.h"
#include "GraphicsHooks.h"
#include "GraphicsLog.h"
#include "GraphicsPaths.h"
#include "RenderQueueBridge.h"
#include "ShaderManager.h"
#include "ShaderPatcher.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gfx {
namespace {

constexpr char kTag[] = "Graphics";

void ReloadShadersOnGlThread(void*) {
    ShaderManager::Get().ReloadAll();
}

std::string LoadReceiverSnippet() {
    std::string snippet;
    bool fromFile = false;
    if (ShaderManager::Get().LoadShaderFile("world_shadow.glsl", snippet, &fromFile))
        GFX_LOGI(kTag, "world_shadow.glsl: %s", fromFile ? "loaded from TESTLIT" : "embedded default");
    return snippet;
}

} // namespace

GraphicsEngine& GraphicsEngine::Get() {
    static GraphicsEngine engine;
    return engine;
}

void GraphicsEngine::EarlyInit() {
    if (m_initialized) return;
    GraphicsLog::Init(paths::kLog);
    GFX_LOGI(kTag, "EAGLE GraphicsEngine starting (GTA SA 2.10 arm64, no AML)");

    m_config.Load(paths::kConfig);
    GraphicsLog::SetDebugEnabled(m_config.debug.log);
    m_config.LogSummary();
    {
        std::lock_guard<std::mutex> lock(m_requestMutex);
        m_pendingConfig = m_config;
        m_hasPendingConfig = false;
    }

    ShaderManager& sm = ShaderManager::Get();
    sm.SetBaseDir(paths::kShaders);
    RegisterEmbeddedShaders(sm);

    const bool receivers = m_config.graphics.enabled && m_config.shadow.enabled;
    ShaderPatcher::Configure(receivers, m_config.shadow.hardwarePcf, m_config.shadow.pcf, m_config.shadow.cascadeBlend,
                             m_config.shadow.water, LoadReceiverSnippet());
    if (!receivers)
        GFX_LOGW(kTag, "shadows disabled at startup: GTA shaders are not patched (enable + restart to get receivers)");

    if (!GameRenderBridge::Init()) {
        GFX_LOGE(kTag, "GraphicsEngine disabled: GTA symbols missing");
        return;
    }
    ShaderPatcher::SetActiveShaderSlot(GameRenderBridge::ActiveShaderSlot());
    RenderQueueBridge::SetSelectObserver(&ShaderPatcher::OnSelect);

    if (!GraphicsHooks::Install()) {
        GFX_LOGE(kTag, "GraphicsEngine disabled: hook installation failed");
        return;
    }
    m_initialized = true;
    GFX_LOGI(kTag, "GraphicsEngine initialized (waiting for the RenderQueue / first frame)");
}

void GraphicsEngine::Shutdown() {
    if (!m_initialized) return;
    if (RenderQueueBridge::OnGameThread()) m_shadows.Shutdown();
    m_suppressGtaShadows.store(false);
    GFX_LOGI(kTag, "GraphicsEngine shutdown");
}

void GraphicsEngine::ApplyConfig(bool logSummary) {
    m_config.Validate();
    GraphicsLog::SetDebugEnabled(m_config.debug.log);
    if (logSummary) m_config.LogSummary();
    // Receiver code generation only affects shaders GTA builds from now on.
    ShaderPatcher::Configure(ShaderPatcher::Enabled() || (m_config.graphics.enabled && m_config.shadow.enabled),
                             m_config.shadow.hardwarePcf, m_config.shadow.pcf, m_config.shadow.cascadeBlend,
                             m_config.shadow.water, LoadReceiverSnippet());
    ShadowGpu::ResetFailure();
}

void GraphicsEngine::ApplyPendingRequests() {
    if (m_reloadConfig.exchange(false)) {
        m_config.Load(paths::kConfig);
        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
            m_pendingConfig = m_config;
            m_hasPendingConfig = false;
        }
        ApplyConfig(true);
        GFX_LOGI(kTag, "config reloaded");
    }
    {
        std::lock_guard<std::mutex> lock(m_requestMutex);
        if (m_hasPendingConfig) {
            m_config = m_pendingConfig;
            m_hasPendingConfig = false;
            ApplyConfig(true);
        }
    }
    if (m_reloadShaders.exchange(false)) {
        if (RenderQueueBridge::Enqueue(&ReloadShadersOnGlThread, nullptr))
            GFX_LOGI(kTag, "engine shaders reload requested (GTA world shaders keep their code until restart)");
        ApplyConfig(false); // re-reads world_shadow.glsl for future GTA shader builds
    }
}

void GraphicsEngine::OnShadowPoint() {
    if (!m_initialized) return;
    ApplyPendingRequests();

    const auto now = std::chrono::steady_clock::now();
    float frameMs = 16.6f;
    if (m_haveLastFrame) frameMs = std::chrono::duration<float, std::milli>(now - m_lastFrame).count();
    m_lastFrame = now;
    m_haveLastFrame = true;
    if (!std::isfinite(frameMs) || frameMs <= 0.0f || frameMs > 1000.0f) frameMs = 16.6f;

    if (!RenderQueueBridge::Install()) return;
    if (!m_config.graphics.enabled) {
        m_suppressGtaShadows.store(false, std::memory_order_relaxed);
        return;
    }

    const WeatherSnapshot weather = GameRenderBridge::GetWeather();
    const float hour = m_config.sun.freezeHour >= 0.0f ? m_config.sun.freezeHour : GameRenderBridge::GetGameHour();
    m_tod = m_timeCycle.Evaluate(hour, weather);
    const SunLight& sun = m_sun.Update(m_config, m_tod, weather, GameRenderBridge::IsInterior());

    CameraSnapshot camera;
    GameRenderBridge::GetCamera(camera);

    UpdateAdaptiveQuality(frameMs);
    m_shadows.SetDistanceScale(m_distanceScale);
    const bool drawn = m_shadows.RenderShadowPass(sun, m_tod, camera, m_config);
    m_suppressGtaShadows.store(drawn && m_config.shadow.suppressGtaShadows, std::memory_order_relaxed);

    if (drawn && !m_loggedFirstFrame) {
        m_loggedFirstFrame = true;
        const CasterStats& st = m_shadows.Stats();
        GFX_LOGI(kTag, "first shadow frame: %s hour=%.2f sun elev=%.1f strength=%.2f casters=%d/%d/%d/%d",
                 m_tod.DominantName(), m_tod.hour, sun.elevationDeg, sun.shadowStrength, st.perCascade[0],
                 st.perCascade[1], st.perCascade[2], st.perCascade[3]);
    }
    if (m_config.debug.perfCounters) LogPerformance(drawn ? m_shadows.LastCpuMs() : 0.0f);
    ++m_frames;
}

void GraphicsEngine::OnEndWorld() {
    if (!m_initialized || !RenderQueueBridge::IsReady()) return;
    m_shadows.OnEndWorld(m_config);
}

void GraphicsEngine::UpdateAdaptiveQuality(float frameMs) {
    m_avgFrameMs += (frameMs - m_avgFrameMs) * 0.05f;
    m_statusFrameMs.store(m_avgFrameMs, std::memory_order_relaxed);
    if (!m_config.performance.adaptive) {
        m_distanceScale = 1.0f;
        m_statusScale.store(m_distanceScale, std::memory_order_relaxed);
        return;
    }
    const float seconds = frameMs / 1000.0f;
    const float targetMs = 1000.0f / m_config.performance.targetFps;
    if (m_avgFrameMs > targetMs * 1.12f) {
        m_lowTimer += seconds;
        m_highTimer = 0.0f;
    } else if (m_avgFrameMs < targetMs * 0.80f) {
        m_highTimer += seconds;
        m_lowTimer = 0.0f;
    } else {
        m_lowTimer = m_highTimer = 0.0f;
    }
    // Hysteresis: 4 s of low FPS to step down, 12 s of headroom to step up.
    if (m_lowTimer > 4.0f && m_distanceScale > 0.5f) {
        m_distanceScale = std::max(0.5f, m_distanceScale * 0.85f);
        m_lowTimer = 0.0f;
        GFX_LOGI(kTag, "adaptive: avg %.1f ms > target %.1f ms, shadow distance x%.2f", m_avgFrameMs, targetMs,
                 m_distanceScale);
    } else if (m_highTimer > 12.0f && m_distanceScale < 1.0f) {
        m_distanceScale = std::min(1.0f, m_distanceScale / 0.85f);
        m_highTimer = 0.0f;
        GFX_LOGI(kTag, "adaptive: headroom, shadow distance x%.2f", m_distanceScale);
    }
    m_statusScale.store(m_distanceScale, std::memory_order_relaxed);
}

void GraphicsEngine::LogPerformance(float shadowCpuMs) {
    m_perfTimer += m_avgFrameMs / 1000.0f;
    if (m_perfTimer < 5.0f) return;
    m_perfTimer = 0.0f;
    const CasterStats& st = m_shadows.Stats();
    GFX_LOGI("Perf", "frame %.2f ms (%.0f fps) | shadow pass CPU %.2f ms | casters %d/%d/%d/%d dropped %d | visible %d sector %d | receivers %d",
             m_avgFrameMs, 1000.0f / std::max(m_avgFrameMs, 0.1f), shadowCpuMs, st.perCascade[0], st.perCascade[1],
             st.perCascade[2], st.perCascade[3], st.dropped, st.visibleConsidered, st.sectorConsidered,
             ShaderPatcher::ReceiverCount());
}

// ------------------------------------------------------------- control API

void GraphicsEngine::RequestReloadConfig() { m_reloadConfig.store(true); }

void GraphicsEngine::RequestReloadShaders() { m_reloadShaders.store(true); }

void GraphicsEngine::SetQuality(int quality) {
    std::lock_guard<std::mutex> lock(m_requestMutex);
    m_pendingConfig.ApplyQualityPreset(static_cast<GraphicsQuality>(std::clamp(quality, 0, 3)));
    m_pendingConfig.Validate();
    m_hasPendingConfig = true;
}

void GraphicsEngine::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_requestMutex);
    m_pendingConfig.graphics.enabled = enabled;
    m_hasPendingConfig = true;
}

void GraphicsEngine::SetShadowEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_requestMutex);
    m_pendingConfig.shadow.enabled = enabled;
    m_hasPendingConfig = true;
}

void GraphicsEngine::SetShadowDistance(float metres) {
    std::lock_guard<std::mutex> lock(m_requestMutex);
    m_pendingConfig.shadow.distance = metres;
    m_pendingConfig.Validate();
    m_hasPendingConfig = true;
}

void GraphicsEngine::SetBloom(bool enabled, float intensity) {
    std::lock_guard<std::mutex> lock(m_requestMutex);
    m_pendingConfig.bloom.enabled = enabled;
    m_pendingConfig.bloom.intensity = intensity;
    m_pendingConfig.Validate();
    m_hasPendingConfig = true;
}

void GraphicsEngine::SetDebugFlag(const std::string& name, bool value) {
    std::lock_guard<std::mutex> lock(m_requestMutex);
    DebugSettings& d = m_pendingConfig.debug;
    if (name == "showCascade") d.showCascade = value;
    else if (name == "showShadowMap") d.showShadowMap = value;
    else if (name == "showDepth") d.showDepth = value;
    else if (name == "showSSAO") d.showSsao = value;
    else if (name == "showSunDirection") d.showSunDirection = value;
    else if (name == "freezeSun") m_pendingConfig.sun.freeze = value;
    else if (name == "freezeShadowCamera") d.freezeShadowCamera = value;
    else if (name == "perfCounters") d.perfCounters = value;
    else if (name == "log") d.log = value;
    else return;
    m_hasPendingConfig = true;
}

std::string GraphicsEngine::StatusString() {
    GraphicsConfig cfg;
    {
        std::lock_guard<std::mutex> lock(m_requestMutex);
        cfg = m_pendingConfig;
    }
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  "initialized=%d enabled=%d quality=%s shadows=%d cascades=%d resolution=%d distance=%.0f "
                  "receivers=%d rqBridge=%d gpuFailed=%d avgFrame=%.1fms scale=%.2f",
                  m_initialized, cfg.graphics.enabled, QualityName(cfg.graphics.quality), cfg.shadow.enabled,
                  cfg.shadow.cascades, cfg.shadow.resolution, cfg.shadow.distance, ShaderPatcher::ReceiverCount(),
                  RenderQueueBridge::IsReady(), ShadowGpu::Failed(), m_statusFrameMs.load(std::memory_order_relaxed),
                  m_statusScale.load(std::memory_order_relaxed));
    return buffer;
}

} // namespace gfx
