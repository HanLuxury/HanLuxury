#include "Renderer.h"
#include "../util/patch.h"
#include "app/app_light.h"
#include "VisibilityPlugins.h"
#include "Camera.h"
#include "World.h"
#include "CWorldScan.h"
#include "Streaming.h"
#include "Pools.h"
#include "RwHelper.h"
#include <algorithm>
#include <type_traits>
 
void CRenderer::ScanWorld() {
    static CVector oldCameraPosn;
    static CVector oldCameraView;

    CCamera& cam = CCamera::Get();
    RwCamera* m_pRwCamera = cam.m_pRwCamera;
    if (!m_pRwCamera) return;

    float farPlane = m_pRwCamera->farPlane;

    RwV3d points[13];
    points[0] = { 0.0f, 0.0f, 0.0f };

    float viewX = m_pRwCamera->viewWindow.x;
    float viewY = m_pRwCamera->viewWindow.y;
    float farX = farPlane * viewX;
    float farY = farPlane * viewY;

    points[1] = { -farX,  farY, farPlane };
    points[2] = {  farX,  farY, farPlane };
    points[3] = {  farX, -farY, farPlane };
    points[4] = { -farX, -farY, farPlane };

    float lodScale = 300.0f / farPlane;
    for (int i = 1; i <= 4; ++i) {
        points[i + 4] = { points[i].x * lodScale, points[i].y * lodScale, points[i].z * lodScale };
    }

    for (int i = 5; i <= 8; ++i) {
        points[i + 4] = { points[i].x * 0.2f, points[i].y * 0.2f, points[i].z * 0.2f };
    }

    // CRenderer::m_pFirstPersonVehicle = nullptr;
    CVisibilityPlugins::InitAlphaEntityList(); // crash here? need to reverse

    if (CWorld::ms_nCurrentScanCode == 0xFFFF) {
        CWorld::ClearScanCodes();
        CWorld::ms_nCurrentScanCode = 1;
    } else {
        CWorld::ms_nCurrentScanCode++;
    }

    CVector* p_pos;
    if (&cam.m_mCameraMatrix) {
        p_pos = &cam.m_mCameraMatrix.GetPosition();
    } else {
        p_pos = &cam.m_placement.m_vPosn;
    }
    oldCameraPosn = *p_pos;

    auto* camFrame = static_cast<RwFrame*>(m_pRwCamera->object.object.parent);
    RwMatrix* camMat = RwFrameGetLTM(camFrame);
    RwV3dTransformPoints(points, points, 13, camMat);

    RwV2d sectorPoints[5];
    for (int i = 0; i < 5; ++i) {
        sectorPoints[i].x = (points[i].x / 50.0f) + 60.0f;
        sectorPoints[i].y = (points[i].y / 50.0f) + 60.0f;
    }

    CRenderer::m_loadingPriority = false;
    CWorldScan::ScanWorld(sectorPoints, 5, CRenderer::ScanSectorList);

    RwV2d lodPoints[5];
    lodPoints[0].x = (points[0].x / 200.0f) + 15.0f;
    lodPoints[0].y = (points[0].y / 200.0f) + 15.0f;
    for (int i = 1; i < 5; ++i) {
        lodPoints[i].x = (points[4 + i].x / 200.0f) + 15.0f;
        lodPoints[i].y = (points[4 + i].y / 200.0f) + 15.0f;
    }

    CWorldScan::ScanWorld(lodPoints, 5, CRenderer::ScanBigBuildingList);
}

void CRenderer::RenderFadingInEntities() {
    RwRenderStateSet(rwRENDERSTATEFOGENABLE,         RWRSTATE(TRUE));
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, RWRSTATE(TRUE));
    RwRenderStateSet(rwRENDERSTATECULLMODE,          RWRSTATE(rwCULLMODECULLBACK));
    DeActivateDirectional();
    SetAmbientColours();
    CVisibilityPlugins::RenderFadingEntities();
}

void CRenderer::RenderFadingInUnderwaterEntities() {
    CHook::CallFunction<void>("_ZN9CRenderer32RenderFadingInUnderwaterEntitiesEv");
}

void CRenderer::RenderRoads() {
    CHook::CallFunction<void>("_ZN9CRenderer11RenderRoadsEv");
}

void CRenderer::RenderEverythingBarRoads() {
    CHook::CallFunction<void>("_ZN9CRenderer24RenderEverythingBarRoadsEv");
}

void CRenderer::ScanSectorList(int32 BlockX, int32 BlockY) {
    CHook::CallFunction<void>("_ZN9CRenderer14ScanSectorListEii", BlockX, BlockY);
}

void CRenderer::ScanBigBuildingList(int32 BlockX, int32 BlockY) {
    CHook::CallFunction<void>("_ZN9CRenderer19ScanBigBuildingListEii", BlockX, BlockY);
}

// CRenderer::AddEntityToRenderList() appends to the visible entity list (or
// the LOD list) with no bounds check at all. A dense mapping (tens of
// thousands of objects, draw distance 300) passes the array size and the
// game then writes entity pointers over whatever follows the array in this
// library: vectors, maps and texture pointers of other systems, which then
// crash far away (CObjectSamp constructor, RwTextureDestroy, collision).
// Entities past the limit are simply not drawn this frame.
static void (*AddEntityToRenderList_orig)(CEntity*, float) = nullptr;
static void AddEntityToRenderList_hook(CEntity* entity, float distance)
{
    if (!entity) return;

    static int32* s_lodCount = nullptr;
    if (!s_lodCount) s_lodCount = *reinterpret_cast<int32**>(g_libGTASA + 0x84A4A0);

    if (CRenderer::ms_nNoOfVisibleEntities >= static_cast<int32>(MAX_VISIBLE_ENTITY_PTRS) - 1 ||
        (s_lodCount && *s_lodCount >= static_cast<int32>(MAX_VISIBLE_LOD_PTRS) - 1)) {
        return;
    }
    AddEntityToRenderList_orig(entity, distance);
}

