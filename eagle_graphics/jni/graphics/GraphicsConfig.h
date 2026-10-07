#pragma once
// EAGLE graphics engine - configuration (Config.ini + Advanced.ini) and quality presets.
//
// Load order: built-in defaults -> preset for [Graphics] Quality -> every key
// present in Config.ini, then Advanced.ini, overrides the preset. Unknown keys
// are ignored, malformed values keep the previous value and are logged.
// Without Config.ini and Advanced.ini the first-release graphics.ini is read.

#include <string>
#include <unordered_map>

namespace gfx {

enum class GraphicsQuality : int { Low = 0, Medium = 1, High = 2, Ultra = 3 };

// INI reader in the SA_DOX/SDX style:
//   [Section]            sections; "[[Group]" / "[Group]]" lines only group sections
//   fDistance = 160.0    typed keys: b bool, i int, f float, s string, c colour, uc byte;
//                        the type letter is dropped, so fDistance == distance
//   # ; //               comment lines ('#' and ';' also end a value)
// Section and key names are case-insensitive.
class IniFile {
public:
    // merge = keep the keys already loaded (a later file overrides them).
    bool Load(const char* path, bool merge = false);
    bool LoadFromString(const std::string& text, bool merge = false);
    static std::string NormalizeKey(const std::string& key);

    bool Has(const char* section, const char* key) const;
    bool GetBool(const char* section, const char* key, bool& out) const;
    bool GetInt(const char* section, const char* key, int& out) const;
    bool GetFloat(const char* section, const char* key, float& out) const;
    bool GetString(const char* section, const char* key, std::string& out) const;
    // "r,g,b" with components 0..1 (or 0..255 if any value > 1).
    bool GetColor(const char* section, const char* key, float out[3]) const;

    size_t Size() const { return m_values.size(); }

private:
    const std::string* Find(const char* section, const char* key) const;
    std::unordered_map<std::string, std::string> m_values;
};

struct GeneralSettings {
    bool enabled = true;
    GraphicsQuality quality = GraphicsQuality::High;
    float exposure = 1.05f;    // used by post-process (phase 6)
    float gamma = 2.20f;
    float saturation = 1.05f;
    float contrast = 1.02f;
};

struct ShadowSettings {
    bool enabled = true;
    int cascades = 3;            // 1..4
    int resolution = 2048;       // per cascade tile, 512..4096
    float distance = 160.0f;     // metres from the camera covered by the last cascade
    float strength = 0.78f;      // overall darkness multiplier (0..1)
    float splitLambda = 0.75f;   // 0 = uniform splits, 1 = logarithmic
    float depthBias = 0.06f;     // metres, constant receiver bias
    float normalBias = 1.5f;     // receiver offset along the normal, in shadow texels
    float slopeBias = 1.6f;      // glPolygonOffset factor while rendering casters
    float slopeUnits = 4.0f;     // glPolygonOffset units while rendering casters
    int pcf = 2;                 // 0 = 1 tap, 1 = 4 taps, 2 = 9 taps
    float pcfSpread = 1.0f;      // PCF tap distance in texels
    bool cascadeBlend = true;    // blend band between cascades
    float blendBand = 0.12f;     // fraction of each cascade used for blending
    float fadeRange = 0.15f;     // fraction of 'distance' used for the final fade-out
    float casterExtend = 140.0f; // metres the light box extends towards the sun
    float minElevation = 9.0f;   // degrees; lower suns are clamped (no infinite shadows)
    float alphaCutoff = 0.45f;   // alpha-tested casters (leaves, fences)
    int maxCasters = 1000;       // per cascade
    bool depth24 = false;        // DEPTH_COMPONENT24 instead of 16
    bool hardwarePcf = true;     // use GL_EXT_shadow_samplers when available
    bool buildings = true;
    bool objects = true;
    bool vehicles = true;
    bool peds = true;
    bool weapons = true;               // weapons held by peds cast shadows
    bool dummies = true;
    bool offscreenCasters = true;      // sector scan around the camera
    float offscreenDynamicRange = 60.0f;
    bool offscreenPeds = false;
    bool water = false;                // water surface receives shadows
    bool suppressGtaShadows = true;    // hide GTA blob/realtime shadows while sun shadows are on
    float prelitDirectShare = 0.85f;   // how much of baked building light is "sun"
    float sunBoost = 0.10f;            // extra sunlight on lit surfaces
    bool tintOverride = false;
    float tint[3] = {0.46f, 0.50f, 0.60f};
};

struct SunSettings {
    float updateThreshold = 0.10f; // degrees the sun must move before the light camera turns
    bool freeze = false;           // keep the current direction
    float freezeHour = -1.0f;      // >= 0: force this hour for the sun only (debug)
};

struct BloomSettings  { bool enabled = true;  float intensity = 0.65f; float threshold = 1.0f; float radius = 1.0f; };
struct SsaoSettings   { bool enabled = true;  float strength = 0.55f;  float radius = 0.7f; };
struct FogSettings    { bool enabled = true;  float density = 1.0f; };
struct GodraySettings { bool enabled = true;  float intensity = 0.35f; };
struct RainSettings   { bool wetRoad = true;  float wetness = 1.0f; };

struct DebugSettings {
    bool log = false;              // debug-level log lines
    bool showCascade = false;      // tint receivers by cascade
    bool showShadowMap = false;    // draw the shadow atlas on screen
    bool showDepth = false;        // reserved (post-process phase)
    bool showSsao = false;         // reserved (post-process phase)
    bool showSunDirection = false; // log sun vector once per second
    bool freezeShadowCamera = false;
    bool perfCounters = false;     // log CPU timings every 5 s
};

struct PerformanceSettings {
    bool adaptive = true;     // reduce shadow distance when FPS stays low
    float targetFps = 30.0f;
};

struct GraphicsConfig {
    GeneralSettings graphics;
    ShadowSettings shadow;
    SunSettings sun;
    BloomSettings bloom;
    SsaoSettings ssao;
    FogSettings fog;
    GodraySettings godray;
    RainSettings rain;
    DebugSettings debug;
    PerformanceSettings performance;

    void ApplyQualityPreset(GraphicsQuality q);
    void Validate();
    // Config.ini, then Advanced.ini on top; legacy graphics.ini only if both are missing.
    // Returns false if no file was found (defaults stay active).
    bool Load(const char* configPath, const char* advancedPath = nullptr, const char* legacyPath = nullptr);
    void ApplyIni(const IniFile& ini);
    void LogSummary() const;
};

const char* QualityName(GraphicsQuality q);

} // namespace gfx
