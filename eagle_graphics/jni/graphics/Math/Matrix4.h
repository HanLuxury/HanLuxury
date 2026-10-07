#pragma once
// EAGLE graphics engine - 4x4 matrix, column-major (OpenGL layout: m[col*4+row]).
//
// The RenderWare helpers below reproduce, term by term, what the GTA SA 2.10
// arm64 driver does in _rwOpenGLCameraBeginUpdate (libGTASA.so 0x23ED98):
//   view  : x = -right.(p-pos), y = up.(p-pos), z = -at.(p-pos)
//   ortho : [0]=1/vw.x [5]=1/vw.y [8]=-off.x/vw.x [9]=-off.y/vw.y
//           [10]=-2/(f-n) [14]=-(f+n)/(f-n) [15]=1 (static projMat at 0x8520D0)
//   persp : glFrustum(-n(vw.x+off.x), n(vw.x-off.x), -n(vw.y+off.y), n(vw.y-off.y), n, f)
// The receiver shaders compare against depth produced by GTA's own vertex
// shaders (ProjMatrix * ViewMatrix * ObjMatrix), so these must match exactly.

#include "Vector3.h"

namespace gfx {

struct Mat4 {
    float m[16];

    static Mat4 Identity();
    static Mat4 Zero();

    float& operator()(int row, int col) { return m[col * 4 + row]; }
    float operator()(int row, int col) const { return m[col * 4 + row]; }

    const float* Data() const { return m; }
};

Mat4 Multiply(const Mat4& a, const Mat4& b);          // a * b
Vec3 TransformPoint(const Mat4& m, const Vec3& p);    // (m * vec4(p,1)).xyz / w
Vec4 Transform(const Mat4& m, const Vec4& v);
bool Invert(const Mat4& in, Mat4& out);
bool IsFinite(const Mat4& m);

// RenderWare camera conventions (see header comment).
Mat4 RwViewMatrix(const Vec3& right, const Vec3& up, const Vec3& at, const Vec3& pos);
Mat4 RwParallelProjection(float viewWindowX, float viewWindowY, float offsetX, float offsetY,
                          float nearPlane, float farPlane);
Mat4 RwPerspectiveProjection(float viewWindowX, float viewWindowY, float offsetX, float offsetY,
                             float nearPlane, float farPlane);

// Maps NDC xyz [-1,1] to [0,1] and then into an atlas tile:
// uv = (ndc*0.5+0.5) * scale + offset, depth = ndc.z*0.5+0.5.
Mat4 AtlasBiasMatrix(float scaleU, float scaleV, float offsetU, float offsetV);

} // namespace gfx
