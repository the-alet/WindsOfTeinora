// Trees: invented species grown from a seed into skeletons (branch segments and leaf cards), and where they stand.
// Engine-free: the renderer builds meshes from the skeletons, physics will use the branches as elastic colliders.
#pragma once
#include <cstdint>
#include <vector>

#include "taelia/sim/math.h"

namespace taelia {

class Terrain;

// Invented species (the world is invented): their shapes follow how real trees grow.
enum class TreeKind : uint8_t {
    Veyl,     // tall and slender, pale bark, a narrow high crown of small round leaves
    Ombrel,   // short thick trunk, a broad spreading crown, dark furrowed bark
    Sarn,     // straight spire with drooping tiers of needle sprays
    Count
};
const char* treeKindName(TreeKind k);

struct fvec3 {
    float x = 0, y = 0, z = 0;
};

struct TreeSegment {
    fvec3 a, b;          // local metres, y up, the trunk base at the origin
    float ra = 0, rb = 0;  // radius at a and b
    int32_t parent = -1;   // segment this one continues or branches from
    uint8_t order = 0;     // 0 trunk, 1 main branches, 2 twigs, ...
};

struct LeafCard {
    fvec3 center;   // local metres
    fvec3 normal;   // the card faces this way
    fvec3 up;       // along the card's length
    float size = 0;  // edge length in metres
};

struct TreeSkeleton {
    TreeKind kind = TreeKind::Veyl;
    std::vector<TreeSegment> segments;
    std::vector<LeafCard> leaves;
    float height = 0, crownRadius = 0, trunkRadius = 0;
};

// Deterministic: the same kind and seed give the same tree.
TreeSkeleton growTree(TreeKind kind, uint64_t seed);

inline constexpr int kTreeVariants = 4;  // skeletons per species; instances pick one and vary scale and turn

struct TreeInstance {
    dvec3 position;   // trunk base on the ground
    float yaw = 0;    // radians
    float scale = 1;
    TreeKind kind = TreeKind::Veyl;
    uint8_t variant = 0;
};

// Trees standing in [x0, x1) x [z0, z1), deterministic from the terrain's seed. Not in water, on bare rock,
// or in the clearing around the spawn point and the campfire.
void treesInArea(const Terrain& terrain, double x0, double z0, double x1, double z1, std::vector<TreeInstance>& out);

inline constexpr double kTreeCell = 7.0;  // at most one tree per cell

}  // namespace taelia