// FC near big mappings, backtrace RenderEverythingBarRoads+0x4a8 (0x4f5654)
// right after qsort(ms_aVisibleLodPtrs, ms_nNoOfVisibleLods, 8, sortLODs)
// (0x4f5650). sortLODs (0x4f5104) dereferences every entry:
// entity->m_pRwObject (+0x20), GetFirstAtomic() for a clump (no NULL check on
// the result, 0x4f512c -> 0x4f5130), atomic->geometry, instance data. A LOD
// whose clump has no atomic (broken or replaced DFF) or an entry that is no
// longer a live entity crashed inside the sort. The list is cleaned first;
// a dropped LOD is simply not drawn this frame.
namespace {
template <class Pool>
bool LiveInPool(Pool* pool, const void* entity)
{
    if (!pool || !pool->m_pObjects || !pool->m_byteMap || pool->m_nSize <= 0) return false;
    using Slot = std::remove_pointer_t<decltype(pool->m_pObjects)>;
    const auto base = reinterpret_cast<uintptr_t>(pool->m_pObjects);
    const auto address = reinterpret_cast<uintptr_t>(entity);
    if (address < base) return false;
    const uintptr_t offset = address - base;
    if (offset % sizeof(Slot) != 0) return false;
    const uintptr_t index = offset / sizeof(Slot);
    if (index >= static_cast<uintptr_t>(pool->m_nSize)) return false;
    return !pool->IsFreeSlotAtIndex(static_cast<int32>(index));
}

bool IsSafeLodEntry(CEntity* entity)
{
    if (!entity) return false;
    if (!LiveInPool(CPools::ms_pBuildingPool, entity) &&
        !LiveInPool(CPools::ms_pDummyPool, entity) &&
        !LiveInPool(CPools::ms_pObjectPool, entity)) {
        return false;
    }
    RwObject* object = entity->m_pRwObject;
    if (!object) return true; // sortLODs checks this one
    if (object->type == rpATOMIC) return true;
    if (object->type == rpCLUMP) return GetFirstAtomic(reinterpret_cast<RpClump*>(object)) != nullptr;
    return false;
}

int32* VisibleLodCount()
{
    // GOT slot of CRenderer::ms_nNoOfVisibleLods (R_AARCH64_GLOB_DAT 0x84A4A0).
    static int32* count = nullptr;
    if (!count) count = *reinterpret_cast<int32**>(g_libGTASA + 0x84A4A0);
    return count;
}

void (*RenderEverythingBarRoads_orig)() = nullptr;
void RenderEverythingBarRoads_hook()
{
    if (int32* count = VisibleLodCount()) {
        const int32 total = std::clamp(*count, 0, static_cast<int32>(MAX_VISIBLE_LOD_PTRS) - 1);
        int32 kept = 0;
        for (int32 i = 0; i < total; ++i) {
            CEntity* entity = CRenderer::ms_aVisibleLodPtrs[i];
            if (IsSafeLodEntry(entity)) CRenderer::ms_aVisibleLodPtrs[kept++] = entity;
        }
        if (kept != *count) {
            static int reported = 0;
            if (reported < 8) {
                ++reported;
                Log("Renderer: %d of %d LOD entries dropped (invalid entity or clump without atomic)",
                    *count - kept, *count);
            }
        }
        *count = kept;
    }
    if (RenderEverythingBarRoads_orig) RenderEverythingBarRoads_orig();
}
} // namespace

void CRenderer::InjectHooks() {
    CHook::Write(g_libGTASA + (VER_x32 ? 0x6764D0 : 0x84AA10), &ms_bRenderOutsideTunnels);
    CHook::Write(g_libGTASA + (VER_x32 ? 0x67914C : 0x8502C8), &m_loadingPriority);

    CHook::Write(g_libGTASA + (VER_x32 ? 0x6778EC : 0x84D210), &ms_aVisibleEntityPtrs);
    CHook::Write(g_libGTASA + (VER_x32 ? 0x6771F0 : 0x84C428), &ms_nNoOfVisibleEntities);

    // GOT slot of CRenderer::ms_aVisibleLodPtrs in the 2.10 x64 lib. Every
    // reference (AddEntityToRenderList, PreRender, RenderEverythingBarRoads)
    // loads the array through this slot, so relocating it is complete.
    if (!VER_x32) {
        CHook::Write(g_libGTASA + 0x84A958, &ms_aVisibleLodPtrs);
        CHook::InlineHook("_ZN9CRenderer21AddEntityToRenderListEP7CEntityf",
                          &AddEntityToRenderList_hook, &AddEntityToRenderList_orig);
        CHook::InlineHook("_ZN9CRenderer24RenderEverythingBarRoadsEv",
                          &RenderEverythingBarRoads_hook, &RenderEverythingBarRoads_orig);
    }

    // CHook::Redirect("_ZN9CRenderer9ScanWorldEv", &ScanWorld); // WARNING: FPS drop
}