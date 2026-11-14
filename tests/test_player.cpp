// The player (the user: the human is the player): it runs, jumps about 1.3 m, crouches slower, dashes, swims in deep
// water and walks out, flies with F and falls when it stops, keeps out of trunks; its magic balls gather the chosen
// aspect (neutral magic slower), fly straight and burst where they hit, leaving their magic in the field there.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "taelia/shared_constants.h"
#include "taelia/sim/world.h"
#include "test.h"

using namespace taelia;

namespace {

void stepFor(World& w, double seconds) {
    for (int i = 0; i < int(seconds * 60.0 + 0.5); ++i) w.step();
}

double flatSpeed(const Player& p) { return std::hypot(p.velocity.x, p.velocity.z); }

}  // namespace

TEST(player_runs_jumps_about_1_3_m_and_lands) {
    WorldDesc d;
    World w(d);
    PlayerInput in;
    in.forward = 1;
    w.setPlayerInput(in);
    stepFor(w, 1.0);
    CHECK_NEAR(flatSpeed(w.player()), k::PLAYER_SPEED, 0.15);
    CHECK(w.player().mode == PlayerMode::Ground);
    CHECK(w.player().gait > 0.5);  // its legs go round
    in.forward = 0;
    in.jump = true;
    w.setPlayerInput(in);
    double top = 0;
    bool left = false;
    for (int i = 0; i < 120; ++i) {
        w.step();
        const Player& p = w.player();
        top = std::max(top, p.position.y - w.groundHeight(p.position.x, p.position.z));
        left = left || p.mode == PlayerMode::Air;
    }
    CHECK(left);
    CHECK(top > 1.1);
    CHECK(top < 1.45);
    CHECK(w.player().mode == PlayerMode::Ground);  // landed (one jump while Space stays down)
}

TEST(player_crouching_moves_slower_and_a_dash_bursts) {
    WorldDesc d;
    World w(d);
    PlayerInput in;
    in.forward = 1;
    in.crouch = true;
    w.setPlayerInput(in);
    stepFor(w, 1.0);
    CHECK_NEAR(flatSpeed(w.player()), k::PLAYER_CROUCH_SPEED, 0.1);
    CHECK(w.player().crouch > 0.9);
    in.crouch = false;
    in.dash = true;
    w.setPlayerInput(in);
    w.step();
    CHECK(flatSpeed(w.player()) > 0.9 * k::PLAYER_DASH_SPEED);
    in.dash = false;
    w.setPlayerInput(in);
    stepFor(w, 1.0);
    CHECK_NEAR(flatSpeed(w.player()), k::PLAYER_SPEED, 0.3);  // back to a run
}

TEST(player_swims_in_deep_water_and_walks_out) {
    WorldDesc d;
    World w(d);
    std::vector<Pond> ponds;
    w.terrain().pondsNear(0, 0, 600, ponds);
    const Pond* deep = nullptr;
    for (const Pond& p : ponds)
        if (p.depth > k::PLAYER_SWIM_DEPTH + 0.4 && (!deep || std::hypot(p.cx, p.cz) < std::hypot(deep->cx, deep->cz))) deep = &p;
    CHECK(deep != nullptr);
    if (!deep) return;
    std::printf("  (the nearest deep pond: centre %.1f %.1f, level %.2f, depth %.2f, radius %.1f)\n", deep->cx, deep->cz, deep->level,
                deep->depth, deep->radius);
    w.placePlayer({deep->cx, 0, deep->cz}, 0.0);
    CHECK(w.player().mode == PlayerMode::Swim);
    CHECK_NEAR(w.player().position.y, deep->level, 0.05);
    PlayerInput in;
    in.forward = 1;
    w.setPlayerInput(in);
    stepFor(w, 1.0);
    CHECK(flatSpeed(w.player()) < k::PLAYER_SWIM_SPEED + 0.1);
    CHECK(w.player().mode == PlayerMode::Swim);
    stepFor(w, 2.0 * deep->radius / k::PLAYER_SWIM_SPEED + 30.0);  // out across the pond
    CHECK(w.player().mode == PlayerMode::Ground);
}

