// The land: heights, pond basins with shaped beds, ground materials. Deterministic from the world seed, double
// precision, engine-free: the renderer, physics and the Unreal port all read it from here.
#pragma once
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "taelia/sim/math.h"

namespace taelia {

// A pond: liquid in a basin. Its own level (not a plane at height 0), a bowl-shaped bed, an irregular shore.
struct Pond {
    double cx = 0, cz = 0;   // centre
    double radius = 0;       // mean shore radius
    double depth = 0;        // at the centre, below the level
    double level = 0;        // water surface height
    uint64_t seed = 0;       // shapes the shore
    // Shore radius toward a direction (the shore is irregular).
    double shoreRadius(double angle) const;
    // Distance from the centre in units of the shore radius toward that point (1 = on the shore).
    double normalizedDistance(double x, double z) const;
    static constexpr double kFootprint = 1.3;  // water may show out to this normalized distance
};

struct GroundMaterials {
    float grass = 1, dirt = 0, rock = 0, sand = 0;  // weights, sum 1
    float meadow = 0;                               // how tall and thick the grass blades grow, 0..1
};

// What the ground is made of somewhere under its surface (the user: its insides, about 10 m of them): how hard it is
// (the pressure it yields under, Pa), how dense (kg/m3), its colour (linear rgb).
struct Stratum {
    double hardness = 2e5, density = 1300;
    float color[3] = {0.10f, 0.07f, 0.045f};
};

struct TerrainSample {
    double height = 0;
    dvec3 normal{0, 1, 0};
    GroundMaterials materials;
    double waterLevel = -1e30;  // surface of the pond covering this point, if the ground here is below it
    bool underwater() const { return waterLevel > height; }
};

class Terrain {
public:
    explicit Terrain(uint64_t seed);

    double height(double x, double z) const;                         // dents and craters included
    TerrainSample sample(double x, double z) const;                  // height, normal, materials, water
    double untouchedHeight(double x, double z) const;                // as the world grew it

    // The ground's insides (the user): what lies `depth` m under the untouched surface: topsoil, clay, sand and
    // gravel, weathered rock, the bedrock past TERRAIN_INTERIOR; sand or mud where the surface is, rock all the way
    // under rock.
    Stratum stratumAt(double x, double z, double depth) const;
    // A dent: a bowl `radius` m across, `depth` m deep round (x, z), part of its soil heaped in a low rim; never deeper
    // than the interior. Everyone reading heights sees it at once (thread-safe: renderers read on workers).
    void dent(double x, double z, double radius, double depth);
    double editAt(double x, double z) const;  // how far the ground has been moved here (m, < 0 dug)
    uint64_t editsVersion() const { return editsVersion_.load(std::memory_order_acquire); }
    // The edits on an n x n grid of TERRAIN_EDIT_CELL cells from the cell holding (x0, z0) (0 where untouched).
    void editsInArea(double x0, double z0, int n, std::vector<float>& out) const;
    GroundMaterials materials(double x, double z, double height, const dvec3& normal) const;
    double waterLevelAt(double x, double z, double groundHeight) const;  // -1e30 if dry
    void pondsNear(double x, double z, double radius, std::vector<Pond>& out) const;

    // The spawn point's clearing and the campfire beside it (step 5 lights it).
    static dvec3 spawnPoint() { return {0, 0, 0}; }
    dvec3 campfirePosition() const { return campfire_; }
    static constexpr double kPondCell = 420.0;  // at most one pond per cell
    uint64_t seed() const { return seed_; }

private:
    double baseHeight(double x, double z) const;  // before basins
    bool pondInCell(int64_t cx, int64_t cz, Pond& out) const;

    uint64_t seed_;
    dvec3 campfire_;
    mutable std::shared_mutex editMutex_;
    std::unordered_map<uint64_t, float> edits_;  // per TERRAIN_EDIT_CELL cell: how far the ground moved (m)
    double editLo_[2] = {1e30, 1e30}, editHi_[2] = {-1e30, -1e30};
    std::atomic<bool> hasEdits_{false};
    std::atomic<uint64_t> editsVersion_{0};
};

}  // namespace taelia
