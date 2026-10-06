#include "GraphicsConfig.h"
#include "GraphicsLog.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gfx {
namespace {

constexpr char kTag[] = "Config";

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string MakeKey(const char* section, const char* key) {
    return Lower(std::string(section ? section : "")) + "." + Lower(std::string(key ? key : ""));
}

bool ParseFloat(const std::string& text, float& out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const float v = std::strtof(text.c_str(), &end);
    if (errno != 0 || !end || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool ParseInt(const std::string& text, int& out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0' || v < -1000000L || v > 1000000L) return false;
    out = static_cast<int>(v);
    return true;
}

} // namespace

// ---------------------------------------------------------------- IniFile

bool IniFile::Load(const char* path) {
    FILE* f = path ? std::fopen(path, "rb") : nullptr;
    if (!f) return false;
    std::string text;
    char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        text.append(chunk, n);
        if (text.size() > 256 * 1024) break; // a config file never needs more
    }
    std::fclose(f);
    return LoadFromString(text);
}

bool IniFile::LoadFromString(const std::string& text) {
    m_values.clear();
    std::string section;
    size_t pos = 0;
    int lineNo = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;

        // Strip comments (';' or '#') and a UTF-8 BOM on the first line.
        if (lineNo == 1 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);
        const size_t comment = line.find_first_of(";#");
        if (comment != std::string::npos) line.erase(comment);
        line = Trim(line);
        if (line.empty()) continue;

        if (line.front() == '[') {
            const size_t close = line.find(']');
            if (close == std::string::npos) {
                GFX_LOGW(kTag, "line %d: missing ']'", lineNo);
                continue;
            }
            section = Lower(Trim(line.substr(1, close - 1)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            GFX_LOGW(kTag, "line %d: expected key=value", lineNo);
            continue;
        }
        const std::string key = Lower(Trim(line.substr(0, eq)));
        const std::string value = Trim(line.substr(eq + 1));
        if (key.empty()) continue;
        m_values[section + "." + key] = value;
        if (pos > text.size()) break;
    }
    return true;
}

const std::string* IniFile::Find(const char* section, const char* key) const {
    auto it = m_values.find(MakeKey(section, key));
    return it == m_values.end() ? nullptr : &it->second;
}

bool IniFile::Has(const char* section, const char* key) const { return Find(section, key) != nullptr; }

bool IniFile::GetString(const char* section, const char* key, std::string& out) const {
    const std::string* v = Find(section, key);
    if (!v) return false;
    out = *v;
    return true;
}

bool IniFile::GetBool(const char* section, const char* key, bool& out) const {
    const std::string* v = Find(section, key);
    if (!v) return false;
    const std::string s = Lower(*v);
    if (s == "1" || s == "true" || s == "on" || s == "yes") { out = true; return true; }
    if (s == "0" || s == "false" || s == "off" || s == "no") { out = false; return true; }
    GFX_LOGW(kTag, "[%s] %s: '%s' is not a boolean", section, key, v->c_str());
    return false;
}

bool IniFile::GetInt(const char* section, const char* key, int& out) const {
    const std::string* v = Find(section, key);
    if (!v) return false;
    if (ParseInt(*v, out)) return true;
    float f;
    if (ParseFloat(*v, f)) { out = static_cast<int>(std::lround(f)); return true; }
    GFX_LOGW(kTag, "[%s] %s: '%s' is not a number", section, key, v->c_str());
    return false;
}

bool IniFile::GetFloat(const char* section, const char* key, float& out) const {
    const std::string* v = Find(section, key);
    if (!v) return false;
    if (ParseFloat(*v, out)) return true;
    GFX_LOGW(kTag, "[%s] %s: '%s' is not a number", section, key, v->c_str());
    return false;
}

bool IniFile::GetColor(const char* section, const char* key, float out[3]) const {
    const std::string* v = Find(section, key);
    if (!v) return false;
    float c[3];
    size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t comma = v->find(',', start);
        const std::string part = Trim(v->substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!ParseFloat(part, c[i])) {
            GFX_LOGW(kTag, "[%s] %s: '%s' is not r,g,b", section, key, v->c_str());
            return false;
        }
        if (comma == std::string::npos && i < 2) {
            GFX_LOGW(kTag, "[%s] %s: '%s' is not r,g,b", section, key, v->c_str());
            return false;
        }
        start = comma + 1;
    }
    const bool bytes = c[0] > 1.0f || c[1] > 1.0f || c[2] > 1.0f;
    for (int i = 0; i < 3; ++i) out[i] = std::clamp(bytes ? c[i] / 255.0f : c[i], 0.0f, 4.0f);
    return true;
}

// ---------------------------------------------------------- GraphicsConfig

const char* QualityName(GraphicsQuality q) {
    switch (q) {
        case GraphicsQuality::Low: return "LOW";
        case GraphicsQuality::Medium: return "MEDIUM";
        case GraphicsQuality::High: return "HIGH";
        default: return "ULTRA";
    }
}

void GraphicsConfig::ApplyQualityPreset(GraphicsQuality q) {
    graphics.quality = q;
    ShadowSettings& s = shadow;
    switch (q) {
        case GraphicsQuality::Low:
            s.cascades = 2; s.resolution = 1024; s.distance = 80.0f; s.pcf = 0;
            s.cascadeBlend = false; s.maxCasters = 450; s.depth24 = false;
            s.offscreenCasters = false; s.casterExtend = 90.0f;
            ssao.enabled = false; godray.enabled = false; bloom.intensity = 0.45f;
            break;
        case GraphicsQuality::Medium:
            s.cascades = 3; s.resolution = 1536; s.distance = 120.0f; s.pcf = 1;
            s.cascadeBlend = false; s.maxCasters = 750; s.depth24 = false;
            s.offscreenCasters = true; s.casterExtend = 120.0f;
            ssao.enabled = true; godray.enabled = false; bloom.intensity = 0.55f;
            break;
        case GraphicsQuality::High:
            s.cascades = 3; s.resolution = 2048; s.distance = 160.0f; s.pcf = 2;
            s.cascadeBlend = true; s.maxCasters = 1000; s.depth24 = false;
            s.offscreenCasters = true; s.casterExtend = 140.0f;
            ssao.enabled = true; godray.enabled = true; bloom.intensity = 0.65f;
            break;
        case GraphicsQuality::Ultra:
            s.cascades = 4; s.resolution = 2048; s.distance = 220.0f; s.pcf = 2;
            s.cascadeBlend = true; s.maxCasters = 1400; s.depth24 = true;
            s.offscreenCasters = true; s.casterExtend = 170.0f;
            ssao.enabled = true; godray.enabled = true; bloom.intensity = 0.65f;
            break;
    }
}

void GraphicsConfig::Validate() {
    auto clampf = [](float& v, float lo, float hi, float fallback) {
        v = std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    graphics.quality = static_cast<GraphicsQuality>(std::clamp(static_cast<int>(graphics.quality), 0, 3));
    clampf(graphics.exposure, 0.1f, 4.0f, 1.05f);
    clampf(graphics.gamma, 1.0f, 3.0f, 2.2f);
    clampf(graphics.saturation, 0.0f, 2.0f, 1.05f);
    clampf(graphics.contrast, 0.5f, 1.5f, 1.02f);

    ShadowSettings& s = shadow;
    s.cascades = std::clamp(s.cascades, 1, 4);
    s.resolution = std::clamp(s.resolution, 512, 4096);
    s.resolution = (s.resolution + 63) & ~63; // keep tiles aligned
    clampf(s.distance, 20.0f, 600.0f, 160.0f);
    clampf(s.strength, 0.0f, 1.0f, 0.78f);
    clampf(s.splitLambda, 0.0f, 1.0f, 0.75f);
    clampf(s.depthBias, 0.0f, 2.0f, 0.06f);
    clampf(s.normalBias, 0.0f, 8.0f, 1.5f);
    clampf(s.slopeBias, 0.0f, 16.0f, 1.6f);
    clampf(s.slopeUnits, 0.0f, 64.0f, 4.0f);
    s.pcf = std::clamp(s.pcf, 0, 2);
    clampf(s.pcfSpread, 0.25f, 4.0f, 1.0f);
    clampf(s.blendBand, 0.0f, 0.5f, 0.12f);
    clampf(s.fadeRange, 0.01f, 0.5f, 0.15f);
    clampf(s.casterExtend, 0.0f, 500.0f, 140.0f);
    clampf(s.minElevation, 2.0f, 45.0f, 9.0f);
    clampf(s.alphaCutoff, 0.0f, 1.0f, 0.45f);
    s.maxCasters = std::clamp(s.maxCasters, 32, 4096);
    clampf(s.offscreenDynamicRange, 0.0f, 300.0f, 60.0f);
    clampf(s.prelitDirectShare, 0.0f, 1.0f, 0.85f);
    clampf(s.sunBoost, 0.0f, 1.0f, 0.10f);
    for (float& c : s.tint) clampf(c, 0.0f, 1.0f, 0.5f);

    clampf(sun.updateThreshold, 0.0f, 10.0f, 0.10f);
    if (!std::isfinite(sun.freezeHour) || sun.freezeHour >= 24.0f) sun.freezeHour = -1.0f;

    clampf(bloom.intensity, 0.0f, 4.0f, 0.65f);
    clampf(bloom.threshold, 0.0f, 8.0f, 1.0f);
    clampf(bloom.radius, 0.1f, 4.0f, 1.0f);
    clampf(ssao.strength, 0.0f, 2.0f, 0.55f);
    clampf(ssao.radius, 0.05f, 5.0f, 0.7f);
    clampf(fog.density, 0.0f, 4.0f, 1.0f);
    clampf(godray.intensity, 0.0f, 2.0f, 0.35f);
    clampf(rain.wetness, 0.0f, 2.0f, 1.0f);
    clampf(performance.targetFps, 15.0f, 120.0f, 30.0f);
}

void GraphicsConfig::ApplyIni(const IniFile& ini) {
    int quality = static_cast<int>(graphics.quality);
    if (ini.GetInt("graphics", "quality", quality))
        ApplyQualityPreset(static_cast<GraphicsQuality>(std::clamp(quality, 0, 3)));

    ini.GetBool("graphics", "enabled", graphics.enabled);
    ini.GetFloat("graphics", "exposure", graphics.exposure);
    ini.GetFloat("graphics", "gamma", graphics.gamma);
    ini.GetFloat("graphics", "saturation", graphics.saturation);
    ini.GetFloat("graphics", "contrast", graphics.contrast);

    ShadowSettings& s = shadow;
    ini.GetBool("shadow", "enabled", s.enabled);
    ini.GetInt("shadow", "cascades", s.cascades);
    ini.GetInt("shadow", "resolution", s.resolution);
    ini.GetFloat("shadow", "distance", s.distance);
    ini.GetFloat("shadow", "strength", s.strength);
    ini.GetFloat("shadow", "splitLambda", s.splitLambda);
    ini.GetFloat("shadow", "depthBias", s.depthBias);
    ini.GetFloat("shadow", "normalBias", s.normalBias);
    ini.GetFloat("shadow", "slopeBias", s.slopeBias);
    ini.GetFloat("shadow", "slopeUnits", s.slopeUnits);
    ini.GetInt("shadow", "pcf", s.pcf);
    ini.GetFloat("shadow", "pcfSpread", s.pcfSpread);
    ini.GetBool("shadow", "cascadeBlend", s.cascadeBlend);
    ini.GetFloat("shadow", "blendBand", s.blendBand);
    ini.GetFloat("shadow", "fadeRange", s.fadeRange);
    ini.GetFloat("shadow", "casterExtend", s.casterExtend);
    ini.GetFloat("shadow", "minElevation", s.minElevation);
    ini.GetFloat("shadow", "alphaCutoff", s.alphaCutoff);
    ini.GetInt("shadow", "maxCasters", s.maxCasters);
    ini.GetBool("shadow", "depth24", s.depth24);
    ini.GetBool("shadow", "hardwarePcf", s.hardwarePcf);
    ini.GetBool("shadow", "buildings", s.buildings);
    ini.GetBool("shadow", "objects", s.objects);
    ini.GetBool("shadow", "vehicles", s.vehicles);
    ini.GetBool("shadow", "peds", s.peds);
    ini.GetBool("shadow", "weapons", s.weapons);
    ini.GetBool("shadow", "dummies", s.dummies);
    ini.GetBool("shadow", "offscreenCasters", s.offscreenCasters);
    ini.GetFloat("shadow", "offscreenDynamicRange", s.offscreenDynamicRange);
    ini.GetBool("shadow", "offscreenPeds", s.offscreenPeds);
    ini.GetBool("shadow", "water", s.water);
    ini.GetBool("shadow", "suppressGtaShadows", s.suppressGtaShadows);
    ini.GetFloat("shadow", "prelitDirectShare", s.prelitDirectShare);
    ini.GetFloat("shadow", "sunBoost", s.sunBoost);
    if (ini.GetColor("shadow", "tint", s.tint)) s.tintOverride = true;

    ini.GetFloat("sun", "updateThreshold", sun.updateThreshold);
    ini.GetBool("sun", "freeze", sun.freeze);
    ini.GetFloat("sun", "freezeHour", sun.freezeHour);

    ini.GetBool("bloom", "enabled", bloom.enabled);
    ini.GetFloat("bloom", "intensity", bloom.intensity);
    ini.GetFloat("bloom", "threshold", bloom.threshold);
    ini.GetFloat("bloom", "radius", bloom.radius);
    ini.GetBool("ssao", "enabled", ssao.enabled);
    ini.GetFloat("ssao", "strength", ssao.strength);
    ini.GetFloat("ssao", "radius", ssao.radius);
    ini.GetBool("fog", "enabled", fog.enabled);
    ini.GetFloat("fog", "density", fog.density);
    ini.GetBool("godray", "enabled", godray.enabled);
    ini.GetFloat("godray", "intensity", godray.intensity);
    ini.GetBool("rain", "wetRoad", rain.wetRoad);
    ini.GetFloat("rain", "wetness", rain.wetness);

    ini.GetBool("debug", "log", debug.log);
    ini.GetBool("debug", "showCascade", debug.showCascade);
    ini.GetBool("debug", "showShadowMap", debug.showShadowMap);
    ini.GetBool("debug", "showDepth", debug.showDepth);
    ini.GetBool("debug", "showSSAO", debug.showSsao);
    ini.GetBool("debug", "showSunDirection", debug.showSunDirection);
    ini.GetBool("debug", "freezeSun", sun.freeze);
    ini.GetBool("debug", "freezeShadowCamera", debug.freezeShadowCamera);
    ini.GetBool("debug", "perfCounters", debug.perfCounters);

    ini.GetBool("performance", "adaptive", performance.adaptive);
    ini.GetFloat("performance", "targetFps", performance.targetFps);

    Validate();
}

bool GraphicsConfig::Load(const char* path) {
    *this = GraphicsConfig{};
    ApplyQualityPreset(GraphicsQuality::High);
    IniFile ini;
    if (!ini.Load(path)) {
        Validate();
        GFX_LOGW(kTag, "%s not found, using built-in HIGH defaults", path ? path : "(null)");
        return false;
    }
    ApplyIni(ini);
    GFX_LOGI(kTag, "loaded %s (%zu keys)", path, ini.Size());
    return true;
}

void GraphicsConfig::LogSummary() const {
    GFX_LOGI(kTag, "enabled=%d quality=%s exposure=%.2f gamma=%.2f saturation=%.2f contrast=%.2f",
             graphics.enabled, QualityName(graphics.quality), graphics.exposure, graphics.gamma,
             graphics.saturation, graphics.contrast);
    const ShadowSettings& s = shadow;
    GFX_LOGI(kTag, "shadow enabled=%d cascades=%d resolution=%d distance=%.0f strength=%.2f lambda=%.2f pcf=%d blend=%d",
             s.enabled, s.cascades, s.resolution, s.distance, s.strength, s.splitLambda, s.pcf, s.cascadeBlend);
    GFX_LOGI(kTag, "shadow bias depth=%.3fm normal=%.2ftx slope=%.2f/%.2f extend=%.0fm minElev=%.1f alphaCut=%.2f maxCasters=%d depth24=%d",
             s.depthBias, s.normalBias, s.slopeBias, s.slopeUnits, s.casterExtend, s.minElevation, s.alphaCutoff,
             s.maxCasters, s.depth24);
    GFX_LOGI(kTag, "casters buildings=%d objects=%d vehicles=%d peds=%d weapons=%d dummies=%d offscreen=%d suppressGta=%d water=%d",
             s.buildings, s.objects, s.vehicles, s.peds, s.weapons, s.dummies, s.offscreenCasters, s.suppressGtaShadows,
             s.water);
    GFX_LOGI(kTag, "debug log=%d showCascade=%d showShadowMap=%d freezeSun=%d freezeShadowCamera=%d perf=%d adaptive=%d target=%.0f",
             debug.log, debug.showCascade, debug.showShadowMap, sun.freeze, debug.freezeShadowCamera,
             debug.perfCounters, performance.adaptive, performance.targetFps);
}

} // namespace gfx
