#include "ShadowCasters.h"
#include "GameRenderBridge.h"

#include "../game/common.h"
#include "../game/Renderer.h"
#include "../game/World.h"
#include "../game/game.h"
#include "../game/Entity/Entity.h"
#include "../game/Models/ModelInfo.h"
#include "../game/Collision/ColModel.h"

#include <algorithm>
#include <cmath>

namespace gfx {
namespace {

constexpr int kAreaEverywhere = 13; // IPL area code visible in every interior

// Index into an open-addressing table.
inline uint32_t HashPtr(const void* p) {
    uint64_t v = reinterpret_cast<uintptr_t>(p);
    v ^= v >> 29;
    v *= 0xBF58476D1CE4E5B9ULL;
    v ^= v >> 32;
    return static_cast<uint32_t>(v);
}

bool TypeEnabled(eEntityType type, const ShadowSettings& s) {
    switch (type) {
        case ENTITY_TYPE_BUILDING: return s.buildings;
        case ENTITY_TYPE_DUMMY: return s.dummies;
        case ENTITY_TYPE_OBJECT: return s.objects;
        case ENTITY_TYPE_VEHICLE: return s.vehicles;
        case ENTITY_TYPE_PED: return s.peds;
        default: return false;
    }
}

} // namespace

void ShadowCasterCollector::Reserve(int maxPerCascade) {
    m_max = std::max(maxPerCascade, 16);
    for (auto& list : m_lists) {
        list.clear();
        if (static_cast<int>(list.capacity()) < m_max) list.reserve(static_cast<size_t>(m_max));
    }
}

bool ShadowCasterCollector::MarkSeen(const CEntity* entity) {
    uint32_t slot = HashPtr(entity) & (kSeenSize - 1);
    for (int probe = 0; probe < 32; ++probe) {
        if (m_seenStamp[slot] != m_stamp) {
            m_seenStamp[slot] = m_stamp;
            m_seenKeys[slot] = entity;
            return true;
        }
        if (m_seenKeys[slot] == entity) return false;
        slot = (slot + 1) & (kSeenSize - 1);
    }
    return true; // table crowded: accept (worst case an entity is drawn twice)
}

void ShadowCasterCollector::Consider(CEntity* e, Source source, const CascadeShadow& csm, const ShadowSettings& s,
                                     const Vec3& cam) {
    if (!e || !e->m_pRwObject || !e->m_bIsVisible) return;
    const eEntityType type = e->GetType();
    if (!TypeEnabled(type, s)) return;
    if (!MarkSeen(e)) return;
    if (GameRenderBridge::IsSkyObject(e)) return;

    const int modelIndex = e->m_nModelIndex;
    if (modelIndex < 0 || modelIndex >= CModelInfo::NUM_MODEL_INFOS || !CModelInfo::GetModelInfo(modelIndex)) return;

    if (source == Source::Sector) {
        // Off-screen entities: GTA has not LOD-resolved them for this frame.
        if (e->m_bIsBIGBuilding) return; // LOD models: the visible list already contains the drawn ones
        if (e->m_nAreaCode != CGame::currArea && e->m_nAreaCode != kAreaEverywhere) return;
        if (type == ENTITY_TYPE_PED && !s.offscreenPeds) return;
        if (type == ENTITY_TYPE_VEHICLE || type == ENTITY_TYPE_OBJECT || type == ENTITY_TYPE_PED) {
            const CVector& p = e->GetPosition();
            const float dx = p.x - cam.x, dy = p.y - cam.y;
            if (dx * dx + dy * dy > s.offscreenDynamicRange * s.offscreenDynamicRange) return;
        }
    }

    CColModel* col = e->GetColModel();
    if (!col) return;
    const float radius = col->GetBoundRadius();
    if (!std::isfinite(radius) || radius <= 0.0f || radius > 1500.0f) return;
    CVector centre;
    e->GetBoundCentre(centre);
    const Vec3 c{centre.x, centre.y, centre.z};
    if (!IsFinite(c)) return;

    for (int i = 0; i < csm.Count(); ++i) {
        if (radius < m_minRadius[i]) continue;
        if (type == ENTITY_TYPE_PED && i >= 2) continue; // peds are sub-texel in the far cascades
        if (!csm.SphereInCascade(i, c, radius)) continue;
        if (static_cast<int>(m_lists[i].size()) >= m_max) {
            ++m_stats.dropped;
            continue;
        }
        m_lists[i].push_back(e);
    }
}

void ShadowCasterCollector::ScanSectors(const CascadeShadow& csm, const ShadowSettings& s, const Vec3& cam) {
    float minX, minY, maxX, maxY;
    csm.GetCasterBoundsXY(minX, minY, maxX, maxY);
    // Never scan more than the shadow range around the camera.
    const float limit = csm.Distance() + s.casterExtend + 50.0f;
    minX = std::max(minX, cam.x - limit);
    minY = std::max(minY, cam.y - limit);
    maxX = std::min(maxX, cam.x + limit);
    maxY = std::min(maxY, cam.y + limit);
    if (!(minX <= maxX && minY <= maxY)) return;

    const int x0 = std::clamp(CWorld::GetSectorX(minX), 0, MAX_SECTORS_X - 1);
    const int y0 = std::clamp(CWorld::GetSectorY(minY), 0, MAX_SECTORS_Y - 1);
    const int x1 = std::clamp(CWorld::GetSectorX(maxX), 0, MAX_SECTORS_X - 1);
    const int y1 = std::clamp(CWorld::GetSectorY(maxY), 0, MAX_SECTORS_Y - 1);

    auto scanList = [&](const CPtrListDoubleLink& list) {
        int guard = 0;
        for (CPtrNodeDoubleLink* node = list.GetNode(); node && guard < 4096; node = node->m_next, ++guard) {
            Consider(node->GetItem<CEntity>(), Source::Sector, csm, s, cam);
            ++m_stats.sectorConsidered;
        }
    };

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            CSector* sector = GetSector(x, y);
            if (!sector) continue;
            if (s.buildings) scanList(sector->m_buildings);
            if (s.dummies) scanList(sector->m_dummies);
        }
    }
    // Repeat sectors wrap every 16 sectors: never visit one twice.
    const int rx1 = std::min(x1, x0 + MAX_REPEAT_SECTORS_X - 1);
    const int ry1 = std::min(y1, y0 + MAX_REPEAT_SECTORS_Y - 1);
    for (int y = y0; y <= ry1; ++y) {
        for (int x = x0; x <= rx1; ++x) {
            CRepeatSector* repeat = GetRepeatSector(x, y);
            if (!repeat) continue;
            if (s.objects) scanList(repeat->GetList(REPEATSECTOR_OBJECTS));
            if (s.vehicles) scanList(repeat->GetList(REPEATSECTOR_VEHICLES));
            if (s.peds && s.offscreenPeds) scanList(repeat->GetList(REPEATSECTOR_PEDS));
        }
    }
}

