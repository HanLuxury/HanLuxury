#include "GraphicsSettings.h"
#include "postfx/EglPostFX.h"
#include "sun/WorldSunShadow.h"
#include "TextureFilter.h"
#include "WorldMsaa.h"
#include "../main.h"
#include "../CSettings.h"
#include "../game/game.h"
#include "../game/Mobile/MobileSettings/MobileSettings.h"
#include "../vendor/ini/config.h"
#include <android/log.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#define GS_LOG(...) __android_log_print(ANDROID_LOG_INFO, "GfxSettings", __VA_ARGS__)

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
    {"postfx",            0,   1, {1,   1,   1,   1}},
    {"sun_shadows",       0,   1, {0,   1,   1,   1}},
    {"shadow_quality",    0,   3, {0,   1,   2,   3}},
    {"shadow_distance",  30, 150, {40,  60,  80, 120}},
    {"soft_shadows",      0,   2, {0,   1,   1,   2}},
    {"ambient_occlusion", 0,   1, {0,   0,   1,   1}},
    {"reflections",       0,   1, {0,   0,   1,   1}},
    {"sun_rays",          0, 100, {30,  50,  60,  70}},
    {"bloom",             0, 100, {30,  40,  45,  50}},
    {"sky",               0, 100, {25,  35,  40,  45}},
    {"wet_roads",         0, 150, {60, 100, 110, 120}},
    {"weather_look",      0,   1, {1,   1,   1,   1}},
    {"fog",               0, 100, {60,  70,  80,  85}},
    {"grass",             0,   1, {0,   1,   1,   1}},
    {"fxaa",              0,   1, {1,   1,   1,   1}},
    {"msaa",              0,   2, {0,   0,   0,   1}},
    {"motion_blur",       0, 100, {0,   0,   0,  30}},
    {"dof",               0, 100, {0,   0,  20,  35}},
    {"anisotropic",       0,   4, {1,   2,   3,   4}},
    {"auto_quality",      0,   1, {1,   1,   1,   1}},
}};
// "Rendah" = the cheap but still pretty look for weak phones: no sun-shadow
// replay, no SSAO/SSR, but grading, bloom, haze, sky, wet roads and FXAA.
constexpr int32_t kDefaultPreset=1;
std::array<int32_t,COUNT> values{};
bool initialised=false;
std::atomic<bool> dirty{false}; // set on the game thread, consumed by a save on any thread

bool Valid(int32_t id) { return id>=0 && id<COUNT; }
// Unknown until the render thread checked the GL extensions: shown meanwhile.
std::atomic<int> msaaSupport{-1},anisoSupport{-1};

// ---------------------------------------------------------------- device
struct Device {
    bool detected=false;
    int ramMb=0,cores=0;
};
Device device;
std::atomic<int> gpuTier{-1};
std::atomic<int> adaptiveLevel{0};
std::atomic<bool> adaptiveEnabled{true};

void DetectDevice() {
    if(device.detected) return;
    device.detected=true;
    struct sysinfo info{};
    if(sysinfo(&info)==0) device.ramMb=int((uint64_t(info.totalram)*info.mem_unit)>>20);
    device.cores=int(sysconf(_SC_NPROCESSORS_CONF));
    GS_LOG("device: RAM %d MB, %d CPU cores",device.ramMb,device.cores);
}

// ---------------------------------------------------------------- tuning
// Base look; ZyZGfx.ini overrides it. The menu percentages scale these.
struct Tuning {
    EglPostFX::Settings fx;
    float bloom=0.8f,rays=0.5f,flare=0.09f,ssrScale=1.0f;
    float shadowDarkness=0.55f;
    bool loaded=false;
};
Tuning tuning;

