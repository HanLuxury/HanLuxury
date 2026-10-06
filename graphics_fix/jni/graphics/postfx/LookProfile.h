#pragma once
#include "EglPostFX.h"
#include <algorithm>
#include <cmath>

namespace EglPostFX {
// Visual traits of one eWeatherType (game/Enums/eWeatherType.h).
struct WeatherTraits {
    float clear=0.7f,overcast=0,rain=0,fog=0,smog=0,sand=0,warm=0,cool=0;
};
inline WeatherTraits TraitsFor(int type) {
    WeatherTraits t;
    switch(type) {
        case 0: case 6: case 11: case 13: case 17: t.clear=1.0f;break;            // EXTRASUNNY_*
        case 1: case 5: case 10: case 14: case 18: t.clear=0.8f;break;            // SUNNY_*
        case 2: t.clear=0.85f;t.smog=1.0f;break;                                   // EXTRASUNNY_SMOG_LA
        case 3: t.clear=0.6f;t.smog=1.0f;break;                                    // SUNNY_SMOG_LA
        case 4: case 7: case 12: case 15: t.clear=0.15f;t.overcast=1.0f;break;     // CLOUDY_*
        case 8: case 16: t.clear=0.0f;t.overcast=1.0f;t.rain=1.0f;break;           // RAINY_*
        case 9: t.clear=0.1f;t.overcast=0.6f;t.fog=1.0f;break;                     // FOGGY_SF
        case 19: t.clear=0.2f;t.sand=1.0f;break;                                   // SANDSTORM_DESERT
        default: break;                                                            // UNDERWATER, EXTRACOLOURS, unknown
    }
    if(type>=10&&type<=12) t.warm=0.5f;      // Las Venturas
    if(type>=17&&type<=19) t.warm=1.0f;      // desert
    if(type>=5&&type<=9) t.cool=0.5f;        // San Fierro
    return t;
}
inline WeatherTraits MixTraits(const WeatherTraits& a,const WeatherTraits& b,float t) {
    auto m=[t](float x,float y){return x+(y-x)*t;};
    WeatherTraits r;
    r.clear=m(a.clear,b.clear);r.overcast=m(a.overcast,b.overcast);r.rain=m(a.rain,b.rain);r.fog=m(a.fog,b.fog);
    r.smog=m(a.smog,b.smog);r.sand=m(a.sand,b.sand);r.warm=m(a.warm,b.warm);r.cool=m(a.cool,b.cool);
    return r;
}

struct LookProfile {
    Settings config;
    float shadow[3]{1,1,1}, highlight[3]{1,1,1};
    float fog[4]{0.25f,0.29f,0.31f,0};
    float detail[4]{},sun[4]{0.5f,0.8f,0,0};
    float skyZenith[4]{};    // rgb, w = sky tint strength
    float skyHorizon[4]{};   // rgb, w = horizon haze
    float sunColor[4]{};     // rgb glow colour, w = glow strength
    float sunDirection[4]{0,0,1,0}; // world direction to the sun, w = visibility
    float wet[4]{};          // wetness, rain, puddle coverage, reflection boost
};
inline float Unit(float x) { return std::isfinite(x) ? std::clamp(x,0.0f,1.0f) : 0.0f; }
inline float Smooth(float a,float b,float x) {
    float t=Unit((x-a)/(b-a));return t*t*(3-2*t);
}
inline FrameEnvironment CleanEnvironment(FrameEnvironment e) {
    if(!std::isfinite(e.hour) || e.hour<0 || e.hour>=24) { e.valid=false;e.hour=12; }
    e.rain=Unit(e.rain);e.wetness=Unit(e.wetness);e.cloud=Unit(e.cloud);e.fog=Unit(e.fog);
    e.underwater=Unit(e.underwater);e.tunnel=Unit(e.tunnel);e.sunGlare=Unit(e.sunGlare);
    e.weatherBlend=Unit(e.weatherBlend);
    // The sun may sit slightly outside the screen: rays still enter from the edge.
    if(!std::isfinite(e.sunUV[0]) || !std::isfinite(e.sunUV[1]) ||
       e.sunUV[0]<-1 || e.sunUV[0]>2 || e.sunUV[1]<-1 || e.sunUV[1]>2) {
        e.sunVisible=false;e.sunUV[0]=0.5f;e.sunUV[1]=0.8f;
    }
    float length=0;
    for(float v:e.toSun) length+=std::isfinite(v) ? v*v : 0.0f;
    if(length<1e-6f) { e.toSun[0]=e.toSun[1]=0;e.toSun[2]=1; }
    else { length=std::sqrt(length);for(float& v:e.toSun) v=std::isfinite(v) ? v/length : 0.0f; }
    return e;
}
inline WeatherTraits EnvironmentTraits(const FrameEnvironment& e) {
    if(e.oldWeather<0&&e.newWeather<0) return {};
    const int from=e.oldWeather<0 ? e.newWeather : e.oldWeather;
    const int to=e.newWeather<0 ? from : e.newWeather;
    return MixTraits(TraitsFor(from),TraitsFor(to),e.weatherBlend);
}
inline void Mix3(float* out,const float* a,const float* b,float t) {
    for(int i=0;i<3;++i) out[i]=a[i]+(b[i]-a[i])*t;
}
// sunVisibility: smoothed 0..1 (day, clouds over the sun, screen position).
inline LookProfile BuildLook(const Settings& settings,const FrameEnvironment& input,
                             const WeatherTraits& weatherInput,float sunVisibility) {
    const auto e=CleanEnvironment(input);
    const WeatherTraits w=settings.weatherLook ? weatherInput : WeatherTraits{};
    float day=1,night=0,golden=0,rain=0,wet=0;
    if(e.valid) {
        day=Smooth(5.0f,7.25f,e.hour)*(1-Smooth(18.0f,20.0f,e.hour));night=1-day;
        golden=std::max(1-std::abs(e.hour-6.5f)/1.6f,1-std::abs(e.hour-18.0f)/1.7f);
        golden=Unit(golden)*(1-e.cloud)*(1-e.rain)*(1-0.7f*w.overcast);
        rain=Unit(e.rain*0.8f+e.fog*0.4f+e.cloud*0.12f);
        wet=std::max(e.wetness,e.rain*0.6f);
    }
    switch(settings.preset) {
        case LookPreset::Day: day=1;night=golden=rain=0;break;
        case LookPreset::GoldenHour: day=1;golden=1;night=rain=0;break;
        case LookPreset::Night: night=1;day=golden=rain=0;break;
        case LookPreset::Rain: day=1;night=golden=0;rain=wet=1;break;
        default: break;
    }
    LookProfile p;p.config=settings;auto& c=p.config;
    c.exposure*=1+night*0.10f+golden*0.04f-rain*0.08f-w.overcast*0.03f*day;
    c.saturation*=1-rain*0.18f-night*0.04f-w.overcast*0.08f-w.fog*0.12f-w.sand*0.10f-w.smog*0.06f;
    c.contrast*=1-rain*0.04f-w.fog*0.08f-w.sand*0.06f-w.overcast*0.04f;
    c.bloomStrength*=1+golden*0.75f+night*0.65f+w.clear*day*0.20f;
    c.bloomThreshold*=1-night*0.48f-golden*0.18f;
    c.vibrance+=0.05f*w.clear*day;
    c.ssrStrength*=0.22f+0.78f*std::max(wet,rain);
    p.shadow[0]=1-0.19f*night-0.055f*rain;
    p.shadow[1]=1-0.04f*night+0.015f*rain;
    p.shadow[2]=1+0.19f*night+0.025f*rain+0.03f*w.cool;
    p.highlight[0]=1+0.20f*golden+0.035f*day-0.035f*rain+0.06f*w.smog+0.12f*w.sand+0.04f*w.warm;
    p.highlight[1]=1+0.045f*golden+0.02f*w.smog+0.04f*w.sand;
    p.highlight[2]=1-0.20f*golden-0.04f*day-0.08f*w.smog-0.15f*w.sand-0.04f*w.warm+0.02f*w.cool;
    p.detail[0]=c.clarity;p.detail[1]=c.vibrance;p.detail[2]=c.shadowLift;p.detail[3]=c.vignette;

    // Sky model in linear RGB, from the hour and the blended weather.
    const float zDay[3]={0.16f,0.33f,0.75f},hDay[3]={0.60f,0.72f,0.90f};
    const float zGold[3]={0.20f,0.25f,0.52f},hGold[3]={1.00f,0.58f,0.32f};
    const float zNight[3]={0.008f,0.012f,0.035f},hNight[3]={0.02f,0.03f,0.07f};
    const float zOver[3]={0.42f,0.45f,0.50f},hOver[3]={0.55f,0.57f,0.60f};
    const float zRain[3]={0.28f,0.31f,0.36f},hRain[3]={0.40f,0.43f,0.47f};
    const float fogSky[3]={0.62f,0.64f,0.66f};
    const float zSand[3]={0.62f,0.45f,0.26f},hSand[3]={0.82f,0.62f,0.38f};
    const float hSmog[3]={0.80f,0.68f,0.48f};
    float zenith[3],horizon[3];
    Mix3(zenith,zDay,zGold,golden);Mix3(horizon,hDay,hGold,golden);
    const float grey=std::max(w.overcast,rain*0.8f);
    Mix3(zenith,zenith,zOver,grey*0.85f);Mix3(horizon,horizon,hOver,grey*0.85f);
    Mix3(zenith,zenith,zRain,std::max(w.rain,e.rain)*0.8f);Mix3(horizon,horizon,hRain,std::max(w.rain,e.rain)*0.8f);
    Mix3(zenith,zenith,fogSky,std::max(w.fog,e.fog)*0.7f);Mix3(horizon,horizon,fogSky,std::max(w.fog,e.fog)*0.85f);
    Mix3(zenith,zenith,zSand,w.sand*0.8f);Mix3(horizon,horizon,hSand,w.sand*0.9f);
    Mix3(horizon,horizon,hSmog,w.smog*0.45f);
    Mix3(zenith,zenith,zNight,night);Mix3(horizon,horizon,hNight,night);
    const float under=(1-e.underwater)*(1-e.tunnel);
    std::copy(zenith,zenith+3,p.skyZenith);std::copy(horizon,horizon+3,p.skyHorizon);
    p.skyZenith[3]=c.skyStrength*(0.45f+0.55f*day)*under;
    p.skyHorizon[3]=Unit(0.15f*w.overcast+0.45f*w.fog+0.40f*w.sand+0.20f*w.smog+0.35f*rain)*under;

    // Fog follows the sky horizon, so distant geometry melts into the same haze.
    p.fog[0]=0.25f-0.19f*night+0.09f*golden;
    p.fog[1]=0.29f-0.21f*night+0.02f*golden;
    p.fog[2]=0.31f-0.18f*night-0.06f*golden;
    Mix3(p.fog,p.fog,horizon,settings.weatherLook ? 0.6f : 0.0f);
    p.fog[3]=c.fogStrength*(0.0008f+0.007f*rain+0.010f*w.fog+0.006f*w.sand+0.002f*w.smog+0.0015f*w.overcast)
        *(1-e.underwater)*(1-e.tunnel);

    const float visibility=e.valid ? Unit(sunVisibility)*day*(1-e.underwater)*(1-e.tunnel) : 0.0f;
    p.sun[0]=e.sunUV[0];p.sun[1]=e.sunUV[1];
    p.sun[2]=c.sunShafts*visibility*(1+golden)*(0.75f+0.5f*e.sunGlare);
    p.sun[3]=c.lensFlare*visibility*(1+golden*0.5f);
    const float glowDay[3]={1.0f,0.92f,0.78f},glowGold[3]={1.0f,0.55f,0.25f};
    Mix3(p.sunColor,glowDay,glowGold,golden);
    p.sunColor[3]=c.skyStrength*visibility*(0.55f+0.45f*golden);
    p.sunDirection[0]=e.toSun[0];p.sunDirection[1]=e.toSun[1];p.sunDirection[2]=e.toSun[2];
    p.sunDirection[3]=visibility;

    const float wetness=Unit(wet*c.wetStrength)*(1-e.underwater);
    p.wet[0]=wetness;
    p.wet[1]=Unit(e.rain)*c.wetStrength*(1-e.tunnel);
    p.wet[2]=0.25f+0.45f*wetness;
    p.wet[3]=2.5f;
    return p;
}
} // namespace EglPostFX
