// Small vector math for the simulation. Positions are double; the renderer works relative to the camera.
// World axes: +X east (right), +Y up, +Z north (forward at yaw 0). Left-handed, like the D3D view space.
#pragma once
#include <cmath>
#include <cstdint>

namespace taelia {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDegToRad = kPi / 180.0;

struct dvec3 {
    double x = 0, y = 0, z = 0;
    constexpr dvec3() = default;
    constexpr dvec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    constexpr dvec3 operator+(const dvec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr dvec3 operator-(const dvec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr dvec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    constexpr dvec3 operator-() const { return {-x, -y, -z}; }
    dvec3& operator+=(const dvec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
};

inline constexpr double dot(const dvec3& a, const dvec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline constexpr dvec3 cross(const dvec3& a, const dvec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(const dvec3& v) { return std::sqrt(dot(v, v)); }
inline dvec3 normalize(const dvec3& v) {
    double l = length(v);
    return l > 0 ? v * (1.0 / l) : dvec3{0, 0, 1};
}

// Direction for yaw (turning right from +Z toward +X) and pitch (up positive), radians.
inline dvec3 directionFromAngles(double yaw, double pitch) {
    return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}

// Wraps an angle to (-pi, pi].
inline double wrapAngle(double a) {
    a = std::fmod(a + kPi, 2.0 * kPi);
    if (a <= 0) a += 2.0 * kPi;
    return a - kPi;
}

inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline double smoothstep(double e0, double e1, double x) {
    double t = clampd((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

}  // namespace taelia
