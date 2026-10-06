#pragma once
// EAGLE graphics engine - Cascaded Shadow Maps (CPU side, no GL).
//
//  * Practical split scheme: split = mix(uniform, logarithmic, lambda).
//  * Each cascade covers the bounding sphere of its camera-frustum slice.
//    The sphere radius depends only on near/far/FOV, so it does not change
//    when the camera rotates; the radius is additionally quantised in 5%
//    steps so GTA's speed-dependent FOV does not resize the cascade every frame.
//  * The light camera is snapped to whole shadow-map texels in the light's
//    right/up plane -> no shimmering when the camera moves.
//  * The light box is extended 'casterExtend' metres towards the sun so tall
//    buildings outside the slice still cast into it.
//  * Matrices follow the RenderWare/GTA conventions (Math/Matrix4.h) because
//    the caster pass is rendered by GTA's own shaders through a RwCamera.
//  * All cascades live in one depth atlas (1x1, 2x1 or 2x2 tiles) so the
//    GTA world shaders (GLSL ES 1.00, no texture arrays) can sample them.

#include "Math/Matrix4.h"
#include "Math/Vector3.h"

namespace gfx {

constexpr int kMaxCascades = 4;

struct CameraFrustum {
    Vec3 pos, right, up, at;   // RW camera LTM
    float tanX = 0.5f;         // viewWindow.x (RW: tan of half horizontal FOV)
    float tanY = 0.5f;         // viewWindow.y
    float nearPlane = 0.1f;
};

struct Cascade {
    float splitNear = 0.0f, splitFar = 0.0f; // view depth range (metres along 'at')
    Vec3 center;                             // bounding sphere of the slice
    float radius = 0.0f;
    // Light camera (RenderWare frame + parallel projection).
    Vec3 lightPos, lightRight, lightUp, lightAt;
    float viewWindow = 0.0f;                 // half extent = radius
    float nearPlane = 0.0f, farPlane = 0.0f;
    Mat4 view, proj, viewProj;               // RW conventions
    Mat4 atlas;                              // world -> (atlas u, atlas v, depth 0..1)
    float texelWorld = 0.0f;                 // metres per shadow-map texel
    float depthBias = 0.0f;                  // normalized depth bias for receivers
    int tileX = 0, tileY = 0;
    int viewport[4] = {0, 0, 0, 0};          // pixels inside the atlas
    float tileRect[4] = {0, 0, 1, 1};        // u0, v0, u1, v1 (PCF clamp, with margin)
};

class CascadeShadow {
public:
    void Configure(int count, int resolution, float distance, float lambda, float casterExtend, float depthBiasMeters);

    void CalculateCascadeSplits(float cameraNear);
    bool UpdateLightMatrices(const CameraFrustum& camera, const Vec3& lightDirection);

    // Sphere vs. light box of cascade i (casters only need to touch the box).
    bool SphereInCascade(int i, const Vec3& center, float radius) const;

    int Count() const { return m_count; }
    int Resolution() const { return m_resolution; }
    float Distance() const { return m_distance; }
    int Columns() const { return m_cols; }
    int Rows() const { return m_rows; }
    int AtlasWidth() const { return m_resolution * m_cols; }
    int AtlasHeight() const { return m_resolution * m_rows; }
    const Cascade& Get(int i) const { return m_cascades[i]; }
    bool Valid() const { return m_valid; }

    // World-space XY bounds of all light boxes (for the caster sector scan).
    void GetCasterBoundsXY(float& minX, float& minY, float& maxX, float& maxY) const;

private:
    int m_count = 3;
    int m_resolution = 2048;
    float m_distance = 160.0f;
    float m_lambda = 0.75f;
    float m_casterExtend = 140.0f;
    float m_depthBiasMeters = 0.06f;
    int m_cols = 2, m_rows = 2;
    float m_splits[kMaxCascades + 1] = {};
    Cascade m_cascades[kMaxCascades];
    bool m_valid = false;
};

} // namespace gfx
