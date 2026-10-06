#include "CascadeShadow.h"

#include <algorithm>
#include <cmath>

namespace gfx {
namespace {

// Rounds r up to the next 5% step (log scale). Keeps the cascade size, and so
// the texel grid, constant while GTA animates the FOV slightly.
float QuantizeRadius(float r) {
    const float step = std::log(1.05f);
    return std::exp(std::ceil(std::log(std::max(r, 0.5f)) / step) * step);
}

} // namespace

void CascadeShadow::Configure(int count, int resolution, float distance, float lambda, float casterExtend,
                              float depthBiasMeters) {
    m_count = std::clamp(count, 1, kMaxCascades);
    m_resolution = std::max(resolution, 256);
    m_distance = std::max(distance, 10.0f);
    m_lambda = Saturate(lambda);
    m_casterExtend = std::max(casterExtend, 0.0f);
    m_depthBiasMeters = std::max(depthBiasMeters, 0.0f);
    m_cols = m_count == 1 ? 1 : 2;
    m_rows = m_count <= 2 ? 1 : 2;
    m_valid = false;
}

void CascadeShadow::CalculateCascadeSplits(float cameraNear) {
    // Practical split scheme (Zhang et al.): log split near the camera, uniform far away.
    const float n = std::max(cameraNear, 0.05f);
    const float splitBase = std::max(n, 1.0f); // avoid a degenerate first log split
    const float f = std::max(m_distance, splitBase + 1.0f);
    m_splits[0] = n;
    for (int i = 1; i <= m_count; ++i) {
        const float p = static_cast<float>(i) / static_cast<float>(m_count);
        const float logSplit = splitBase * std::pow(f / splitBase, p);
        const float uniSplit = splitBase + (f - splitBase) * p;
        m_splits[i] = LerpF(uniSplit, logSplit, m_lambda);
    }
    m_splits[m_count] = f;
}

bool CascadeShadow::UpdateLightMatrices(const CameraFrustum& cam, const Vec3& lightDirection) {
    m_valid = false;
    Vec3 at = lightDirection;
    if (!Normalize(at) || !IsFinite(cam.pos) || !IsFinite(cam.at)) return false;

    // Light basis: depends only on the light direction (stable while the camera moves).
    const Vec3 refUp = std::fabs(at.z) < 0.99f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    Vec3 up = refUp - at * Dot(refUp, at);
    if (!Normalize(up)) return false;
    const Vec3 right = Cross(up, at); // RenderWare frames: right = up x at

    Vec3 camAt = cam.at;
    if (!Normalize(camAt)) return false;
    const float k2 = cam.tanX * cam.tanX + cam.tanY * cam.tanY; // squared slope of the frustum diagonal

    const float atlasW = static_cast<float>(AtlasWidth());
    const float atlasH = static_cast<float>(AtlasHeight());

    for (int i = 0; i < m_count; ++i) {
        Cascade& c = m_cascades[i];
        const float n = m_splits[i];
        const float f = m_splits[i + 1];
        c.splitNear = n;
        c.splitFar = f;

        // Minimal sphere around the frustum slice [n, f] (centre on the view axis).
        float centerDist = 0.5f * (n + f) * (1.0f + k2);
        float radius;
        if (centerDist >= f) {
            centerDist = f;
            radius = f * std::sqrt(k2);
        } else {
            radius = std::sqrt((f - centerDist) * (f - centerDist) + f * f * k2);
        }
        radius = QuantizeRadius(radius);
        c.center = cam.pos + camAt * centerDist;
        c.radius = radius;

        // Texel snapping in the light's right/up plane.
        const float texel = 2.0f * radius / static_cast<float>(m_resolution);
        const float back = std::max(radius, m_casterExtend) + radius * 0.25f;
        const float pr = std::floor(Dot(right, c.center) / texel) * texel;
        const float pu = std::floor(Dot(up, c.center) / texel) * texel;
        const float pa = Dot(at, c.center) - back;
        c.lightPos = right * pr + up * pu + at * pa;
        c.lightRight = right;
        c.lightUp = up;
        c.lightAt = at;
        c.viewWindow = radius;
        c.nearPlane = 0.5f;
        c.farPlane = back + radius + 1.0f;
        c.texelWorld = texel;

        c.view = RwViewMatrix(right, up, at, c.lightPos);
        c.proj = RwParallelProjection(radius, radius, 0.0f, 0.0f, c.nearPlane, c.farPlane);
        c.viewProj = Multiply(c.proj, c.view);

        c.tileX = i % m_cols;
        c.tileY = i / m_cols;
        const float su = 1.0f / static_cast<float>(m_cols);
        const float sv = 1.0f / static_cast<float>(m_rows);
        const float ou = static_cast<float>(c.tileX) * su;
        const float ov = static_cast<float>(c.tileY) * sv;
        c.atlas = Multiply(AtlasBiasMatrix(su, sv, ou, ov), c.viewProj);
        c.viewport[0] = c.tileX * m_resolution;
        c.viewport[1] = c.tileY * m_resolution;
        c.viewport[2] = m_resolution;
        c.viewport[3] = m_resolution;
        // Two texels of margin: 3x3 PCF with bilinear compare never reads a neighbour tile.
        c.tileRect[0] = ou + 2.0f / atlasW;
        c.tileRect[1] = ov + 2.0f / atlasH;
        c.tileRect[2] = ou + su - 2.0f / atlasW;
        c.tileRect[3] = ov + sv - 2.0f / atlasH;

        // Receiver bias grows with the texel footprint of the cascade.
        c.depthBias = (m_depthBiasMeters + 0.5f * texel) / (c.farPlane - c.nearPlane);

        if (!IsFinite(c.viewProj) || !IsFinite(c.atlas)) return false;
    }
    m_valid = true;
    return true;
}

bool CascadeShadow::SphereInCascade(int i, const Vec3& center, float radius) const {
    if (!m_valid || i < 0 || i >= m_count) return false;
    const Cascade& c = m_cascades[i];
    const Vec3 d = center - c.lightPos;
    const float x = Dot(c.lightRight, d);
    const float y = Dot(c.lightUp, d);
    const float z = Dot(c.lightAt, d);
    const float ext = c.viewWindow + radius;
    return std::fabs(x) <= ext && std::fabs(y) <= ext && z + radius >= c.nearPlane && z - radius <= c.farPlane;
}

void CascadeShadow::GetCasterBoundsXY(float& minX, float& minY, float& maxX, float& maxY) const {
    minX = minY = 1e30f;
    maxX = maxY = -1e30f;
    for (int i = 0; i < m_count; ++i) {
        const Cascade& c = m_cascades[i];
        for (int corner = 0; corner < 8; ++corner) {
            const float sx = (corner & 1) ? c.viewWindow : -c.viewWindow;
            const float sy = (corner & 2) ? c.viewWindow : -c.viewWindow;
            const float sz = (corner & 4) ? c.farPlane : c.nearPlane;
            const Vec3 p = c.lightPos + c.lightRight * sx + c.lightUp * sy + c.lightAt * sz;
            minX = std::min(minX, p.x);
            minY = std::min(minY, p.y);
            maxX = std::max(maxX, p.x);
            maxY = std::max(maxY, p.y);
        }
    }
}

} // namespace gfx
