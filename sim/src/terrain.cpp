#include "taelia/sim/terrain.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "taelia/sim/noise.h"
#include "taelia/shared_constants.h"

namespace taelia {

namespace {

constexpr double kSpawnClear = 70.0;   // flat-ish within this radius of the spawn point
constexpr double kSpawnBlend = 180.0;  // back to full relief here
constexpr double kBank = 0.06;         // the shore stands this much above the water

uint64_t sub(uint64_t seed, uint64_t salt) { return mix64(seed ^ (salt * 0x9e3779b97f4a7c15ull)); }

double smin(double a, double b, double k) {
    const double h = std::max(k - std::fabs(a - b), 0.0) / k;
    return std::min(a, b) - h * h * k * 0.25;
}

// Ponds are expensive to find (their level looks at the land around the shore), so each cell is worked out once.
struct PondCache {
    std::shared_mutex mutex;
    std::unordered_map<uint64_t, Pond> cells;  // radius 0 = no pond
};

PondCache& pondCache(uint64_t seed) {
    // Entries live for the program's life, so each thread may keep the last one it found without locking again.
    thread_local uint64_t lastSeed = 0;
    thread_local PondCache* last = nullptr;
    if (last && lastSeed == seed) return *last;
    static std::mutex registryMutex;
    static std::unordered_map<uint64_t, PondCache*> registry;
    std::lock_guard<std::mutex> lock(registryMutex);
    PondCache*& c = registry[seed];
    if (!c) c = new PondCache();  // one per world seed, kept for the program's life
    lastSeed = seed;
    last = c;
    return *c;
}

}  // namespace

double Pond::shoreRadius(double angle) const {
    const double u1 = hashToUnit(mix64(seed + 1)), u2 = hashToUnit(mix64(seed + 2)), u3 = hashToUnit(mix64(seed + 3));
    const double s = 1.0 + 0.22 * std::sin(angle + u1 * 6.2832) + 0.14 * std::sin(2.0 * angle + u2 * 6.2832) +
                     0.08 * std::sin(3.0 * angle + u3 * 6.2832) + 0.04 * std::sin(5.0 * angle + u1 * 9.1);
    return radius * s;
}

double Pond::normalizedDistance(double x, double z) const {
    const double dx = x - cx, dz = z - cz;
    const double d = std::sqrt(dx * dx + dz * dz);
    return d / shoreRadius(std::atan2(dz, dx));
}

Terrain::Terrain(uint64_t seed) : seed_(seed) { campfire_ = {2.5, height(2.5, 7.5), 7.5}; }

double Terrain::baseHeight(double x, double z) const {
    auto raw = [this](double px, double pz) {
        const double wx = px + 260.0 * fbm2(px, pz, 1900.0, 3, 0.5, sub(seed_, 1));
        const double wz = pz + 260.0 * fbm2(px + 5200.0, pz - 1300.0, 1900.0, 3, 0.5, sub(seed_, 2));
        double h = 90.0 * fbm2(wx, wz, 4200.0, 3, 0.5, sub(seed_, 3));                      // lowlands and uplands
        const double hilly = smoothstep(-0.35, 0.35, fbm2(px, pz, 6000.0, 2, 0.5, sub(seed_, 4)));
        h += hilly * 160.0 * (ridged2(wx, wz, 1300.0, 5, 0.5, sub(seed_, 5)) - 0.3);          // hills and ridges
        h += 16.0 * fbm2(wx, wz, 420.0, 3, 0.5, sub(seed_, 6));                              // rolling ground
        h += 1.4 * fbm2(px, pz, 45.0, 4, 0.45, sub(seed_, 7));                               // uneven underfoot
        return h;
    };
    double h = raw(x, z);
    const double r = std::sqrt(x * x + z * z);
    if (r < kSpawnBlend) {
        // A gentle clearing around the spawn point: a quarter of the relief.
        const double h0 = raw(0.0, 0.0);
        const double w = 1.0 - smoothstep(kSpawnClear, kSpawnBlend, r);
        h += w * (h0 + 0.25 * (h - h0) - h);
    }
    return h;
}

bool Terrain::pondInCell(int64_t cellX, int64_t cellZ, Pond& out) const {
    PondCache& cache = pondCache(seed_);
    const uint64_t key = hashLattice(cellX, 0, cellZ, 0x51);
    {
        std::shared_lock<std::shared_mutex> lock(cache.mutex);
        auto it = cache.cells.find(key);
        if (it != cache.cells.end()) {
            out = it->second;
            return out.radius > 0;
        }
    }
    Pond p;
    const uint64_t h = hashLattice(cellX, 7, cellZ, sub(seed_, 11));
    bool exists = true;
    if (cellX == 0 && cellZ == 0) {
        // The spawn pond: a short walk from the campfire.
        p.cx = 38.0;
        p.cz = 30.0;
        p.radius = 21.0;
        p.depth = 2.4;
    } else {
        exists = hashToUnit(h) < 0.42;
        p.cx = (double(cellX) + 0.2 + 0.6 * hashToUnit(mix64(h + 1))) * kPondCell;
        p.cz = (double(cellZ) + 0.2 + 0.6 * hashToUnit(mix64(h + 2))) * kPondCell;
        p.radius = 12.0 + 33.0 * std::pow(hashToUnit(mix64(h + 3)), 1.5);
        p.depth = 1.4 + 2.6 * hashToUnit(mix64(h + 4));
    }
    p.seed = mix64(h + 5);
    if (exists) {
        // The level sits just under the lowest land around the shore, so the basin holds the water.
        double lowest = 1e30;
        for (int i = 0; i < 24; ++i) {
            const double a = i * (2.0 * kPi / 24.0);
            for (double rn : {1.0, 1.15, Pond::kFootprint}) {
                const double r = p.shoreRadius(a) * rn;
                lowest = std::min(lowest, baseHeight(p.cx + std::cos(a) * r, p.cz + std::sin(a) * r));
            }
        }
        p.level = lowest - 0.12;
        // On steep ground the bowl would be a pit in a slope, not a pond.
        if (baseHeight(p.cx, p.cz) - p.level > 6.0) exists = false;
    }
    if (!exists) p.radius = 0;
    {
        std::unique_lock<std::shared_mutex> lock(cache.mutex);
        cache.cells.emplace(key, p);
    }
    out = p;
    return exists;
}

double Terrain::height(double x, double z) const { return untouchedHeight(x, z) + editAt(x, z); }

double Terrain::untouchedHeight(double x, double z) const {
    double h = baseHeight(x, z);
    const int64_t cx = int64_t(std::floor(x / kPondCell)), cz = int64_t(std::floor(z / kPondCell));
    for (int64_t dz = -1; dz <= 1; ++dz) {
        for (int64_t dx = -1; dx <= 1; ++dx) {
            Pond p;
            if (!pondInCell(cx + dx, cz + dz, p)) continue;
            const double rn = p.normalizedDistance(x, z);
            if (rn > 1.6) continue;
            // A bowl that meets the level at the shore and climbs steeply outside it.
            const double out = std::max(rn - 1.0, 0.0);
            const double bowl = p.level - p.depth * (1.0 - std::min(rn, 1.0) * std::min(rn, 1.0)) + 60.0 * out * out;
            h = smin(h, bowl, 0.8);
            // A low bank keeps the land around the shore above the water.
            const double w = smoothstep(0.92, 1.0, rn) * (1.0 - smoothstep(Pond::kFootprint, 1.45, rn));
            if (w > 0) h = std::max(h, p.level + kBank - 1000.0 * (1.0 - w));
        }
    }
    return h;
}

double Terrain::waterLevelAt(double x, double z, double groundHeight) const {
    double level = -1e30;
    const int64_t cx = int64_t(std::floor(x / kPondCell)), cz = int64_t(std::floor(z / kPondCell));
    for (int64_t dz = -1; dz <= 1; ++dz)
        for (int64_t dx = -1; dx <= 1; ++dx) {
            Pond p;
            if (pondInCell(cx + dx, cz + dz, p) && groundHeight < p.level && p.normalizedDistance(x, z) < Pond::kFootprint)
                level = std::max(level, p.level);
        }
    return level;
}

void Terrain::pondsNear(double x, double z, double radius, std::vector<Pond>& out) const {
    const double reach = radius + 120.0;
    const int64_t x0 = int64_t(std::floor((x - reach) / kPondCell)), x1 = int64_t(std::floor((x + reach) / kPondCell));
    const int64_t z0 = int64_t(std::floor((z - reach) / kPondCell)), z1 = int64_t(std::floor((z + reach) / kPondCell));
    for (int64_t cz = z0; cz <= z1; ++cz)
        for (int64_t cx = x0; cx <= x1; ++cx) {
            Pond p;
            if (!pondInCell(cx, cz, p)) continue;
            const double d = std::hypot(p.cx - x, p.cz - z);
            if (d < radius + p.radius * 1.6) out.push_back(p);
        }
}

GroundMaterials Terrain::materials(double x, double z, double h, const dvec3& n) const {
    GroundMaterials m;
    const double slope = 1.0 - n.y;
    const double rock = smoothstep(0.17, 0.30, slope);  // bare rock from about 34 degrees
    double sand = 0, mud = 0;
    const int64_t cx = int64_t(std::floor(x / kPondCell)), cz = int64_t(std::floor(z / kPondCell));
    for (int64_t dz = -1; dz <= 1; ++dz)
        for (int64_t dx = -1; dx <= 1; ++dx) {
            Pond p;
            if (!pondInCell(cx + dx, cz + dz, p)) continue;
            const double rn = p.normalizedDistance(x, z);
            if (rn > 1.4) continue;
            // A narrow sandy band along the shore; the bed further in is mud.
            const double nearLevel = 1.0 - smoothstep(0.15, 0.45, std::fabs(h - p.level));
            const double patchy = smoothstep(-0.1, 0.4, fbm2(x, z, 9.0, 2, 0.5, sub(seed_, 22)));
            sand = std::max(sand, patchy * nearLevel * smoothstep(0.8, 0.92, rn) * (1.0 - smoothstep(1.02, 1.1, rn)));
            mud = std::max(mud, 1.0 - smoothstep(0.7, 0.88, rn));
        }
    double dirt = 0.6 * smoothstep(0.58, 0.85, fbm2(x, z, 55.0, 4, 0.55, sub(seed_, 20)) * 0.5 + 0.5);
    dirt = std::max(dirt, mud);
    const dvec3 fire = campfirePosition();
    dirt = std::max(dirt, 1.0 - smoothstep(2.0, 4.5, std::hypot(x - fire.x, z - fire.z)));  // trodden ground
    m.rock = float(rock);
    m.sand = float((1.0 - rock) * sand);
    m.dirt = float((1.0 - rock - m.sand) * dirt);
    m.grass = std::max(0.0f, 1.0f - m.rock - m.sand - m.dirt);
    const double meadow = smoothstep(-0.25, 0.45, fbm2(x, z, 140.0, 3, 0.5, sub(seed_, 21)));
    m.meadow = float(meadow * m.grass);
    return m;
}

TerrainSample Terrain::sample(double x, double z) const {
    TerrainSample s;
    s.height = height(x, z);
    const double e = 0.25;
    const double hx = height(x + e, z) - height(x - e, z), hz = height(x, z + e) - height(x, z - e);
    s.normal = normalize(dvec3{-hx / (2 * e), 1.0, -hz / (2 * e)});
    s.materials = materials(x, z, s.height, s.normal);
    s.waterLevel = waterLevelAt(x, z, s.height);
    return s;
}

}  // namespace taelia

