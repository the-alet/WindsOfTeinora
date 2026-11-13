// Gradient noise on an integer lattice (cell index as int64, fraction in double), so it stays as smooth at
// 10,000 km as at the origin. Inputs are world metres; `wavelength` is the lattice cell size.
#pragma once
#include <cmath>
#include <cstdint>

#include "taelia/sim/hash.h"

namespace taelia {

// Perlin-style gradient noise, about [-1, 1].
inline double gradientNoise2(double x, double z, double wavelength, uint64_t seed) {
    static constexpr double kDirs[16][2] = {
        {1.0, 0.0}, {0.9238795, 0.3826834}, {0.7071068, 0.7071068}, {0.3826834, 0.9238795},
        {0.0, 1.0}, {-0.3826834, 0.9238795}, {-0.7071068, 0.7071068}, {-0.9238795, 0.3826834},
        {-1.0, 0.0}, {-0.9238795, -0.3826834}, {-0.7071068, -0.7071068}, {-0.3826834, -0.9238795},
        {0.0, -1.0}, {0.3826834, -0.9238795}, {0.7071068, -0.7071068}, {0.9238795, -0.3826834}};
    const double gx = x / wavelength, gz = z / wavelength;
    const double fx0 = std::floor(gx), fz0 = std::floor(gz);
    const int64_t ix = static_cast<int64_t>(fx0), iz = static_cast<int64_t>(fz0);
    const double tx = gx - fx0, tz = gz - fz0;
    auto corner = [&](int64_t cx, int64_t cz, double dx, double dz) {
        const double* d = kDirs[hashLattice(cx, 0, cz, seed) >> 60];
        return d[0] * dx + d[1] * dz;
    };
    const double n00 = corner(ix, iz, tx, tz), n10 = corner(ix + 1, iz, tx - 1, tz);
    const double n01 = corner(ix, iz + 1, tx, tz - 1), n11 = corner(ix + 1, iz + 1, tx - 1, tz - 1);
    auto fade = [](double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); };
    const double u = fade(tx), v = fade(tz);
    const double a = n00 + (n10 - n00) * u, b = n01 + (n11 - n01) * u;
    return (a + (b - a) * v) * 1.4142135;
}

// Fractal sum: octaves halve the wavelength and multiply the amplitude by `gain`. Normalised to about [-1, 1].
inline double fbm2(double x, double z, double wavelength, int octaves, double gain, uint64_t seed) {
    double sum = 0, amp = 1, norm = 0;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * gradientNoise2(x, z, wavelength, mix64(seed + uint64_t(i) * 0x9e37ull));
        norm += amp;
        amp *= gain;
        wavelength *= 0.5;
    }
    return sum / norm;
}

// Ridged fractal: sharp crests, rounded valleys. [0, 1].
inline double ridged2(double x, double z, double wavelength, int octaves, double gain, uint64_t seed) {
    double sum = 0, amp = 1, norm = 0, weight = 1;
    for (int i = 0; i < octaves; ++i) {
        double n = 1.0 - std::fabs(gradientNoise2(x, z, wavelength, mix64(seed + uint64_t(i) * 0x7f4bull)));
        n *= n;
        n *= weight;
        weight = n * 1.6 < 1.0 ? (n * 1.6 > 0 ? n * 1.6 : 0.0) : 1.0;
        sum += amp * n;
        norm += amp;
        amp *= gain;
        wavelength *= 0.5;
    }
    return sum / norm;
}

}  // namespace taelia

namespace taelia {

// 3D gradient noise on an integer lattice (12 edge gradients), about [-1, 1]. Same precision rules as above.
inline double gradientNoise3(double x, double y, double z, double wavelength, uint64_t seed) {
    static constexpr double kG[12][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
                                         {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};
    const double gx = x / wavelength, gy = y / wavelength, gz = z / wavelength;
    const double fx = std::floor(gx), fy = std::floor(gy), fz = std::floor(gz);
    const int64_t ix = int64_t(fx), iy = int64_t(fy), iz = int64_t(fz);
    const double tx = gx - fx, ty = gy - fy, tz = gz - fz;
    auto corner = [&](int64_t cx, int64_t cy, int64_t cz, double dx, double dy, double dz) {
        const double* g = kG[hashLattice(cx, cy, cz, seed) % 12];
        return g[0] * dx + g[1] * dy + g[2] * dz;
    };
    auto fade = [](double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); };
    const double u = fade(tx), v = fade(ty), w = fade(tz);
    auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
    const double x00 = lerp(corner(ix, iy, iz, tx, ty, tz), corner(ix + 1, iy, iz, tx - 1, ty, tz), u);
    const double x10 = lerp(corner(ix, iy + 1, iz, tx, ty - 1, tz), corner(ix + 1, iy + 1, iz, tx - 1, ty - 1, tz), u);
    const double x01 = lerp(corner(ix, iy, iz + 1, tx, ty, tz - 1), corner(ix + 1, iy, iz + 1, tx - 1, ty, tz - 1), u);
    const double x11 = lerp(corner(ix, iy + 1, iz + 1, tx, ty - 1, tz - 1), corner(ix + 1, iy + 1, iz + 1, tx - 1, ty - 1, tz - 1), u);
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

inline double fbm3(double x, double y, double z, double wavelength, int octaves, double gain, uint64_t seed) {
    double sum = 0, amp = 1, norm = 0;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * gradientNoise3(x, y, z, wavelength, mix64(seed + uint64_t(i) * 0x3c6ef372ull));
        norm += amp;
        amp *= gain;
        wavelength *= 0.5;
    }
    return sum / norm;
}

}  // namespace taelia