void LoadTuning() {
    tuning=Tuning{};
    auto& fx=tuning.fx;
    // Built-in look matching the reference photos: soft filmic tone, mild
    // contrast, sunlit haze, gentle eye adaptation.
    fx.toneMix=0.5f;fx.exposure=1.0f;fx.saturation=1.02f;fx.contrast=0.95f;
    if(!g_pszStorage) return;
    std::snprintf(fx.textureDir,sizeof(fx.textureDir),"%sgraphics/textures/",g_pszStorage);
    char path[256];
    std::snprintf(path,sizeof(path),"%sgraphics/ZyZGfx.ini",g_pszStorage);
    ini_table_s* t=ini_table_create();
    if(!ini_table_read_from_file(t,path)) { ini_table_destroy(t);GS_LOG("no %s, built-in look",path);return; }
    auto has=[&](const char* s,const char* k) { return ini_table_check_entry(t,s,k); };
    auto F=[&](const char* s,const char* k,float def) {
        if(!has(s,k)) return def;
        const float v=ini_table_get_entry_as_float(t,s,k,def);
        return std::isfinite(v) ? v : def;
    };
    auto On=[&](const char* s) { return !has(s,"Enable") || F(s,"Enable",1.0f)!=0.0f; };

    fx.toneMix=F("Post","Tonemap",fx.toneMix);
    fx.exposure=F("Post","Exposure",fx.exposure);
    fx.saturation=F("Post","Saturation",fx.saturation);
    fx.contrast=F("Post","Contrast",fx.contrast);
    if(has("Post","Sharpen")) fx.clarity=std::clamp(F("Post","Sharpen",0)*100.0f,0.0f,0.6f);
    if(On("FSR") && has("FSR","Sharpness")) fx.clarity=std::max(fx.clarity,0.15f*F("FSR","Sharpness",0.8f));
    fx.grain=F("Post","FilmGrain",fx.grain);
    fx.dither=F("Post","Dither",fx.dither);
    fx.gradeStrength=On("Grading") ? F("Grading","Strength",1.0f) : 0.0f;

    // ZyZ uses HDR units (threshold 1.5 ~ our linear 0.70).
    const float threshold=F("Bloom","Threshold",1.5f);
    tuning.bloom=On("Bloom") ? 0.8f*F("Bloom","Intensity",0.6f)/0.6f : 0.0f;
    fx.bloomThreshold=threshold*0.47f;
    fx.bloomKnee=F("Bloom","SoftKnee",0.3f)*0.67f;
    fx.nightThreshold=std::clamp(F("Bloom","NightThreshold",0.78f)/std::max(threshold,0.01f),0.1f,1.0f);
    fx.nightGlow=F("Bloom","NightGlow",4.0f)/4.0f;

    fx.autoExposure=On("AutoExposure") ? F("AutoExposure","Strength",fx.autoExposure) : 0.0f;
    fx.aeKey=F("AutoExposure","Key",fx.aeKey);
    fx.aeMin=F("AutoExposure","Min",fx.aeMin);
    fx.aeMax=F("AutoExposure","Max",fx.aeMax);
    fx.aeSpeed=F("AutoExposure","Speed",fx.aeSpeed);

    const float rays=F("SunRays","Intensity",0.6f)/0.6f;
    tuning.rays=On("SunRays") ? 0.5f*rays : 0.0f;
    tuning.flare=On("SunRays") ? 0.09f*rays : 0.0f;
    fx.rayLength=F("SunRays","Density",fx.rayLength);
    fx.rayDecay=F("SunRays","Decay",fx.rayDecay);

    if(On("Fog")) {
        fx.hazeDensity=F("Fog","Density",fx.hazeDensity);
        fx.hazeFalloff=F("Fog","HeightFalloff",fx.hazeFalloff);
        fx.hazeBaseHeight=F("Fog","BaseHeight",fx.hazeBaseHeight);
        fx.hazeUniform=F("Fog","HazeDensity",fx.hazeUniform);
        fx.hazeStart=F("Fog","HazeStart",fx.hazeStart);
        fx.hazeMax=F("Fog","MaxOpacity",fx.hazeMax);
        fx.hazeFoggy=F("Fog","FoggyWeather",fx.hazeFoggy);
    } else {
        fx.hazeDensity=fx.hazeUniform=0.0f;
    }

    const char* folder=ini_table_get_entry(t,"LUT","Folder");
    const char* file=ini_table_get_entry(t,"LUT","File");
    if(file && *file) {
        std::snprintf(fx.lutPath,sizeof(fx.lutPath),"%sgraphics/%s%s%s",g_pszStorage,
                      folder ? folder : "",folder && *folder ? "/" : "",file);
        fx.lutStrength=F("LUT","Strength",1.0f);
    }

    fx.aoStrength=0.65f*F("AO","Strength",1.0f);
    fx.aoRadius=F("AO","Radius",1.5f)/1.5f;
    tuning.shadowDarkness=std::clamp(F("Shadows","Strength",tuning.shadowDarkness),0.0f,0.85f);
    tuning.ssrScale=On("SSR") ? F("SSR","Strength",1.0f) : 0.0f;
    if(On("Wet")) {
        fx.wetReflection=F("Wet","Reflection",1.0f);
        fx.wetPuddles=F("Wet","Puddles",1.0f);
        fx.wetRipples=F("Wet","Ripples",1.0f);
        fx.wetForce=F("Wet","Force",0.0f);
    } else {
        fx.wetReflection=fx.wetPuddles=fx.wetRipples=0.0f;
    }
    tuning.loaded=true;
    ini_table_destroy(t);
    GS_LOG("loaded %s (sections HDR/TAA/Vehicle/Water/Lighting/Wind/Relief/Surfaces/Rain/"
           "ExperimentalLights are not used by this client)",path);
}

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
bool Visible(int32_t id) {
    if(id==MSAA) return msaaSupport.load()!=0;
    if(id==ANISOTROPIC) return anisoSupport.load()!=0;
    return Valid(id);
}
void SetMsaaSupport(bool supported) { msaaSupport.store(supported ? 1 : 0); }
void SetAnisotropicSupport(bool supported) { anisoSupport.store(supported ? 1 : 0); }