namespace taelia {

namespace {

uint64_t editKey(int64_t i, int64_t j) { return (uint64_t(uint32_t(int32_t(i))) << 32) | uint64_t(uint32_t(int32_t(j))); }

Stratum stratum(double hardness, double density, const float* color) {
    Stratum s;
    s.hardness = hardness;
    s.density = density;
    for (int i = 0; i < 3; ++i) s.color[i] = color[i];
    return s;
}

}  // namespace

Stratum Terrain::stratumAt(double x, double z, double depth) const {
    const double h = untouchedHeight(x, z), e = 0.25;
    const dvec3 n = normalize(dvec3{-(untouchedHeight(x + e, z) - untouchedHeight(x - e, z)) / (2 * e), 1.0,
                                    -(untouchedHeight(x, z + e) - untouchedHeight(x, z - e)) / (2 * e)});
    const GroundMaterials m = materials(x, z, h, n);
    const bool wet = waterLevelAt(x, z, h) > h;
    if (depth >= k::TERRAIN_INTERIOR) return stratum(k::HARDNESS_BEDROCK, 2700, k::STRATUM_ROCK);
    if (m.rock > 0.5f) return stratum(k::HARDNESS_ROCK, 2600, k::STRATUM_ROCK);
    if (wet && depth < 1.0) return stratum(k::HARDNESS_MUD, 1700, k::STRATUM_MUD);
    if (m.sand > 0.5f && depth < k::TERRAIN_SUBSOIL) return stratum(k::HARDNESS_SAND, 1600, k::STRATUM_SAND);
    if (depth < k::TERRAIN_TOPSOIL) return stratum(k::HARDNESS_TOPSOIL, 1300, k::STRATUM_TOPSOIL);
    if (depth < k::TERRAIN_SUBSOIL) return stratum(k::HARDNESS_SUBSOIL, 1800, k::STRATUM_SUBSOIL);
    if (depth < k::TERRAIN_GRAVEL) return stratum(k::HARDNESS_GRAVEL, 1900, k::STRATUM_GRAVEL);
    return stratum(k::HARDNESS_ROCK, 2500, k::STRATUM_ROCK);
}

void Terrain::dent(double x, double z, double radius, double depth) {
    if (!(radius > 1e-4) || !(depth > 1e-6)) return;
    const double cell = k::TERRAIN_EDIT_CELL, outer = radius * 1.6;
    const int64_t i0 = int64_t(std::floor((x - outer) / cell)), i1 = int64_t(std::floor((x + outer) / cell));
    const int64_t j0 = int64_t(std::floor((z - outer) / cell)), j1 = int64_t(std::floor((z + outer) / cell));
    std::unique_lock<std::shared_mutex> lock(editMutex_);
    for (int64_t j = j0; j <= j1; ++j)
        for (int64_t i = i0; i <= i1; ++i) {
            const double r = std::hypot((double(i) + 0.5) * cell - x, (double(j) + 0.5) * cell - z) / radius;
            double dh;
            if (r < 1.0) dh = -depth * (1.0 - r * r);                                    // the bowl
            else if (r < 1.6) dh = 0.15 * depth * std::sin(kPi * (r - 1.0) / 0.6);  // the soil heaped round it
            else continue;
            float& e = edits_[editKey(i, j)];
            e = float(std::max(double(e) + dh, -double(k::TERRAIN_INTERIOR)));
        }
    editLo_[0] = std::min(editLo_[0], x - outer - cell);
    editLo_[1] = std::min(editLo_[1], z - outer - cell);
    editHi_[0] = std::max(editHi_[0], x + outer + cell);
    editHi_[1] = std::max(editHi_[1], z + outer + cell);
    hasEdits_.store(true, std::memory_order_release);
    editsVersion_.fetch_add(1, std::memory_order_acq_rel);
}

double Terrain::editAt(double x, double z) const {
    if (!hasEdits_.load(std::memory_order_acquire)) return 0.0;
    std::shared_lock<std::shared_mutex> lock(editMutex_);
    if (x < editLo_[0] || x > editHi_[0] || z < editLo_[1] || z > editHi_[1]) return 0.0;
    const double cell = k::TERRAIN_EDIT_CELL;
    const double u = x / cell - 0.5, v = z / cell - 0.5;
    const int64_t i = int64_t(std::floor(u)), j = int64_t(std::floor(v));
    const double fu = u - double(i), fv = v - double(j);
    auto at = [&](int64_t a, int64_t b) {
        const auto it = edits_.find(editKey(a, b));
        return it == edits_.end() ? 0.0 : double(it->second);
    };
    return (at(i, j) * (1 - fu) + at(i + 1, j) * fu) * (1 - fv) + (at(i, j + 1) * (1 - fu) + at(i + 1, j + 1) * fu) * fv;
}

void Terrain::editsInArea(double x0, double z0, int n, std::vector<float>& out) const {
    out.assign(size_t(n) * size_t(n), 0.0f);
    if (!hasEdits_.load(std::memory_order_acquire)) return;
    const double cell = k::TERRAIN_EDIT_CELL;
    const int64_t i0 = int64_t(std::floor(x0 / cell)), j0 = int64_t(std::floor(z0 / cell));
    std::shared_lock<std::shared_mutex> lock(editMutex_);
    for (const auto& [key, v] : edits_) {
        const int64_t i = int64_t(int32_t(uint32_t(key >> 32))), j = int64_t(int32_t(uint32_t(key & 0xffffffffull)));
        if (i < i0 || j < j0 || i >= i0 + n || j >= j0 + n) continue;
        out[size_t(j - j0) * size_t(n) + size_t(i - i0)] = v;
    }
}

}  // namespace taelia
