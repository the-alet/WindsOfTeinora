// Inside the physics: PhysX types live only here and in physics.cpp / physics_plants.cpp.
#pragma once
#include <PxPhysicsAPI.h>

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "taelia/sim/physics.h"
#include "taelia/sim/plants.h"

namespace taelia::phys {

inline physx::PxVec3 toPx(const dvec3& v) { return physx::PxVec3(float(v.x), float(v.y), float(v.z)); }
inline dvec3 toD(const physx::PxVec3& v) { return {double(v.x), double(v.y), double(v.z)}; }

// Ground surfaces; the order is the material index stored in the height field samples.
enum Surface : uint8_t { kGrass, kDirt, kRock, kSand, kMud, kSurfaceCount };

struct Ground {
    double height = 0;
    dvec3 normal{0, 1, 0};
    Surface surface = kGrass;
    double grassHeight = 0.06;  // m: how high the blades stand here
    double water = -1e30;       // pond surface, if above the ground
};

using CellKey = std::pair<int64_t, int64_t>;

// A twig or leaf spray a ball's path crossed, to be applied in order along the path.
struct PlantHit {
    double along = 0;  // 0..1 along the path
    uint64_t id = 0;   // one per twig, one per leaf spray
    dvec3 normal;      // the push, tree frame
    double share = 0, bounce = 0, rub = 0;
    bool twig = false;
    const TreeInstance* tree = nullptr;
};

struct BallBody {
    Ball state;
    physx::PxRigidDynamic* actor = nullptr;
    dvec3 prevPosition;
    double prevRotation[4] = {0, 0, 0, 1};
    std::vector<uint64_t> touching, touchingNext;  // twigs / leaf sprays overlapped on the last sub-step
    int slowSubsteps = 0;                          // in a row, slow on the ground: it may settle
    Ground rest;                                   // the ground under it while asleep
    bool restKnown = false;
};

// A height field tile: 1 m samples, and per 2 m cell the surface, grass and pond level the ball needs.
struct Tile {
    physx::PxRigidStatic* actor = nullptr;  // null until built
    physx::PxShape* shape = nullptr;
    physx::PxHeightField* field = nullptr;
    double x0 = 0, z0 = 0, lo = 0, hi = 0;
    std::vector<uint8_t> surface;
    std::vector<float> grass, water;
    int64_t lastUsed = 0;
};

// Thin twigs and leaf sprays of one skeleton, bucketed in a grid in the tree's own (unscaled) frame.
struct SkeletonGrid {
    const TreeSkeleton* skeleton = nullptr;
    std::vector<int32_t> thin;          // segment indices thinner than the capsule limit (at scale 1)
    std::vector<int32_t> chain;         // per thin element: the twig it belongs to (consecutive thin segments)
    std::vector<double> beyond;         // per thin element: kg of its twig from there out to the tip (scale 1)
    double reach = 0;                   // how far it spreads from the trunk's axis, twigs and leaves included
    double lo[3] = {0, 0, 0};           // grid origin
    int dim[3] = {0, 0, 0};
    std::vector<uint32_t> cellStart;    // dim product + 1
    std::vector<uint32_t> items;        // element ids: < thin.size() a twig segment, else a leaf spray
    std::vector<uint32_t> stamp;        // per element, for one visit per query
    uint32_t query = 0;
};

struct TreeBody {
    TreeInstance tree;
    uint64_t key = 0;
    int skeleton = 0;  // kind * kTreeVariants + variant
    physx::PxRigidStatic* actor = nullptr;
    int shapes = 0;
    int64_t lastUsed = 0;
};

}  // namespace taelia::phys

namespace taelia {

struct Physics::Impl {
    const Terrain* terrain = nullptr;
    const WindField* wind = nullptr;
    uint64_t seed = 0;

    physx::PxPhysics* px = nullptr;
    physx::PxDefaultCpuDispatcher* dispatcher = nullptr;
    physx::PxScene* scene = nullptr;
    physx::PxMaterial* ballMaterial = nullptr;
    physx::PxMaterial* barkMaterial = nullptr;
    physx::PxMaterial* surfaces[phys::kSurfaceCount] = {};
    physx::PxRigidStatic* flatGround = nullptr;  // tests without terrain

    dvec3 origin{};  // PhysX coordinates are world minus this, so floats stay precise far out
    bool originSet = false;
    std::vector<phys::BallBody> balls;
    std::vector<Ball> view;
    uint32_t nextId = 1;
    int64_t stepIndex = 0;
    double seconds = 0;
    int tilesBuiltThisStep = 0;
    std::map<phys::CellKey, phys::Tile> tiles;
    std::map<phys::CellKey, phys::TreeBody> trees;
    std::map<phys::CellKey, int64_t> emptyTreeCells;  // tree cells known to hold no tree, and when last wanted
    double treeReach = 0;                             // the widest crown so far (m from its trunk), plus a ball
    std::vector<TreeSkeleton> skeletons;              // kind * kTreeVariants + variant, grown on first use
    std::vector<phys::SkeletonGrid> grids;
    std::vector<TreeInstance> scratchTrees;
    std::vector<phys::PlantHit> plantHits;
    std::map<uint32_t, physx::PxRigidStatic*> rocks;
    Physics::MagicSampler magic;
    PhysicsStats stats;

    Impl(const Terrain* t, const WindField* w, uint64_t s);
    ~Impl();

    phys::Ground groundUnder(const dvec3& p) const;
    void substep(double h, bool first);
    void applyAirWaterGround(phys::BallBody& b, double h);
    void streamGround(const dvec3& p, double radius);
    void buildTile(const phys::CellKey& key, phys::Tile& tile);
    void releaseUnusedGround();
    void removeBall(size_t i);

    // physics_plants.cpp
    void streamTrees(const dvec3& p, double radius);
    void addTree(const TreeInstance& t, const phys::CellKey& key);
    void releaseUnusedTrees();
    void releaseTree(phys::TreeBody& t);
    void plantContacts(phys::BallBody& b, const dvec3& from, const dvec3& to);
    phys::SkeletonGrid& grid(int skeleton);
};

}  // namespace taelia
