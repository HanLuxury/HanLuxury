//
// Created on 23.09.2023.
// 
   
#include "app_game.h"
#include "../game/Birds.h"
#include "../game/Skidmarks.h"
#include "../main.h"
#include "../game/Coronas.h"
#include "../game/Clouds.h"
#include "../game/Glass.h"
#include "../game/MovingThings.h"
#include "../game/VisibilityPlugins.h"
#include "../game/WaterLevel.h"
#include "../game/WaterCannons.h"
#include "../game/WeaponEffects.h"
#include "../game/SpecialFX.h"
#include "../game/PointLights.h"
#include "../game/PostEffects.h"
#include "game/Camera.h"
#include "util/patch.h"
#include "keyboard.h"
#include "HUD.h"
#include "game/Widgets/TouchInterface.h"
#include "game/CrossHair.h"
#include "tools/DebugModules.h"
#include "Mirrors.h"
#include "Mobile/MobileSettings/MobileSettings.h"
#include "Weather.h"
#include "Renderer.h"
#include "cef/CEF3D.h"
#include "game/Scene.h"
#include "game/Shadow/Shadows.h"
#include "net/netgame.h"   // CObjectPool::ProcessMaterialText()
#include "graphics/sun/WorldSunShadow.h"
#include "graphics/postfx/EglPostFX.h"
#include "graphics/RenderThread.h"
#include "game/TimeCycle.h"
#include "game/Clock.h"
#include "game/game.h"
#include <algorithm>
#include <cmath>

static WorldSunShadow::Frame GetSunShadowFrame()
{
    WorldSunShadow::Frame frame;
    if(CMirrors::bRenderingReflection || CMirrors::TypeOfMirror || !Scene.m_pRwCamera ||
       CTimeCycle::m_CurrentStoredValue >= 16) return frame;
    // Interiors (SA-MP worlds other than 0): no sunlight, so no sun shadow.
    if(!CGame::CanSeeOutSideFromCurrArea()) return frame;
    // All fields already belong to the client's existing clock/weather/camera
    // integration. No new address lookup or modification of the game clock.
    const auto sun=CTimeCycle::GetVectorToSun();
    const auto position=CCamera::Get().GetPosition();
    const auto front=CCamera::GetActiveCamera().Front;
    const float hour=float(CClock::GetGameClockHours())+float(CClock::GetGameClockMinutes())/60.0f;
    auto unit=[](float value) {return std::isfinite(value) ? std::clamp(value,0.0f,1.0f):0.0f;};
    const float daylight=unit((hour-5.0f)/1.25f)*unit((20.0f-hour)/1.5f)*unit((sun.z-0.01f)/0.18f);
    // CWeather::Update (0x6f11a4..0x6f11dc, 0x6f1a68) sets CloudCoverage=1 for
    // every weather outside {0,1,2,3,5,6,10,11,13,14,17,18} and every id >= 19,
    // and SA-MP's SetWeather makes it jump there in one frame. Before, that
    // cut the sun shadows to 20% at once in most weathers ("graphics gone").
    // Clouds now only soften the sun; with "Grafis ikut cuaca" off they do not
    // count at all. Heavy rain still hides the sun. Eased (~1 s), no popping.
    const bool followWeather=EglPostFX::GetSettings().weatherLook;
    const float cloud=followWeather ? unit(CWeather::CloudCoverage) : 0.0f;
    const float target=daylight*(1-0.75f*unit(CWeather::Rain))*(1-0.35f*cloud)
        *(1-unit(CWeather::UnderWaterness))*(1-unit(CWeather::InTunnelness));
    static float eased=-1.0f;
    eased=eased<0.0f ? target : eased+(target-eased)*0.06f;
    frame.sunlight=eased;
    frame.focus[0]=position.x+front.x*22.0f;
    frame.focus[1]=position.y+front.y*22.0f;
    frame.focus[2]=position.z+front.z*22.0f;
    frame.toSun[0]=sun.x;frame.toSun[1]=sun.y;frame.toSun[2]=sun.z;
    // Too faint to see: skip the whole caster replay.
    frame.valid=frame.sunlight>0.02f;
    return frame;
}

