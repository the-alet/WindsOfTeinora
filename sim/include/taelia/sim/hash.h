// Hashes and lattice noise built on integer mixing, so they keep their precision at large coordinates
// (sin()-based hashes run out of precision and tear into straight edges far away).
#pragma once
#include <cmath>
#include <cstdint>

namespace taelia {

// splitmix64 finalizer: a strong 64-bit mix.
inline constexpr uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

inline constexpr uint64_t hashCombine(uint64_t h, uint64_t v) { return mix64(h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2))); }

inline constexpr uint64_t hashLattice(int64_t x, int64_t y, int64_t z, uint64_t seed) {
    uint64_t h = mix64(seed ^ 0x51ed270b2aa3c7f5ull);
    h = hashCombine(h, static_cast<uint64_t>(x));
    h = hashCombine(h, static_cast<uint64_t>(y));
    h = hashCombine(h, static_cast<uint64_t>(z));
    return h;
}

// Uniform in [0, 1) from the top 53 bits.
inline constexpr double hashToUnit(uint64_t h) { return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0); }

// Value noise on an integer lattice: the cell index is an integer and the fraction is taken inside the cell,
// so the result is as smooth at 10,000 km as at the origin. Returns [-1, 1].
inline double latticeNoise2(double x, double z, double cellSize, uint64_t seed) {
    const double gx = x / cellSize, gz = z / cellSize;
    const double fx0 = std::floor(gx), fz0 = std::floor(gz);
    const int64_t ix = static_cast<int64_t>(fx0), iz = static_cast<int64_t>(fz0);
    const double tx = gx - fx0, tz = gz - fz0;
    auto fade = [](double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); };
    const double u = fade(tx), v = fade(tz);
    auto at = [&](int64_t a, int64_t b) { return hashToUnit(hashLattice(a, 0, b, seed)) * 2.0 - 1.0; };
    const double a = at(ix, iz), b = at(ix + 1, iz), c = at(ix, iz + 1), d = at(ix + 1, iz + 1);
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}

}  // namespace taelia
