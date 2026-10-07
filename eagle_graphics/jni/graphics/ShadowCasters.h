#pragma once
// EAGLE graphics engine - shadow caster collection (game thread).
//
// Sources, in priority order:
//   1. CRenderer::ms_aVisibleEntityPtrs - everything GTA draws this frame,
//      already LOD-resolved, streamed and PreRender'ed (peds animated,
//      vehicle wheels/doors updated). This list is relocated by this client
//      (game/Renderer.cpp), the engine reads the live copy.
//   2. Optional sector scan (CWorld::ms_aSectors / ms_aRepeatSectors) inside
//      the XY bounds of the light boxes only: buildings/objects behind or
//      beside the camera whose shadow falls into view. LOD (big building)
//      entities, other interiors and far dynamic entities are skipped.
// Each entity is tested against every cascade light box with its collision
// bounding sphere; tiny casters are skipped in the far cascades. No
// allocation per frame (vectors are reserved once).

#include "CascadeShadow.h"
#include "GraphicsConfig.h"

#include <cstdint>
#include <vector>

struct CEntity;

namespace gfx {

struct CasterStats {
    int visibleConsidered = 0;
    int sectorConsidered = 0;
    int perCascade[kMaxCascades] = {0, 0, 0, 0};
    int dropped = 0;
};

class ShadowCasterCollector {
public:
    void Reserve(int maxPerCascade);
    void Collect(const CascadeShadow& csm, const ShadowSettings& settings, const Vec3& cameraPos);

    const std::vector<CEntity*>& List(int cascade) const { return m_lists[cascade]; }
    const CasterStats& Stats() const { return m_stats; }

private:
    enum class Source { Visible, Sector };
    void Consider(CEntity* entity, Source source, const CascadeShadow& csm, const ShadowSettings& s, const Vec3& cam);
    bool MarkSeen(const CEntity* entity); // false if already seen this frame
    void ScanSectors(const CascadeShadow& csm, const ShadowSettings& s, const Vec3& cam);

    static constexpr int kSeenSize = 8192; // power of two
    const CEntity* m_seenKeys[kSeenSize] = {};
    uint32_t m_seenStamp[kSeenSize] = {};
    uint32_t m_stamp = 1;
    std::vector<CEntity*> m_lists[kMaxCascades];
    int m_max = 1000;
    float m_minRadius[kMaxCascades] = {0.0f, 0.5f, 1.5f, 3.0f};
    CasterStats m_stats;
};

} // namespace gfx
