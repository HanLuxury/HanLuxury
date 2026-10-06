#pragma once
#include <cstdint>

struct ini_table_s;

// Options of the client's existing graphics system (EglPostFX, WorldSunShadow,
// CPlantMgr update), shown as extra rows in the GRAFIS tab of the existing
// ModernPauseMenu and stored in the existing settings.ini ([graphics]) through
// CSettings. Nothing here renders; Apply() only hands values to the systems.
//
// All functions run on the game thread (CSettings load/save, ModernMenu
// request processing); the systems' own SetSettings are thread-safe.
namespace GraphicsSettings {
enum Id : int32_t {
    PRESET,            // 0 Rendah, 1 Sedang, 2 Tinggi, 3 Ultra, 4 Kustom
    POSTFX,            // EglPostFX on/off
    SUN_SHADOWS,       // WorldSunShadow on/off
    SHADOW_QUALITY,    // sun map 512/1024/2048/4096 (high resolution shadows)
    SHADOW_DISTANCE,   // metres (long / extended shadow distance)
    SOFT_SHADOWS,      // PCF radius: sharp / soft / very soft
    AMBIENT_OCCLUSION, // SSAO
    REFLECTIONS,       // screen-space reflections
    SUN_RAYS,          // god rays + lens flare, percent
    BLOOM,             // percent
    SKY,               // realtime sky colour/glow, percent
    WET_ROADS,         // wet roads/puddles while raining, percent
    WEATHER_LOOK,      // grading/fog follow the weather in realtime
    FOG,               // percent
    GRASS,             // procedural grass/plants follow the camera
    FXAA,              // anti-aliasing in the post-processing composite
    COUNT
};
// Menu setting ids: GTA's MobileSettings use 0..MS_MAX-1 (37); these start here.
constexpr int32_t kMenuBase = 64;
constexpr int32_t kPresetCustom = 4;

int32_t Get(int32_t id);
int32_t Min(int32_t id);
int32_t Max(int32_t id);
// Clamps, applies live and marks the settings dirty. PRESET writes every
// value of that preset; changing any other value switches PRESET to Kustom.
void Set(int32_t id, int32_t value);
void ResetDefaults();
void Apply();
// True once after a change; the caller saves settings.ini.
bool ConsumeDirty();
// Called by CSettings::LoadSettings / CSettings::save with their ini table.
void Load(ini_table_s* config);
void Save(ini_table_s* config);
} // namespace GraphicsSettings