void SetGpu(const char* renderer) {
    if(!renderer) return;
    // Weak GPUs common in budget phones; mid-range ones get mid tier.
    static constexpr const char* kLow[]={"Mali-T","Mali-4","Mali-G31","Mali-G51","Mali-G52","Mali-G57",
        "PowerVR","Adreno (TM) 3","Adreno (TM) 4","Adreno (TM) 5","Adreno (TM) 610","Adreno (TM) 612",
        "Adreno (TM) 613"};
    static constexpr const char* kMid[]={"Mali-G68","Mali-G71","Mali-G72","Mali-G76","Mali-G77",
        "Adreno (TM) 6"};
    int tier=2;
    for(const char* m:kMid) if(std::strstr(renderer,m)) tier=1;
    for(const char* m:kLow) if(std::strstr(renderer,m)) tier=0;
    gpuTier.store(tier);
    GS_LOG("GPU %s -> tier %d (overall %d)",renderer,tier,Tier());
}
int Tier() {
    DetectDevice();
    int tier=2;
    if(device.ramMb>0 && device.ramMb<3500) tier=0;
    else if(device.ramMb>0 && device.ramMb<5500) tier=1;
    if(device.cores>0 && device.cores<=4) tier=0;
    const int gpu=gpuTier.load();
    if(gpu>=0) tier=std::min(tier,gpu);
    return tier;
}
bool LowMemory() { DetectDevice();return device.ramMb>0 && device.ramMb<3500; }
int ShadowResolutionCap() {
    DetectDevice();
    if(device.ramMb>0 && device.ramMb<3500) return 1024;
    if(device.ramMb>0 && device.ramMb<5500) return 2048;
    return 4096;
}
bool AdaptiveEnabled() { return adaptiveEnabled.load(std::memory_order_relaxed); }
// Same limit ApplyFPSPatch (game/patches.cpp) gives RsGlobal->maxFPS.
int TargetFps() {
    int fps=std::clamp(CSettings::m_Settings.iFPS,20,std::max(CSettings::maxFps,60));
    if(CMobileSettings::ms_MobileSettings[MS_FrameLimiter].value!=0) fps=std::min(fps,30);
    return fps;
}
int AdaptiveLevel() { return adaptiveLevel.load(std::memory_order_relaxed); }
void SetAdaptiveLevel(int level) { adaptiveLevel.store(std::clamp(level,0,3),std::memory_order_relaxed); }

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
    LoadPreset(Tier()>=2 ? 2 : kDefaultPreset);
    dirty=true;
    Apply();
}
bool ConsumeDirty() { return dirty.exchange(false); }

