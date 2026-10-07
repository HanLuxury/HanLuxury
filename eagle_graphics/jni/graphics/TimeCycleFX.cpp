#include "TimeCycleFX.h"
#include "GraphicsLog.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

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

TimeCycleFX::TimeCycleFX() { ResetProfiles(); }

void TimeCycleFX::ResetProfiles() {
    m_ramps[0] = 5.0f;
    m_ramps[1] = 7.0f;
    m_ramps[2] = 16.0f;
    m_ramps[3] = 19.0f;
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

    // Piecewise cross-fades (defaults): night->sunrise @5, sunrise->day @7, day->sunset @16, sunset->night @19.
    const float toSunrise = Ramp(hour, m_ramps[0]);
    const float toDay = Ramp(hour, m_ramps[1]);
    const float toSunset = Ramp(hour, m_ramps[2]);
    const float toNight = Ramp(hour, m_ramps[3]);
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

// ------------------------------------------------------- eagle_timecyc.dat

int TimeCycleFX::ParseProfiles(const std::string& text, std::string& error) {
    static const char* const kNames[4] = {"SUNRISE", "DAY", "SUNSET", "NIGHT"};
    constexpr size_t kColumns = 17;
    int applied = 0;
    size_t pos = 0;
    int lineNo = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        const size_t comment = line.find("//");
        if (comment != std::string::npos) line.erase(comment);
        std::vector<std::string> w;
        for (size_t i = 0; i < line.size();) {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            const size_t b = i;
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            if (i > b) w.emplace_back(line, b, i - b);
        }
        if (w.empty() || w[0][0] == '#' || w[0][0] == ';') continue;
        std::string name = w[0];
        for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        std::vector<float> v;
        bool numbersOk = true;
        for (size_t i = 1; i < w.size(); ++i) {
            errno = 0;
            char* e = nullptr;
            const float f = std::strtof(w[i].c_str(), &e);
            if (errno != 0 || !e || *e != '\0' || !std::isfinite(f)) { numbersOk = false; break; }
            v.push_back(f);
        }
        const std::string where = "line " + std::to_string(lineNo) + " (" + name + ")";
        if (!numbersOk) { error += where + ": not a number; "; continue; }
        if (name == "RAMPS") {
            if (v.size() != 4 || !(v[0] >= 0.5f && v[0] < v[1] && v[1] < v[2] && v[2] < v[3] && v[3] <= 23.5f)) {
                error += where + ": needs 4 increasing hours between 0.5 and 23.5; ";
                continue;
            }
            for (int i = 0; i < 4; ++i) m_ramps[i] = v[i];
            ++applied;
            continue;
        }
        int index = -1;
        for (int i = 0; i < 4; ++i)
            if (name == kNames[i]) index = i;
        if (index < 0) { error += where + ": unknown period; "; continue; }
        if (v.size() != kColumns) { error += where + ": needs " + std::to_string(kColumns) + " numbers; "; continue; }
        auto c01 = [](float x, float hi) { return x < 0.0f ? 0.0f : (x > hi ? hi : x); };
        LookProfile p;
        p.sunColor = {c01(v[0], 4.0f), c01(v[1], 4.0f), c01(v[2], 4.0f)};
        p.sunIntensity = c01(v[3], 4.0f);
        p.shadowStrength = c01(v[4], 1.5f);
        p.shadowTint = {c01(v[5], 1.0f), c01(v[6], 1.0f), c01(v[7], 1.0f)};
        p.sunBoost = c01(v[8], 4.0f);
        p.exposure = c01(v[9], 4.0f);
        p.saturation = c01(v[10], 2.0f);
        p.contrast = c01(v[11], 2.0f);
        p.bloomScale = c01(v[12], 4.0f);
        p.fogTint = {c01(v[13], 1.0f), c01(v[14], 1.0f), c01(v[15], 1.0f)};
        p.fogDensity = c01(v[16], 4.0f);
        m_profiles[index] = p;
        ++applied;
    }
    return applied;
}

bool TimeCycleFX::LoadProfiles(const char* path) {
    ResetProfiles();
    FILE* f = path ? std::fopen(path, "rb") : nullptr;
    if (!f) {
        GFX_LOGW("TimeCycleFX", "%s not found, built-in time-of-day looks", path ? path : "(null)");
        return false;
    }
    std::string text;
    char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0 && text.size() < 64 * 1024) text.append(chunk, n);
    std::fclose(f);
    std::string error;
    const int rows = ParseProfiles(text, error);
    if (!error.empty()) GFX_LOGW("TimeCycleFX", "%s: %s", path, error.c_str());
    GFX_LOGI("TimeCycleFX", "loaded %s (%d rows, ramps %.1f/%.1f/%.1f/%.1f h)", path, rows, m_ramps[0], m_ramps[1],
             m_ramps[2], m_ramps[3]);
    return true;
}

} // namespace gfx
