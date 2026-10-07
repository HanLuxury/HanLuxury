//
// Created on 26.11.2023.
//

#include "RealTimeShadowManager.h"
#include "util/patch.h"
#include "Entity/Ped/Ped.h"
#include "Mobile/MobileSettings/MobileSettings.h"
#include "Models/ModelInfo.h"
#include "Camera.h"
#include "modloader/HookScope.h"
#include <cstddef>

#if !VER_x32
// Native gate below reads CPed+0x720 (0x6DCFF8); keep the client field in sync.
static_assert(offsetof(CPed, m_nPedType) == 0x720, "CPed::m_nPedType must match libGTASA arm64");
#endif
 
void CRealTimeShadowManager::ReturnRealTimeShadow(CRealTimeShadow *shdw) {
    if (shdw->m_pOwner) {
        shdw->m_pOwner->m_pShadowData = nullptr;
        shdw->m_pOwner = nullptr;
    }
}

CRealTimeShadow* CRealTimeShadowManager::GetRealTimeShadow(CPhysical* physical) {
    return CHook::CallFunction<CRealTimeShadow*>(g_libGTASA + (VER_x32 ? 0x5B87AC + 1 : 0x6DD0C4), this, physical);
}

void CRealTimeShadowManager::DoShadowThisFrame(CPhysical* physical) {
  //  DLOG("DoShadowThisFrame");
//    switch (g_fx.GetFxQuality()) {
//        case FX_QUALITY_VERY_HIGH: // Always render
//            break;
//        case FX_QUALITY_HIGH: { // Only draw for main player
//            if (physical->IsPed()) {
//                if (physical->AsPed()->m_nPedType == PED_TYPE_PLAYER1) {
//                    break;
//                }
//            }
//            return;
//        }
//        default: // For any other quality: skip
//            return;
//    }

    // Same gate as native mobile DoShadowThisFrame (0x6DCFE8..0x6DD024):
    // only the local player gets a real-time shadow, unless Shadows is set to
    // real-time for everyone (MobileSettings[MS_Shadows] == 2). Without it
    // every NPC took one of the 40 slots, lost its blob shadow, and pulsed in
    // and out of the 12 visible ranks as the camera moved.
    const bool localPlayer = physical->IsPed()
        && static_cast<CPed*>(physical)->m_nPedType == PEDTYPE_PLAYER1;
    if (!localPlayer && CMobileSettings::ms_MobileSettings[MS_Shadows].value != 2) {
        return;
    }
    // Real-time shadows for everyone (MS_Shadows == 2): each one renders the
    // ped/vehicle again into a 256 px raster every frame. Skip the ones whose
    // shadow cannot reach the screen (bounding sphere grown 3x + 2 m for long
    // low-sun shadows). The local player always keeps its shadow.
    if (!localPlayer) {
        CBaseModelInfo* info = CModelInfo::GetModelInfo(physical->m_nModelIndex);
        CColModel* col = info ? info->GetColModel() : nullptr;
        const float radius = col ? col->GetBoundRadius() : 2.0f;
        CVector centre;
        physical->GetBoundCentre(centre);
        if (!CCamera::Get().IsSphereVisible(&centre, radius * 3.0f + 2.0f)) return;
    }
    if (const auto shdw = physical->m_pShadowData) {
        shdw->m_bKeepAlive = true;
    } else {
        (void)GetRealTimeShadow(physical);
    }
}

// Instance manager milik game. Diisi dari hook di bawah saat engine
// memanggil DoShadowThisFrame/ReturnRealTimeShadow untuk ped/vehicle.
// Lihat catatan di RealTimeShadowManager.h::GetInstance().
static CRealTimeShadowManager* s_pRealTimeShadowMan = nullptr;

CRealTimeShadowManager* CRealTimeShadowManager::GetInstance() {
    return s_pRealTimeShadowMan;
}

inline void ReturnRealTimeShadow_hook(CRealTimeShadowManager* thiz, CRealTimeShadow *shdw) {
    s_pRealTimeShadowMan = thiz;
    thiz->ReturnRealTimeShadow(shdw);
}

inline void DoShadowThisFrame_hook(CRealTimeShadowManager* thiz, CPhysical *physical) {
    s_pRealTimeShadowMan = thiz;
    thiz->DoShadowThisFrame(physical);
}

// Native CRealTimeShadowManager::Init (0x6DC9F0) creates 40 shadows with
// CRealTimeShadow::Create(class): class 0 -> 512 px (slots 0-3), 1 -> 256 px
// (4-11), 2 -> 16 px (12-39). Update (0x6DCC78) hands rasters out by camera
// distance rank every frame, so a visible ped shadow could switch between a
// sharp shadow and a 16x16 blob while the camera moved. Class 2 now gets the
// 256 px raster: Update sorts 17..256 px rasters into its medium list from
// index 4, 8 + 28 = 36 entries in a 40-entry array (verified at 0x6DCC78).
static bool (*CRealTimeShadow_Create_orig)(CRealTimeShadow*, int32, bool, int32, bool) = nullptr;
static bool CRealTimeShadow_Create_hook(CRealTimeShadow* shadow, int32 sizeClass, bool blurred, int32 blurPasses, bool moreBlur) {
    ML_HOOK_SCOPE();
    if (sizeClass >= 2) sizeClass = 1;
    return CRealTimeShadow_Create_orig ? CRealTimeShadow_Create_orig(shadow, sizeClass, blurred, blurPasses, moreBlur) : false;
}

void CRealTimeShadowManager::InjectHooks() {
    CHook::InlineHook("_ZN15CRealTimeShadow6CreateEibib", &CRealTimeShadow_Create_hook, &CRealTimeShadow_Create_orig);
    CHook::Redirect("_ZN22CRealTimeShadowManager17DoShadowThisFrameEP9CPhysical", &DoShadowThisFrame_hook);
    CHook::Redirect("_ZN22CRealTimeShadowManager20ReturnRealTimeShadowEP15CRealTimeShadow", &ReturnRealTimeShadow_hook);
}
