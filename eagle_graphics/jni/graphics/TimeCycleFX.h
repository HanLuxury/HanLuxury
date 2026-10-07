#pragma once
// EAGLE graphics engine - time-of-day / weather look profiles.
//
// Four presets (SUNRISE 05-07, DAY 07-16, SUNSET 16-19, NIGHT 19-05) are
// cross-faded with one-hour smoothstep bands driven by the GTA clock, so the
// look never switches abruptly. Weather (rain, clouds, fog) is applied on top.
// The sun DIRECTION is not decided here: it always comes from GTA's timecycle
// (CTimeCycle::m_VectorToSun) so shadows match the visible sun corona.
//
// The four looks and the ramp hours can be edited in
// TESTLIT/graphics/data/eagle_timecyc.dat (one row per period, see that file);
// rows that are missing keep the built-in values.

#include "GameRenderBridge.h"
#include "Math/Vector3.h"

#include <string>

namespace gfx {

struct LookProfile {
    Vec3 sunColor{1.0f, 0.96f, 0.90f};
    float sunIntensity = 1.0f;     // scales sun boost / bloom (phase 6)
    float shadowStrength = 1.0f;   // multiplies [shadow] strength
    Vec3 shadowTint{0.46f, 0.50f, 0.60f}; // colour multiplier at full shadow (sky-lit ambient)
    float sunBoost = 1.0f;         // multiplies [shadow] sunBoost
    // Post-process parameters consumed from phase 6 on.
    float exposure = 1.0f;
    float saturation = 1.0f;
    float contrast = 1.0f;
    float bloomScale = 1.0f;
    Vec3 fogTint{0.70f, 0.78f, 0.88f};
    float fogDensity = 1.0f;
};

struct TimeOfDayState {
    float hour = 12.0f;
    float wSunrise = 0.0f, wDay = 1.0f, wSunset = 0.0f, wNight = 0.0f; // sum = 1
    float rain = 0.0f, cloud = 0.0f, fog = 0.0f, wetness = 0.0f;
    LookProfile look;
    const char* DominantName() const;
};

class TimeCycleFX {
public:
    TimeCycleFX();

    // hour: 0..24, weather snapshot from GameRenderBridge::GetWeather().
    TimeOfDayState Evaluate(float hour, const WeatherSnapshot& weather) const;

    LookProfile& Profile(int index) { return m_profiles[index]; } // 0 sunrise, 1 day, 2 sunset, 3 night

    // Built-in looks, then every valid row of the file. Returns false if the file is missing.
    bool LoadProfiles(const char* path);
    // Host-testable parser. Returns the number of rows applied; 'error' lists rejected lines.
    int ParseProfiles(const std::string& text, std::string& error);
    void ResetProfiles();
    const float* Ramps() const { return m_ramps; }

private:
    LookProfile m_profiles[4];
    float m_ramps[4] = {5.0f, 7.0f, 16.0f, 19.0f}; // night->sunrise, sunrise->day, day->sunset, sunset->night
};

} // namespace gfx
