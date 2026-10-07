#include "GraphicsSettings.h"
#include "postfx/EglPostFX.h"
#include "sun/WorldSunShadow.h"
#include "../game/game.h"
#include "../vendor/ini/config.h"
#include <algorithm>
#include <array>
#include <atomic>

namespace GraphicsSettings {
namespace {
struct Spec {
    const char* key;                 // settings.ini [graphics] key
    int32_t min,max;
    std::array<int32_t,4> preset;    // Rendah, Sedang, Tinggi, Ultra
};
// PRESET's own row is filled at run time. Defaults = "Sedang": stable FPS on
// mid-range phones, all fixes active. Ultra is meant for flagship GPUs.
constexpr std::array<Spec,COUNT> kSpecs{{
    {"preset",            0,   4, {0,   1,   2,   3}},
    {"postfx",            0,   1, {0,   1,   1,   1}},
    {"sun_shadows",       0,   1, {0,   1,   1,   1}},
    {"shadow_quality",    0,   3, {0,   1,   2,   3}},
    {"shadow_distance",  30, 150, {40,  60,  80, 120}},
    {"soft_shadows",      0,   2, {0,   1,   1,   2}},
    {"ambient_occlusion", 0,   1, {0,   0,   1,   1}},
    {"reflections",       0,   1, {0,   0,   1,   1}},
    {"sun_rays",          0, 100, {0,  40,  50,  60}},
    {"bloom",             0, 100, {25, 35,  40,  45}},
    {"sky",               0, 100, {0,  30,  35,  45}},
    {"wet_roads",         0, 150, {0, 100, 100, 120}},
    {"weather_look",      0,   1, {1,   1,   1,   1}},
    {"fog",               0, 100, {50, 60,  65,  70}},
    {"grass",             0,   1, {0,   1,   1,   1}},
    {"fxaa",              0,   1, {0,   1,   1,   1}},
}};
constexpr int32_t kDefaultPreset=1;
std::array<int32_t,COUNT> values{};
bool initialised=false;
std::atomic<bool> dirty{false}; // set on the game thread, consumed by a save on any thread

bool Valid(int32_t id) { return id>=0 && id<COUNT; }
void LoadPreset(int32_t preset) {
    preset=std::clamp(preset,0,3);
    for(int32_t i=1;i<COUNT;++i) values[i]=kSpecs[i].preset[preset];
    values[PRESET]=preset;
}
void EnsureInitialised() {
    if(initialised) return;
    LoadPreset(kDefaultPreset);
    initialised=true;
}
// Kustom unless every value equals one preset.
int32_t MatchingPreset() {
    for(int32_t p=0;p<4;++p) {
        bool same=true;
        for(int32_t i=1;i<COUNT && same;++i) same=values[i]==kSpecs[i].preset[p];
        if(same) return p;
    }
    return kPresetCustom;
}
float Percent(int32_t id) { return float(values[id])/100.0f; }
} // namespace

int32_t Get(int32_t id) { EnsureInitialised();return Valid(id) ? values[id] : 0; }
int32_t Min(int32_t id) { return Valid(id) ? kSpecs[id].min : 0; }
int32_t Max(int32_t id) { return Valid(id) ? kSpecs[id].max : 0; }

void Set(int32_t id,int32_t value) {
    EnsureInitialised();
    if(!Valid(id)) return;
    value=std::clamp(value,kSpecs[id].min,kSpecs[id].max);
    if(id==PRESET) {
        if(value==kPresetCustom || value==values[PRESET]) return; // Kustom is a result, not a choice
        LoadPreset(value);
    } else {
        if(values[id]==value) return;
        values[id]=value;
        values[PRESET]=MatchingPreset();
    }
    dirty=true;
    Apply();
}
void ResetDefaults() {
    EnsureInitialised();
    LoadPreset(kDefaultPreset);
    dirty=true;
    Apply();
}
bool ConsumeDirty() { return dirty.exchange(false); }

void Apply() {
    EnsureInitialised();
    // Post-processing: start from the system's own defaults so tuning that is
    // not exposed in the menu keeps the values chosen in EglPostFX.h.
    EglPostFX::Settings fx;
    fx.enabled=values[POSTFX]!=0;
    fx.ssao=values[AMBIENT_OCCLUSION]!=0;
    fx.ssr=values[REFLECTIONS]!=0;
    fx.sunShafts=0.5f*Percent(SUN_RAYS);
    fx.lensFlare=0.09f*Percent(SUN_RAYS);
    fx.bloomStrength=0.8f*Percent(BLOOM);
    fx.skyStrength=Percent(SKY);
    fx.wetStrength=Percent(WET_ROADS);
    fx.weatherLook=values[WEATHER_LOOK]!=0;
    fx.fogStrength=Percent(FOG);
    fx.fxaa=values[FXAA]!=0;
    EglPostFX::SetSettings(fx);

    WorldSunShadow::Settings sun;
    sun.enabled=values[SUN_SHADOWS]!=0;
    static constexpr int kResolution[4]={512,1024,2048,4096};
    static constexpr float kSoftness[3]={0.6f,1.0f,1.8f};
    sun.resolution=kResolution[std::clamp(values[SHADOW_QUALITY],0,3)];
    sun.radius=float(values[SHADOW_DISTANCE]);
    sun.softness=kSoftness[std::clamp(values[SOFT_SHADOWS],0,2)];
    // Larger maps cover more casters; the per-frame budget follows.
    sun.maxDraws=values[SHADOW_DISTANCE]>=100 ? 4000 : 3000;
    WorldSunShadow::SetSettings(sun);

    CGame::ms_bUpdatePlants=values[GRASS]!=0;
}

void Load(ini_table_s* config) {
    EnsureInitialised();
    if(config) {
        const int32_t preset=ini_table_get_entry_as_int(config,"graphics",kSpecs[PRESET].key,kDefaultPreset);
        LoadPreset(preset>=0 && preset<4 ? preset : kDefaultPreset);
        for(int32_t i=1;i<COUNT;++i)
            values[i]=std::clamp(ini_table_get_entry_as_int(config,"graphics",kSpecs[i].key,values[i]),
                                 kSpecs[i].min,kSpecs[i].max);
        values[PRESET]=MatchingPreset();
    }
    dirty=false;
    Apply();
}
void Save(ini_table_s* config) {
    EnsureInitialised();
    if(!config) return;
    for(int32_t i=0;i<COUNT;++i) ini_table_create_entry_as_int(config,"graphics",kSpecs[i].key,values[i]);
}
} // namespace GraphicsSettings
