// The ground's insides and things deforming where they hit (the user): strata harder with depth down to the bedrock
// past the 10 m interior; a hit dents the ground by the energy it loses and the hardness there (a bowl and a rim, never
// deeper than the interior); a dropped ball leaves a dent and squashes; a hard landing presses the feet in.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "taelia/shared_constants.h"
#include "taelia/sim/world.h"
#include "test.h"

using namespace taelia;

TEST(the_ground_is_harder_with_depth_down_to_the_bedrock) {
    Terrain t(1);
    const double x = 3.0, z = 2.0;  // the spawn's clearing: topsoil over clay
    const double depths[5] = {0.1, 1.0, 2.5, 6.0, 12.0};
    double last = 0;
    for (const double d : depths) {
        const double hard = t.stratumAt(x, z, d).hardness;
        CHECK(hard >= last);
        last = hard;
    }
    CHECK(t.stratumAt(x, z, 0.1).hardness < t.stratumAt(x, z, 12.0).hardness / 100.0);
    CHECK(t.stratumAt(x, z, k::TERRAIN_INTERIOR + 0.1).hardness == double(k::HARDNESS_BEDROCK));
}

TEST(a_dent_is_a_bowl_with_a_rim_and_never_deeper_than_the_interior) {
    Terrain t(1);
    const double x = 3.0, z = 2.0;
    const double h0 = t.height(x, z), rim0 = t.height(x + 0.65, z);
    t.dent(x, z, 0.5, 0.2);
    CHECK_NEAR(h0 - t.height(x, z), 0.2, 0.02);       // the bowl
    CHECK(t.height(x + 0.65, z) > rim0 + 0.005);       // the rim
    CHECK_NEAR(t.height(x + 3.0, z), t.untouchedHeight(x + 3.0, z), 1e-12);  // untouched beyond
    CHECK(t.editsVersion() > 0);
    for (int i = 0; i < 20; ++i) t.dent(x, z, 0.5, 5.0);
    CHECK(t.editAt(x, z) >= -k::TERRAIN_INTERIOR - 1e-6);  // down to the bedrock, no further
}

TEST(a_hit_dents_the_ground_by_its_energy_and_the_hardness_there) {
    WorldDesc d;
    World w(d);
    const dvec3 p{4.0, w.groundHeight(4.0, -3.0), -3.0};
    w.dentGround(p, 10.0, 0.08);
    const double small = -w.terrain().editAt(p.x, p.z);
    const dvec3 q{-4.0, w.groundHeight(-4.0, -3.0), -3.0};
    w.dentGround(q, 40.0, 0.08);
    const double big = -w.terrain().editAt(q.x, q.z);
    CHECK(small > 0.002);
    CHECK(big > 1.5 * small);  // more energy, more pushed aside
    CHECK(big < 0.2);
}

TEST(a_ball_thrown_down_dents_soft_ground_and_squashes) {
    WorldDesc d;
    World w(d);
    const dvec3 at{4.0, 0.0, -3.0};  // the clearing: topsoil
    const double ground = w.groundHeight(at.x, at.z);
    w.setFocus({at.x, ground + 2.0, at.z});
    w.throwBall({at.x, ground + 1.2, at.z}, {0, -16.0, 0});  // thrown down hard (tall grass slows a mere drop)
    double squash = 0;
    for (int i = 0; i < 90 && !w.balls().empty(); ++i) {
        w.step();
        squash = std::max(squash, w.balls().front().squash);
    }
    CHECK(squash > 0.03);
    CHECK(squash <= k::BALL_SQUASH_MAX + 1e-9);
    double deepest = 0;
    for (double dx = -0.3; dx <= 0.3; dx += 0.05)
        for (double dz = -0.3; dz <= 0.3; dz += 0.05) deepest = std::min(deepest, w.terrain().editAt(at.x + dx, at.z + dz));
    std::printf("  (edits %llu, deepest %.5f, squash %.3f)\n", (unsigned long long)w.terrain().editsVersion(), deepest, squash);
    CHECK(deepest < -0.001);  // a dent where it landed
    CHECK(deepest > -0.06);
}

TEST(a_hard_landing_presses_the_feet_into_the_ground) {
    WorldDesc d;
    World w(d);
    PlayerInput in;
    in.fly = true;
    in.jump = true;
    w.setPlayerInput(in);
    for (int i = 0; i < 60; ++i) w.step();  // up about 8 m
    in.fly = true;  // stop flying: it falls
    in.jump = false;
    w.setPlayerInput(in);
    w.step();
    in.fly = false;
    w.setPlayerInput(in);
    for (int i = 0; i < 240 && w.player().mode != PlayerMode::Ground; ++i) w.step();
    CHECK(w.player().mode == PlayerMode::Ground);
    const dvec3 feet = w.humanPosition();
    double deepest = 0;
    for (double dx = -0.4; dx <= 0.4; dx += 0.05)
        for (double dz = -0.4; dz <= 0.4; dz += 0.05) deepest = std::min(deepest, w.terrain().editAt(feet.x + dx, feet.z + dz));
    CHECK(deepest < -0.01);
}
