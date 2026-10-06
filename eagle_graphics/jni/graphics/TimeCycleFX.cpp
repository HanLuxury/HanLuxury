#include "TimeCycleFX.h"

#include <cmath>

namespace gfx {
namespace {

LookProfile Blend(const LookProfile& a, const LookProfile& b, float t) {
    LookProfile r;
    r.sunColor = Lerp(a.sunColor, b.sunColor, t);
    r.sunIntensity = LerpF(a.sunIntensity, b.sunIntensity, t);
    r.shadowStrength = LerpF(a.shadowStrength, b.shadowStrength, t);
    r.shadowTint = Lerp(a.shadowTint, b.shadowTint, t);
    r.sunBoost = LerpF(a.sunBoost, b.sunBoost, t);
    r.exposure = LerpF(a.exposure, b.exposure, t);
    r.saturation = LerpF(a.saturation, b.saturation, t);
    r.contrast = LerpF(a.contrast, b.contrast, t);
    r.bloomScale = LerpF(a.bloomScale, b.bloomScale, t);
    r.fogTint = Lerp(a.fogTint, b.fogTint, t);
    r.fogDensity = LerpF(a.fogDensity, b.fogDensity, t);
    return r;
}

LookProfile Scale(const LookProfile& p, float w) {
    LookProfile r;
    r.sunColor = p.sunColor * w;
    r.sunIntensity = p.sunIntensity * w;
    r.shadowStrength = p.shadowStrength * w;
    r.shadowTint = p.shadowTint * w;
    r.sunBoost = p.sunBoost * w;
    r.exposure = p.exposure * w;
    r.saturation = p.saturation * w;
    r.contrast = p.contrast * w;
    r.bloomScale = p.bloomScale * w;
    r.fogTint = p.fogTint * w;
    r.fogDensity = p.fogDensity * w;
    return r;
}

void Accumulate(LookProfile& acc, const LookProfile& p) {
    acc.sunColor += p.sunColor;
    acc.sunIntensity += p.sunIntensity;
    acc.shadowStrength += p.shadowStrength;
    acc.shadowTint += p.shadowTint;
    acc.sunBoost += p.sunBoost;
    acc.exposure += p.exposure;
    acc.saturation += p.saturation;
    acc.contrast += p.contrast;
    acc.bloomScale += p.bloomScale;
    acc.fogTint += p.fogTint;
    acc.fogDensity += p.fogDensity;
}

// Smooth 0->1 ramp over [center - 0.5h, center + 0.5h].
float Ramp(float hour, float center) { return SmoothStep(center - 0.5f, center + 0.5f, hour); }

} // namespace

const char* TimeOfDayState::DominantName() const {
    float best = wDay;
    const char* name = "DAY";
    if (wSunrise > best) { best = wSunrise; name = "SUNRISE"; }
    if (wSunset > best) { best = wSunset; name = "SUNSET"; }
    if (wNight > best) { name = "NIGHT"; }
    return name;
}

TimeCycleFX::TimeCycleFX() {
    LookProfile& sunrise = m_profiles[0];
    sunrise.sunColor = {1.00f, 0.74f, 0.50f};
    sunrise.sunIntensity = 0.85f;
    sunrise.shadowStrength = 0.80f;
    sunrise.shadowTint = {0.55f, 0.53f, 0.64f};
    sunrise.sunBoost = 1.15f;
    sunrise.exposure = 1.06f; sunrise.saturation = 1.04f; sunrise.contrast = 1.03f; sunrise.bloomScale = 1.15f;
    sunrise.fogTint = {0.96f, 0.78f, 0.66f}; sunrise.fogDensity = 1.1f;

    LookProfile& day = m_profiles[1];
    day.sunColor = {1.00f, 0.96f, 0.90f};
    day.sunIntensity = 1.00f;
    day.shadowStrength = 1.00f;
    day.shadowTint = {0.46f, 0.50f, 0.60f};
    day.sunBoost = 1.0f;
    day.exposure = 1.00f; day.saturation = 1.04f; day.contrast = 1.03f; day.bloomScale = 0.8f;
    day.fogTint = {0.72f, 0.80f, 0.90f}; day.fogDensity = 0.8f;

    LookProfile& sunset = m_profiles[2];
    sunset.sunColor = {1.00f, 0.62f, 0.34f};
    sunset.sunIntensity = 0.92f;
    sunset.shadowStrength = 0.88f;
    sunset.shadowTint = {0.50f, 0.45f, 0.60f};
    sunset.sunBoost = 1.35f;
    sunset.exposure = 1.04f; sunset.saturation = 1.06f; sunset.contrast = 1.05f; sunset.bloomScale = 1.3f;
    sunset.fogTint = {0.98f, 0.66f, 0.42f}; sunset.fogDensity = 1.2f;

    LookProfile& night = m_profiles[3];
    night.sunColor = {0.55f, 0.62f, 0.85f};
    night.sunIntensity = 0.0f;
    night.shadowStrength = 0.0f;
    night.shadowTint = {0.70f, 0.72f, 0.82f};
    night.sunBoost = 0.0f;
    night.exposure = 1.18f; night.saturation = 0.95f; night.contrast = 1.02f; night.bloomScale = 1.2f;
    night.fogTint = {0.22f, 0.28f, 0.40f}; night.fogDensity = 0.9f;
}

TimeOfDayState TimeCycleFX::Evaluate(float hour, const WeatherSnapshot& weather) const {
    TimeOfDayState s;
    if (!std::isfinite(hour)) hour = 12.0f;
    hour = std::fmod(std::fmax(hour, 0.0f), 24.0f);
    s.hour = hour;

    // Piecewise cross-fades: night->sunrise @5, sunrise->day @7, day->sunset @16, sunset->night @19.
    const float toSunrise = Ramp(hour, 5.0f);
    const float toDay = Ramp(hour, 7.0f);
    const float toSunset = Ramp(hour, 16.0f);
    const float toNight = Ramp(hour, 19.0f);
    s.wSunrise = toSunrise * (1.0f - toDay);
    s.wDay = toDay * (1.0f - toSunset);
    s.wSunset = toSunset * (1.0f - toNight);
    s.wNight = 1.0f - s.wSunrise - s.wDay - s.wSunset;
    if (s.wNight < 0.0f) s.wNight = 0.0f;

    LookProfile acc = Scale(m_profiles[0], s.wSunrise);
    Accumulate(acc, Scale(m_profiles[1], s.wDay));
    Accumulate(acc, Scale(m_profiles[2], s.wSunset));
    Accumulate(acc, Scale(m_profiles[3], s.wNight));

    // Weather on top of the time of day.
    s.rain = weather.rain;
    s.cloud = weather.cloud;
    s.fog = weather.fog;
    s.wetness = weather.wetRoads;
    const float overcast = Saturate(fmaxf(weather.cloud, weather.rain));
    LookProfile rainy = acc;
    rainy.sunColor = Lerp(acc.sunColor, Vec3{0.80f, 0.86f, 0.92f}, 0.8f);
    rainy.sunIntensity = acc.sunIntensity * 0.25f;
    rainy.shadowStrength = acc.shadowStrength * 0.10f;
    rainy.shadowTint = Lerp(acc.shadowTint, Vec3{0.72f, 0.76f, 0.80f}, 0.7f);
    rainy.sunBoost = acc.sunBoost * 0.1f;
    rainy.exposure = acc.exposure * 1.05f;
    rainy.saturation = acc.saturation * 0.82f;
    rainy.contrast = acc.contrast * 0.97f;
    rainy.fogTint = Lerp(acc.fogTint, Vec3{0.55f, 0.62f, 0.62f}, 0.75f);
    rainy.fogDensity = acc.fogDensity * 1.8f;
    s.look = Blend(acc, rainy, overcast);

    // Fog alone softens the sun.
    const float fogLoss = 1.0f - 0.6f * weather.fog;
    s.look.shadowStrength *= fogLoss;
    s.look.sunIntensity *= fogLoss;
    return s;
}

} // namespace gfx