void RenderScene()
{
    const bool underWater = CWeather::UnderWaterness <= 0.0f;

    // Установка состояний рендеринга
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, RWRSTATE(NULL));
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, RWRSTATE(FALSE));
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, RWRSTATE(FALSE));
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, RWRSTATE(FALSE));

    if (!CMirrors::TypeOfMirror) {
        CMovingThings::Render_BeforeClouds();
        CClouds::Render();
    }

    // Восстановление состояний рендеринга
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, RWRSTATE(TRUE));
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, RWRSTATE(TRUE));
    RwRenderStateSet(rwRENDERSTATESHADEMODE, RWRSTATE(rwSHADEMODEGOURAUD));

    if (CMirrors::TypeOfMirror != MIRROR_TYPE_SPHERE_MAP)
        CHook::CallFunction<void>("_ZN14CCarFXRenderer15PreRenderUpdateEv");

    CRenderer::RenderRoads();

    if (CMirrors::TypeOfMirror != MIRROR_TYPE_SPHERE_MAP)
        CCoronas::RenderReflections();

    CRenderer::RenderEverythingBarRoads();

    if (CMirrors::TypeOfMirror != MIRROR_TYPE_SPHERE_MAP) {
        CRenderer::RenderFadingInUnderwaterEntities();
        if (underWater) {
            RwRenderStateSet(rwRENDERSTATECULLMODE, RWRSTATE(rwCULLMODECULLNONE));
            CWaterLevel::RenderWater();
            RwRenderStateSet(rwRENDERSTATECULLMODE, RWRSTATE(rwCULLMODECULLBACK));
        }
    }

    if (CMirrors::TypeOfMirror != MIRROR_TYPE_SPHERE_MAP || CMobileSettings::ms_MobileSettings[MS_CarReflections].value == 3)
        CRenderer::RenderFadingInEntities();

    //BreakManager_c::Render(&g_breakMan, 0);

    if (!CMirrors::bRenderingReflection) {
        // Keep the real RenderWare camera active while the shadow polys are
        // projected. The original code used a tiny near-clip adjustment;
        // retain it because the shadow plane is close to the receiver.
        RwCamera* shadowCam = Scene.m_pRwCamera;
        if (shadowCam) {
            const float oldNear = RwCameraGetNearClipPlane(shadowCam);
            const float oldFar  = RwCameraGetFarClipPlane(shadowCam);
            float z = CCamera::GetActiveCamera().Front.z;
            float v3 = z <= 0.0f ? -z : 0.0f;
            constexpr float kTiny = 5.9604645e-8f;
            float adjustedNear = ((2.0f * kTiny * 0.25f - 2.0f * kTiny) * v3
                                  + 2.0f * kTiny) * (oldFar - oldNear) + oldNear;
            if (!(adjustedNear > 0.0f) || adjustedNear >= oldFar)
                adjustedNear = oldNear;

            RwCameraEndUpdate(shadowCam);
            RwCameraSetNearClipPlane(shadowCam, adjustedNear);
            RwCameraBeginUpdate(shadowCam);

            CShadows::UpdateStaticShadows();
            CShadows::RenderStaticShadows(false);
            CShadows::RenderStoredShadows();

            RwCameraEndUpdate(shadowCam);
            RwCameraSetNearClipPlane(shadowCam, oldNear);
            RwCameraBeginUpdate(shadowCam);
        }
    }

    //BreakManager_c::Render(&g_breakMan, 1);

    if (CMirrors::TypeOfMirror != MIRROR_TYPE_SPHERE_MAP)
        CHook::CallFunction<void>("_ZN9CPlantMgr6RenderEv");

    RwRenderStateSet(rwRENDERSTATECULLMODE, RWRSTATE(rwCULLMODECULLNONE));
    if (!CMirrors::TypeOfMirror) {
        CClouds::RenderBottomFromHeight();
        CWeather::RenderRainStreaks();

        if (CMobileSettings::ms_MobileSettings[MS_Visuals].value == 3)
            CCoronas::RenderSunReflection();
    }

    if (!underWater && CMirrors::TypeOfMirror != MIRROR_TYPE_SPHERE_MAP) {
        RwRenderStateSet(rwRENDERSTATECULLMODE, RWRSTATE(rwCULLMODECULLNONE));
        CWaterLevel::RenderWater();
        RwRenderStateSet(rwRENDERSTATECULLMODE, RWRSTATE(rwCULLMODECULLBACK));
    }
}

namespace {
void (*RenderEffectsOriginal)() = nullptr;
}

void InstallRenderEffectsHook()
{
    CHook::InlineHook("_Z13RenderEffectsv", &RenderEffects, &RenderEffectsOriginal);
}

void RenderEffects()
{
    // End of the opaque world: native RenderScene is done; particles, coronas,
    // glass and CPostEffects::MobileRender (tail of the original) are not yet
    // drawn. The sun-shadow composite and the world depth copy are queued here
    // so they run on GTA's render thread before those passes: particles/glass
    // are not darkened and GTA's own post effects grade the shadowed image.
    GraphicsRenderThread::Install(); // cheap after the first success
    WorldSunShadow::SubmitWorldEnd(GetSunShadowFrame());

    // Preserve GTA's full effects order, as in the Alyn native render loop.
    if (RenderEffectsOriginal)
        RenderEffectsOriginal();
    DebugModules::Render3D();
    CCEF3D::ApplyPendingTexture();
}

#include "graphics/postfx/EglPostFX.h"

