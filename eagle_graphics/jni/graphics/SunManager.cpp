#include "SunManager.h"
#include "GameRenderBridge.h"
#include "GraphicsLog.h"

#include <cmath>

namespace gfx {
namespace {

constexpr char kTag[] = "Sun";

// Same formula as CTimeCycle::CalcColoursForPoint, used only for the debug
// "freezeHour" override (the live path reads the game's vector).
Vec3 GtaSunVectorForHour(float hour) {
    const float a = hour * 60.0f * (2.0f * kPi / 1440.0f);
    return Normalized(Vec3{std::sin(a) + 0.7f, -0.7f, 0.2f - std::cos(a)}, Vec3{0.0f, 0.0f, 1.0f});
}

// Keeps the azimuth of 'toSun' but raises its elevation to at least minDeg.
Vec3 ClampElevation(const Vec3& toSun, float minDeg, float& outDeg) {
    const float minRad = minDeg * kDegToRad;
    const float elev = std::asin(Clamp(toSun.z, -1.0f, 1.0f));
    const float e = elev < minRad ? minRad : elev;
    outDeg = e * kRadToDeg;
    Vec3 horizontal{toSun.x, toSun.y, 0.0f};
    if (!Normalize(horizontal)) return {0.0f, 0.0f, 1.0f};
    return Vec3{horizontal.x * std::cos(e), horizontal.y * std::cos(e), std::sin(e)};
}

} // namespace

void SunManager::Reset() {
    m_sun = SunLight{};
    m_hasDirection = false;
    m_logTimer = 0.0f;
}

const SunLight& SunManager::Update(const GraphicsConfig& config, const TimeOfDayState& tod,
                                   const WeatherSnapshot& weather, bool interior) {
    const SunSettings& sc = config.sun;

    Vec3 toSun = sc.freezeHour >= 0.0f ? GtaSunVectorForHour(sc.freezeHour) : GameRenderBridge::GetVectorToSun();
    if (!IsFinite(toSun)) toSun = {0.0f, 0.0f, 1.0f};

    if (sc.freeze && m_hasDirection) {
        toSun = m_sun.toSun; // debug: keep the previous sun
    }

    const float elevation = std::asin(Clamp(toSun.z, -1.0f, 1.0f)) * kRadToDeg;
    float castElevation = elevation;
    const Vec3 castToSun = ClampElevation(toSun, config.shadow.minElevation, castElevation);
    const Vec3 castDir = -castToSun;

    // Dead-zone: rotate the light camera only after a visible change.
    const float threshold = std::cos(sc.updateThreshold * kDegToRad);
    if (!m_hasDirection || Dot(castDir, m_castDirection) < threshold) {
        m_castDirection = castDir;
        m_hasDirection = true;
    }

    // Horizon fade: shadows disappear while the sun is within ~1..7 deg of the horizon.
    const float horizon = SmoothStep(1.0f, 7.0f, elevation);
    const float environment = (1.0f - weather.underwater) * (1.0f - weather.tunnel) * (interior ? 0.0f : 1.0f);

    m_sun.toSun = toSun;
    m_sun.direction = m_castDirection;
    m_sun.elevationDeg = elevation;
    m_sun.castElevationDeg = castElevation;
    m_sun.color = tod.look.sunColor;
    m_sun.intensity = tod.look.sunIntensity * horizon * environment;
    m_sun.shadowStrength = Saturate(config.shadow.strength * tod.look.shadowStrength * horizon * environment);
    m_sun.castsShadows = config.shadow.enabled && m_sun.shadowStrength > 0.01f;

    if (config.debug.showSunDirection) {
        m_logTimer += 1.0f;
        if (m_logTimer >= 60.0f) { // roughly once per second at 60 FPS
            m_logTimer = 0.0f;
            GFX_LOGI(kTag, "hour=%.2f %s toSun=(%.3f %.3f %.3f) elev=%.1f cast=%.1f strength=%.2f intensity=%.2f",
                     tod.hour, tod.DominantName(), toSun.x, toSun.y, toSun.z, elevation, castElevation,
                     m_sun.shadowStrength, m_sun.intensity);
        }
    }
    return m_sun;
}

} // namespace gfx
