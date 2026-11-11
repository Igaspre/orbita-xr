#pragma once
// Small vector/quaternion/matrix helpers.
// Right handed, +X right, +Y up, -Z forward. Matrices are row-vector (v' = v * M).
#include <algorithm>
#include <cmath>

namespace ox {

constexpr float kPi = 3.14159265358979323846f;
inline float deg2rad(float d) { return d * kPi / 180.0f; }
inline float rad2deg(float r) { return r * 180.0f / kPi; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline float wrapPi(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a <= -kPi) a += 2.0f * kPi;
    return a;
}

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
    float l = length(v);
    return l > 1e-9f ? v * (1.0f / l) : Vec3(0, 0, 0);
}

struct Quat {
    float w = 1, x = 0, y = 0, z = 0;
    Quat() = default;
    Quat(float W, float X, float Y, float Z) : w(W), x(X), y(Y), z(Z) {}

    static Quat identity() { return {}; }
    static Quat fromAxisAngle(const Vec3& axis, float angle) {
        Vec3 a = normalize(axis);
        float s = std::sin(angle * 0.5f);
        return {std::cos(angle * 0.5f), a.x * s, a.y * s, a.z * s};
    }
    static Quat fromRotationVector(const Vec3& r) {
        float ang = length(r);
        if (ang < 1e-9f) return {1.0f, r.x * 0.5f, r.y * 0.5f, r.z * 0.5f};
        return fromAxisAngle(r * (1.0f / ang), ang);
    }
    // positive yaw turns left, positive pitch looks up
    static Quat fromYawPitch(float yaw, float pitch) {
        return fromAxisAngle({0, 1, 0}, yaw) * fromAxisAngle({1, 0, 0}, pitch);
    }

    Quat operator*(const Quat& b) const {
        return {w * b.w - x * b.x - y * b.y - z * b.z, w * b.x + x * b.w + y * b.z - z * b.y,
                w * b.y - x * b.z + y * b.w + z * b.x, w * b.z + x * b.y - y * b.x + z * b.w};
    }
    Quat conj() const { return {w, -x, -y, -z}; }
    float norm() const { return std::sqrt(w * w + x * x + y * y + z * z); }
    Quat normalized() const {
        float n = norm();
        if (n < 1e-12f) return {};
        float k = 1.0f / n;
        return {w * k, x * k, y * k, z * k};
    }
    Vec3 rotate(const Vec3& v) const {
        Vec3 u(x, y, z);
        Vec3 t = cross(u, v) * 2.0f;
        return v + t * w + cross(u, t);
    }
    Vec3 forward() const { return rotate({0, 0, -1}); }
};

inline float dot(const Quat& a, const Quat& b) { return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z; }

inline Quat slerp(Quat a, Quat b, float t) {
    float d = dot(a, b);
    if (d < 0) { b = {-b.w, -b.x, -b.y, -b.z}; d = -d; }
    if (d > 0.9995f)
        return Quat{a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}
            .normalized();
    float th = std::acos(clampf(d, -1.0f, 1.0f));
    float s = std::sin(th);
    float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return Quat{a.w * wa + b.w * wb, a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb}.normalized();
}

inline float yawOf(const Quat& q) {
    Vec3 f = q.forward();
    return std::atan2(-f.x, -f.z);
}
inline float pitchOf(const Quat& q) { return std::asin(clampf(q.forward().y, -1.0f, 1.0f)); }
inline Quat yawOnly(const Quat& q) { return Quat::fromAxisAngle({0, 1, 0}, yawOf(q)); }

inline Vec3 dirFromYawPitch(float yaw, float pitch) {
    float cp = std::cos(pitch);
    return {-std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp};
}

struct Mat4 {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += m[i][k] * b.m[k][j];
                r.m[i][j] = s;
            }
        return r;
    }
    // same rotation as q.rotate(), laid out for row vectors
    static Mat4 fromQuat(const Quat& qi) {
        Quat q = qi.normalized();
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        float R[3][3] = {{1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
                         {2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
                         {2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)}};
        Mat4 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r.m[i][j] = R[j][i];
        return r;
    }
    // D3D style depth (0..1)
    static Mat4 perspectiveRH(float fovY, float aspect, float zn, float zf) {
        float ys = 1.0f / std::tan(fovY * 0.5f);
        Mat4 r;
        r.m[0][0] = ys / aspect;
        r.m[1][1] = ys;
        r.m[2][2] = zf / (zn - zf);
        r.m[2][3] = -1;
        r.m[3][2] = zn * zf / (zn - zf);
        r.m[3][3] = 0;
        return r;
    }
};

inline Vec3 transformPoint(const Vec3& v, const Mat4& M, float* wOut = nullptr) {
    float x = v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0] + M.m[3][0];
    float y = v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1] + M.m[3][1];
    float z = v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2] + M.m[3][2];
    float w = v.x * M.m[0][3] + v.y * M.m[1][3] + v.z * M.m[2][3] + M.m[3][3];
    if (wOut) *wOut = w;
    return {x, y, z};
}

// glasses specs usually give the diagonal FOV
inline float verticalFovFromDiagonal(float diagFovRad, float aspect) {
    float t = std::tan(diagFovRad * 0.5f);
    return 2.0f * std::atan(t / std::sqrt(aspect * aspect + 1.0f));
}

} // namespace ox