void Render2dStuff()
{
    // This boundary is hooked in the Alyn flow; its native RenderScene remains
    // untouched. Composite before an alternate target is flushed and before HUD.
    //
    // arm64: this runs on the GAME thread, which has no EGL context (libGTASA's
    // RenderQueue is multi-threaded; GL runs on GraphicsThread). The existing
    // EglPostFX work is therefore queued as a RenderQueue command and runs on
    // the render thread in order with the game's commands: world end (queued
    // in RenderEffects) -> effects -> alt render target flush -> post-processing -> HUD.
    GraphicsRenderThread::Install(); // cheap after the first success
    if( CHook::CallFunction<bool>(g_libGTASA + (VER_x32 ? 0x001BB7F4 + 1 : 0x24EA90)) ) // emu_IsAltRenderTarget()
        CHook::CallFunction<void>(g_libGTASA + (VER_x32 ? 0x001BC20C + 1 : 0x24F5B8)); // emu_FlushAltRenderTarget()


    // Batas 3D -> HUD yang sudah ada; tidak menambah hook/offset RenderWare.
    // Read existing client values only; never patch weather/time/corona offsets.
    EglPostFX::FrameEnvironment fxEnvironment;
    fxEnvironment.valid=true;
    fxEnvironment.hour=float(CClock::GetGameClockHours())+float(CClock::GetGameClockMinutes())/60.0f;
    fxEnvironment.rain=CWeather::Rain;fxEnvironment.wetness=CWeather::WetRoads;
    fxEnvironment.cloud=CWeather::CloudCoverage;fxEnvironment.fog=CWeather::Foggyness;
    fxEnvironment.underwater=CWeather::UnderWaterness;fxEnvironment.tunnel=CWeather::InTunnelness;
    fxEnvironment.sunGlare=CWeather::SunGlare;
    fxEnvironment.oldWeather=CWeather::OldWeatherType;fxEnvironment.newWeather=CWeather::NewWeatherType;
    fxEnvironment.weatherBlend=CWeather::InterpolationValue;
    if(CTimeCycle::m_CurrentStoredValue < 16) {
        const auto sun=CTimeCycle::GetVectorToSun();
        fxEnvironment.toSun[0]=sun.x;fxEnvironment.toSun[1]=sun.y;fxEnvironment.toSun[2]=sun.z;
    }
    if(!CGame::CanSeeOutSideFromCurrArea()) fxEnvironment.tunnel=1.0f; // interior: no sky/sun effects
    if(RsGlobal && RsGlobal->maximumWidth>0 && RsGlobal->maximumHeight>0) {
        // Fallback only: with a world camera the sun position is projected
        // from GTA's sun vector (this corona position is stale behind the camera).
        fxEnvironment.sunUV[0]=CCoronas::SunScreenX/float(RsGlobal->maximumWidth);
        fxEnvironment.sunUV[1]=1.0f-CCoronas::SunScreenY/float(RsGlobal->maximumHeight);
        // SunGlare is only >0 in EXTRASUNNY weathers; it now scales the rays
        // instead of switching them on/off. Cloud blocking is eased over time.
        fxEnvironment.sunVisible=!CCoronas::SunBlockedByClouds;
    }
    EglPostFX::SubmitBeforeHud(fxEnvironment);

    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, RWRSTATE(FALSE));
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, RWRSTATE(FALSE));
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, RWRSTATE(TRUE));
    RwRenderStateSet(rwRENDERSTATESRCBLEND, RWRSTATE(rwBLENDSRCALPHA));
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, RWRSTATE(rwBLENDINVSRCALPHA));
    RwRenderStateSet(rwRENDERSTATEFOGENABLE, RWRSTATE(rwRENDERSTATENARENDERSTATE));
    RwRenderStateSet(rwRENDERSTATECULLMODE, RWRSTATE(rwCULLMODECULLNONE));

    CGUI::Render();

    CCrossHair::Render();

    if (CHUD::bIsShow) {

        // radar
        auto radar = CTouchInterface::m_pWidgets[WIDGET_RADAR];

        if (radar)
        {

            radar->m_fOriginX = CHUD::radarPos.x;
            radar->m_fOriginY = CHUD::radarPos.y;

            radar->m_fScaleX = CHUD::radarSize.x;
            radar->m_fScaleY = CHUD::radarSize.y;
        }

        ((void (*)()) (g_libGTASA + (VER_x32 ? 0x00437B0C + 1 : 0x51CFF0)))(); // CHud::DrawRadar

        if(!CKeyBoard::m_bEnable)
            ( ( void(*)(bool) )(g_libGTASA + (VER_x32 ? 0x002B0BD8 + 1 : 0x36FB00)) )(false); // CTouchInterface::DrawAll
    }

    CHook::CallFunction<void>(g_libGTASA + (VER_x32 ? 0x1C0750 + 1 : 0x252CE4), 1); // emu_GammaSet(1) (fonts/textdraws)
    ((void (*)(bool)) (g_libGTASA + (VER_x32 ? 0x0054BDD4 + 1 : 0x66B678)))(1u); // CMessages::Display - gametext
    ((void (*)(bool)) (g_libGTASA + (VER_x32 ? 0x005A9120 + 1 : 0x6CCEA0)))(1u); // CFont::RenderFontBuffer
    // emu_GammaSet(0): native Render2dStuff (0x4D8A68) closes the GammaSet(1)
    // bracket after the fonts. Without it the gamma shader flag leaked into the
    // reflection buffer, MobileMenu, fades and the next frame's shaders.
    CHook::CallFunction<void>(g_libGTASA + (VER_x32 ? 0x1C0750 + 1 : 0x252CE4), 0);
}
