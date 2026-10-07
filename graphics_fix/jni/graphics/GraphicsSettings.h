#pragma once
#include <cstdint>

struct ini_table_s;

// Options of the client's existing graphics system (EglPostFX, WorldSunShadow,
// CPlantMgr update, texture filtering, world MSAA), shown as extra rows in the
// GRAFIS tab of the existing ModernPauseMenu and stored in the existing
// settings.ini ([graphics]) through CSettings. Nothing here renders; Apply()
// only hands values to the systems.
//
// Optional tuning file: TESTLIT/graphics/ZyZGfx.ini (read once at start). It
// sets the base look (tonemap, exposure, bloom, haze, LUT, ...); the menu
// values scale/toggle on top of it.
//
// Set/Load/Save/Apply run on the game thread (CSettings load/save, ModernMenu
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
    FOG,               // percent (weather fog and aerial haze)
    GRASS,             // procedural grass/plants follow the camera
    FXAA,              // anti-aliasing in the post-processing composite
    MSAA,              // 0 off, 1 = 2x, 2 = 4x multisampling of the 3D world
    MOTION_BLUR,       // camera motion blur, percent
    DOF,               // depth of field (distance blur), percent
    ANISOTROPIC,       // texture filtering: 0 off, 1 = 2x, 2 = 4x, 3 = 8x, 4 = 16x
    ADAPTIVE,          // lower heavy effects automatically while the FPS is low
    COUNT
};
// Menu setting ids: GTA's MobileSettings use 0..MS_MAX-1 (37); these start here.
constexpr int32_t kMenuBase = 64;
constexpr int32_t kPresetCustom = 4;

int32_t Get(int32_t id);
int32_t Min(int32_t id);
int32_t Max(int32_t id);
// False when this device cannot do the option (the menu hides the row).
bool Visible(int32_t id);
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

// Device information (any thread). Tier: 0 low, 1 mid, 2 high.
void SetMsaaSupport(bool supported);
void SetAnisotropicSupport(bool supported);
void SetGpu(const char* renderer); // render thread, GL_RENDERER
int Tier();
bool LowMemory();
int ShadowResolutionCap();

// Adaptive quality: the render thread measures the frame rate and sets a
// level 0..3; WorldSunShadow/EglPostFX lower their heaviest work with it.
bool AdaptiveEnabled();
int TargetFps();
int AdaptiveLevel();
void SetAdaptiveLevel(int level);
} // namespace GraphicsSettings
