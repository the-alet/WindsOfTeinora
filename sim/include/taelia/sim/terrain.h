// The land: heights, pond basins with shaped beds, ground materials. Deterministic from the world seed, double
// precision, engine-free: the renderer, physics and the Unreal port all read it from here.
#pragma once
#include <cstdint>
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

    double height(double x, double z) const;
    TerrainSample sample(double x, double z) const;                  // height, normal, materials, water
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
};

}  // namespace taelia