void ShadowCasterCollector::Collect(const CascadeShadow& csm, const ShadowSettings& s, const Vec3& cam) {
    m_stats = CasterStats{};
    for (auto& list : m_lists) list.clear();
    if (++m_stamp == 0) { // wrapped: reset the table once every 4 billion frames
        std::fill(std::begin(m_seenStamp), std::end(m_seenStamp), 0u);
        m_stamp = 1;
    }

    // Distance-based minimum caster size per cascade (scaled with the shadow range).
    const float scale = csm.Distance() / 160.0f;
    m_minRadius[0] = 0.0f;
    m_minRadius[1] = 0.5f * scale;
    m_minRadius[2] = 1.5f * scale;
    m_minRadius[3] = 3.0f * scale;

    const int visible = std::clamp<int>(CRenderer::ms_nNoOfVisibleEntities, 0, static_cast<int>(MAX_VISIBLE_ENTITY_PTRS));
    for (int i = 0; i < visible; ++i) {
        Consider(CRenderer::ms_aVisibleEntityPtrs[i], Source::Visible, csm, s, cam);
        ++m_stats.visibleConsidered;
    }
    if (s.offscreenCasters) ScanSectors(csm, s, cam);

    for (int i = 0; i < csm.Count(); ++i) m_stats.perCascade[i] = static_cast<int>(m_lists[i].size());
}

} // namespace gfx