TEST(player_flies_with_f_and_falls_when_it_stops) {
    WorldDesc d;
    World w(d);
    PlayerInput in;
    in.fly = true;
    w.setPlayerInput(in);
    w.step();
    CHECK(w.player().mode == PlayerMode::Fly);
    in.fly = false;
    in.jump = true;  // rise
    w.setPlayerInput(in);
    stepFor(w, 3.0);
    const Player& p = w.player();
    const double up = p.position.y - w.groundHeight(p.position.x, p.position.z);
    CHECK(up > 8.0);
    CHECK(p.velocity.y <= k::PLAYER_FLY_SPEED + 0.01);
    in.jump = false;
    in.fly = true;  // stops flying
    w.setPlayerInput(in);
    w.step();
    CHECK(w.player().mode == PlayerMode::Air);
    in.fly = false;
    w.setPlayerInput(in);
    stepFor(w, 4.0);
    CHECK(w.player().mode == PlayerMode::Ground);
}

TEST(player_keeps_out_of_trunks) {
    WorldDesc d;
    World w(d);
    std::vector<TreeInstance> trees;
    treesInArea(w.terrain(), -60, -60, 60, 60, trees);
    CHECK(!trees.empty());
    if (trees.empty()) return;
    const TreeInstance t = trees[0];
    // From 6 m away, running straight at it.
    w.placePlayer(t.position + dvec3{-6.0, 0, 0}, kPi * 0.5);
    PlayerInput in;
    in.forward = 1;
    in.yaw = kPi * 0.5;  // toward +x
    w.setPlayerInput(in);
    double nearest = 1e9;
    for (int i = 0; i < 180; ++i) {
        w.step();
        const dvec3 q = w.player().position;
        nearest = std::min(nearest, std::hypot(q.x - t.position.x, q.z - t.position.z));
    }
    CHECK(nearest > k::PLAYER_RADIUS);
}

TEST(magic_balls_gather_their_aspect_faster_than_neutral_and_stop_full) {
    // Earth is rich near the ground; fire away from the campfire is none, so a fire ball fills from neutral alone.
    double kg[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        WorldDesc d;
        World w(d);
        w.placePlayer({40, 0, -40}, 0.0);
        w.setFocus(w.humanPosition() + dvec3{0, 1.6, 0});
        stepFor(w, 0.5);
        PlayerInput in;
        in.gather = true;
        in.aspect = i == 0 ? Aspect::Earth : Aspect::Fire;
        w.setPlayerInput(in);
        stepFor(w, 1.0);
        kg[i] = w.gathering().kg;
        CHECK(w.gathering().aspect == in.aspect);
        if (i == 1) {
            stepFor(w, 40.0);
            CHECK(w.gathering().kg <= k::MAGIC_BALL_FULL + 1e-6);
            CHECK(w.gathering().kg > 0.5 * k::MAGIC_BALL_FULL);
        }
    }
    CHECK(kg[1] > 0.0);
    CHECK(kg[0] > 1.5 * kg[1]);
}

TEST(a_thrown_magic_ball_flies_straight_and_bursts_where_it_hits) {
    WorldDesc d;
    World w(d);
    w.placePlayer({40, 0, -40}, 0.0);
    w.setFocus(w.humanPosition() + dvec3{0, 1.6, 0});
    stepFor(w, 0.5);
    PlayerInput in;
    in.gather = true;
    in.face = true;
    in.aspect = Aspect::Lightning;
    w.setPlayerInput(in);
    stepFor(w, 6.0);
    const double held = w.gathering().kg;
    CHECK(held > k::MAGIC_BALL_MIN);
    // Aimed at the ground 12 m ahead.
    const dvec3 feet = w.humanPosition();
    const dvec3 target{feet.x, w.groundHeight(feet.x, feet.z + 12.0), feet.z + 12.0};
    const double before = w.magicAt(target + dvec3{0, 0.5, 0}).rho[Aspect::Lightning];
    in.gather = false;
    in.aim = target;
    w.setPlayerInput(in);
    w.step();
    CHECK(w.gathering().kg == 0.0);
    CHECK(w.magicBalls().size() == 1u);
    CHECK(w.player().toss >= 0.0);  // the throwing arm
    if (w.magicBalls().empty()) return;
    const dvec3 v0 = w.magicBalls().front().velocity;
    for (int i = 0; i < 120 && !w.magicBalls().empty(); ++i) {
        const dvec3 v = w.magicBalls().front().velocity;
        CHECK(length(v - v0) < 1e-9);  // weightless: straight on
        w.step();
    }
    CHECK(w.magicBalls().empty());  // it hit the ground and burst
    CHECK(w.magicAt(target + dvec3{0, 0.5, 0}).rho[Aspect::Lightning] > before + 1.0);
}
