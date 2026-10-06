#include "Matrix4.h"

#include <cmath>
#include <cstring>

namespace gfx {

Mat4 Mat4::Identity() {
    Mat4 r = Zero();
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 Mat4::Zero() {
    Mat4 r;
    std::memset(r.m, 0, sizeof(r.m));
    return r;
}

Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r = Mat4::Zero();
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

Vec4 Transform(const Mat4& m, const Vec4& v) {
    return {m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12] * v.w,
            m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13] * v.w,
            m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] * v.w,
            m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15] * v.w};
}

Vec3 TransformPoint(const Mat4& m, const Vec3& p) {
    const Vec4 r = Transform(m, {p.x, p.y, p.z, 1.0f});
    const float w = (std::fabs(r.w) > 1e-12f) ? r.w : 1.0f;
    return {r.x / w, r.y / w, r.z / w};
}

bool IsFinite(const Mat4& m) {
    for (float v : m.m)
        if (!std::isfinite(v)) return false;
    return true;
}

// Cofactor inverse (MESA gluInvertMatrix layout, column-major safe).
bool Invert(const Mat4& in, Mat4& out) {
    const float* m = in.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-20f) return false;
    const float invDet = 1.0f / det;
    for (int i = 0; i < 16; ++i) out.m[i] = inv[i] * invDet;
    return IsFinite(out);
}

Mat4 RwViewMatrix(const Vec3& r, const Vec3& u, const Vec3& a, const Vec3& p) {
    Mat4 v = Mat4::Zero();
    v.m[0] = -r.x; v.m[1] = u.x; v.m[2] = -a.x;
    v.m[4] = -r.y; v.m[5] = u.y; v.m[6] = -a.y;
    v.m[8] = -r.z; v.m[9] = u.z; v.m[10] = -a.z;
    v.m[12] = Dot(r, p);
    v.m[13] = -Dot(u, p);
    v.m[14] = Dot(a, p);
    v.m[15] = 1.0f;
    return v;
}

Mat4 RwParallelProjection(float vwx, float vwy, float offx, float offy, float n, float f) {
    Mat4 p = Mat4::Zero();
    const float rx = 1.0f / vwx, ry = 1.0f / vwy, depth = f - n;
    p.m[0] = rx;
    p.m[5] = ry;
    p.m[8] = -rx * offx;
    p.m[9] = -ry * offy;
    p.m[10] = -2.0f / depth;
    p.m[14] = -(n + f) / depth;
    p.m[15] = 1.0f;
    return p;
}

Mat4 RwPerspectiveProjection(float vwx, float vwy, float offx, float offy, float n, float f) {
    // Standard glFrustum with the bounds used by the driver.
    const float l = -n * (vwx + offx), r = n * (vwx - offx);
    const float b = -n * (vwy + offy), t = n * (vwy - offy);
    Mat4 p = Mat4::Zero();
    p.m[0] = 2.0f * n / (r - l);
    p.m[5] = 2.0f * n / (t - b);
    p.m[8] = (r + l) / (r - l);
    p.m[9] = (t + b) / (t - b);
    p.m[10] = -(f + n) / (f - n);
    p.m[11] = -1.0f;
    p.m[14] = -2.0f * f * n / (f - n);
    return p;
}

Mat4 AtlasBiasMatrix(float su, float sv, float ou, float ov) {
    Mat4 b = Mat4::Zero();
    b.m[0] = 0.5f * su;
    b.m[5] = 0.5f * sv;
    b.m[10] = 0.5f;
    b.m[12] = 0.5f * su + ou;
    b.m[13] = 0.5f * sv + ov;
    b.m[14] = 0.5f;
    b.m[15] = 1.0f;
    return b;
}

} // namespace gfx
