#pragma once
// EAGLE graphics engine - small vector types used by the CPU side of the
// renderer (sun, cascades, caster culling). No dependency on game headers.

#include <cmath>

namespace gfx {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float vx, float vy, float vz) : x(vx), y(vy), z(vz) {}

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    constexpr Vec4() = default;
    constexpr Vec4(float vx, float vy, float vz, float vw) : x(vx), y(vy), z(vz), w(vw) {}
};

constexpr float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

constexpr Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }

inline bool IsFinite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Returns false (and leaves v untouched) for zero/NaN vectors.
inline bool Normalize(Vec3& v) {
    const float len = Length(v);
    if (!std::isfinite(len) || len < 1e-6f) return false;
    v *= 1.0f / len;
    return true;
}

inline Vec3 Normalized(const Vec3& v, const Vec3& fallback) {
    Vec3 r = v;
    return Normalize(r) ? r : fallback;
}

constexpr Vec3 Lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

constexpr float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

constexpr float Saturate(float v) { return Clamp(v, 0.0f, 1.0f); }

constexpr float LerpF(float a, float b, float t) { return a + (b - a) * t; }

inline float SmoothStep(float e0, float e1, float x) {
    if (e1 == e0) return x < e0 ? 0.0f : 1.0f;
    const float t = Saturate((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

inline float SafeFloat(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;

} // namespace gfx
