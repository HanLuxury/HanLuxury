//
// Created on 26.11.2023.
//

#include "RealTimeShadowManager.h"
#include "util/patch.h"
#include "Entity/Ped/Ped.h"
#include "Mobile/MobileSettings/MobileSettings.h"
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

void CRealTimeShadowManager::InjectHooks() {
    CHook::Redirect("_ZN22CRealTimeShadowManager17DoShadowThisFrameEP9CPhysical", &DoShadowThisFrame_hook);
    CHook::Redirect("_ZN22CRealTimeShadowManager20ReturnRealTimeShadowEP15CRealTimeShadow", &ReturnRealTimeShadow_hook);
}
