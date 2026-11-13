// Trees: deterministic skeletons with sane shapes, and placement that respects water, rock and the clearing.
#include "taelia/sim/plants.h"
#include "taelia/sim/terrain.h"
#include "test.h"

using namespace taelia;

TEST(trees_grow_deterministically) {
    for (int k = 0; k < int(TreeKind::Count); ++k) {
        const TreeSkeleton a = growTree(TreeKind(k), 42), b = growTree(TreeKind(k), 42), c = growTree(TreeKind(k), 43);
        CHECK(a.segments.size() == b.segments.size() && a.leaves.size() == b.leaves.size());
        CHECK(a.segments.size() > 60 && a.leaves.size() > 150);
        CHECK(a.segments[5].b.x == b.segments[5].b.x);
        CHECK(a.segments.size() != c.segments.size() || a.segments[5].b.x != c.segments[5].b.x);
        CHECK(a.height > 8.0f && a.height < 26.0f);
        CHECK(a.crownRadius > 1.0f && a.crownRadius < a.height);
        for (const TreeSegment& s : a.segments) {
            CHECK(s.ra >= s.rb - 1e-4f);                  // limbs taper
            CHECK(s.parent < int(&s - a.segments.data()));  // parents come first
            CHECK(s.b.y > -0.5f);                          // nothing grows into the ground
        }
    }
}

TEST(trees_keep_out_of_water_rock_and_the_clearing) {
    Terrain t(1);
    std::vector<TreeInstance> trees;
    treesInArea(t, -400, -400, 400, 400, trees);
    CHECK(trees.size() > 200);
    std::vector<TreeInstance> again;
    treesInArea(t, -400, -400, 400, 400, again);
    CHECK(again.size() == trees.size());
    const dvec3 fire = t.campfirePosition();
    for (const TreeInstance& tr : trees) {
        CHECK(std::hypot(tr.position.x, tr.position.z) >= 16.0);
        CHECK(std::hypot(tr.position.x - fire.x, tr.position.z - fire.z) >= 9.0);
        const TerrainSample s = t.sample(tr.position.x, tr.position.z);
        CHECK(!s.underwater());
        CHECK(std::fabs(tr.position.y - (s.height - 0.08)) < 1e-6);
    }
    // Split areas give the same trees as one big area.
    std::vector<TreeInstance> left, right;
    treesInArea(t, -400, -400, 0, 400, left);
    treesInArea(t, 0, -400, 400, 400, right);
    CHECK(left.size() + right.size() == trees.size());
}
