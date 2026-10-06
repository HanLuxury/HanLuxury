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
#include "graphics/GraphicsEngine.h"

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
    // Preserve GTA's full effects order, as in the Alyn native render loop.
    if (RenderEffectsOriginal)
        RenderEffectsOriginal();
    DebugModules::Render3D();
    CCEF3D::ApplyPendingTexture();
}

void Render2dStuff()
{
    // 3D -> HUD boundary (this client redirects the native Render2dStuff).
    // EAGLE graphics: shadow receivers are switched off here (queued on the
    // RenderQueue thread) so the HUD, SA-MP UI, CEF and text are never
    // shadowed; the post-process composite (phase 6) is queued at this point too.
    gfx::GraphicsEngine::Get().OnEndWorld();
    if( CHook::CallFunction<bool>(g_libGTASA + (VER_x32 ? 0x001BB7F4 + 1 : 0x24EA90)) ) // emu_IsAltRenderTarget()
        CHook::CallFunction<void>(g_libGTASA + (VER_x32 ? 0x001BC20C + 1 : 0x24F5B8)); // emu_FlushAltRenderTarget()

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

    CHook::CallFunction<void>(g_libGTASA + (VER_x32 ? 0x1C0750 + 1 : 0x252CE4), 1); // textdraw text
    ((void (*)(bool)) (g_libGTASA + (VER_x32 ? 0x0054BDD4 + 1 : 0x66B678)))(1u); // CMessages::Display - gametext
    ((void (*)(bool)) (g_libGTASA + (VER_x32 ? 0x005A9120 + 1 : 0x6CCEA0)))(1u); // CFont::RenderFontBuffer
}
