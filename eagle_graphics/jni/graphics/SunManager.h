#pragma once
// EAGLE graphics engine - real-time sun.
//
// Direction: GTA's own sun vector. CTimeCycle::CalcColoursForPoint (libGTASA
// 0x5030B0) recomputes it every frame from the clock:
//     a      = (hours*60 + minutes + seconds/60) * (2*pi/1440)
//     toSun  = normalize(sin(a) + 0.7, -0.7, 0.2 - cos(a))
// so the sun rises in the east (~05:45), peaks at ~50 deg at noon and sets
// around 18:40. Using the same vector keeps shadows aligned with the corona.
//
// The engine adds: elevation clamp for casting (no infinite sunset shadows),
// a small dead-zone so the light camera does not re-rotate every frame
// (avoids shadow shimmer), horizon/weather/interior fades and colour.

#include "GraphicsConfig.h"
#include "TimeCycleFX.h"
#include "Math/Vector3.h"

namespace gfx {

struct SunLight {
    Vec3 direction{0.0f, 0.0f, -1.0f}; // light travel direction (sun -> scene), used by the light camera
    Vec3 toSun{0.0f, 0.0f, 1.0f};      // unclamped direction to the sun (shading, N.L)
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 0.0f;            // 0 at night
    float shadowStrength = 0.0f;       // final receiver strength (0 = shadows off)
    float elevationDeg = 0.0f;         // true elevation of the sun
    float castElevationDeg = 0.0f;     // elevation used for the shadow projection
    bool castsShadows = false;
};

class SunManager {
public:
    void Reset();
    // Game thread, once per frame. Returns the current sun.
    const SunLight& Update(const GraphicsConfig& config, const TimeOfDayState& tod, const WeatherSnapshot& weather,
                           bool interior);
    const SunLight& Current() const { return m_sun; }

private:
    SunLight m_sun;
    Vec3 m_castDirection{0.0f, 0.0f, -1.0f};
    bool m_hasDirection = false;
    float m_logTimer = 0.0f;
};

} // namespace gfx