void Apply() {
    EnsureInitialised();
    // Post-processing: the tuned base look (built-in or ZyZGfx.ini), then the
    // menu toggles and strengths on top.
    EglPostFX::Settings fx=tuning.fx;
    fx.enabled=values[POSTFX]!=0;
    fx.ssao=values[AMBIENT_OCCLUSION]!=0;
    fx.ssr=values[REFLECTIONS]!=0;
    fx.ssrStrength*=tuning.ssrScale;
    fx.sunShafts=tuning.rays*Percent(SUN_RAYS);
    fx.lensFlare=tuning.flare*Percent(SUN_RAYS);
    fx.bloomStrength=tuning.bloom*Percent(BLOOM);
    fx.skyStrength=Percent(SKY);
    fx.wetStrength=Percent(WET_ROADS);
    fx.weatherLook=values[WEATHER_LOOK]!=0;
    fx.fogStrength=Percent(FOG);
    fx.fxaa=values[FXAA]!=0;
    fx.motionBlur=Percent(MOTION_BLUR);
    fx.dofStrength=Percent(DOF);
    fx.lowMemory=LowMemory();
    EglPostFX::SetSettings(fx);

    WorldSunShadow::Settings sun;
    sun.enabled=values[SUN_SHADOWS]!=0;
    static constexpr int kResolution[4]={512,1024,2048,4096};
    static constexpr float kSoftness[3]={0.6f,1.0f,1.8f};
    sun.resolution=std::min(kResolution[std::clamp(values[SHADOW_QUALITY],0,3)],ShadowResolutionCap());
    sun.radius=float(values[SHADOW_DISTANCE]);
    sun.softness=kSoftness[std::clamp(values[SOFT_SHADOWS],0,2)];
    sun.darkness=tuning.shadowDarkness;
    // Every caster costs a second draw on the render thread: fewer on weak CPUs.
    const int tier=Tier();
    sun.maxDraws=tier==0 ? 1200 : (values[SHADOW_DISTANCE]>=100 ? 4000 : 3000);
    WorldSunShadow::SetSettings(sun);

    TextureFilter::SetLevel(values[ANISOTROPIC]);
    WorldMsaa::SetLevel(values[MSAA]);
    CGame::ms_bUpdatePlants=values[GRASS]!=0;

    adaptiveEnabled.store(values[ADAPTIVE]!=0);
    if(values[ADAPTIVE]==0) adaptiveLevel.store(0);
}

void Load(ini_table_s* config) {
    EnsureInitialised();
    LoadTuning();
    if(config) {
        // First start (no saved preset): the preset that fits this phone.
        const int32_t fallback=Tier()>=2 ? 2 : kDefaultPreset;
        const bool saved=ini_table_check_entry(config,"graphics",kSpecs[PRESET].key);
        const int32_t preset=saved ? ini_table_get_entry_as_int(config,"graphics",kSpecs[PRESET].key,fallback) : fallback;
        LoadPreset(preset>=0 && preset<4 ? preset : fallback);
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
