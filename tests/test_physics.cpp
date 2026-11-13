#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "taelia/shared_constants.h"
#include "taelia/sim/physics.h"
#include "taelia/sim/plants.h"
#include "taelia/sim/terrain.h"
#include "taelia/sim/wind.h"
#include "taelia/sim/world.h"
#include "test.h"

using namespace taelia;

namespace {

constexpr double kDt = 1.0 / 60.0;
const double R = k::BALL_RADIUS;

// Steps a flat test world (no terrain, no plants) until the ball first comes back down to the ground.
struct Flight {
    double landX = 0, landZ = 0, seconds = 0;
};

Flight flyUntilLanding(Physics& p, double maxSeconds) {
    Flight f;
    double t = 0;
    bool rising = p.balls().back().velocity.y > 0;
    while (t < maxSeconds) {
        p.step(kDt, t += kDt);
        const Ball& b = p.balls().back();
        if (b.velocity.y > 0) rising = true;
        if (b.position.y < R + 0.02 && (rising || b.velocity.y <= 0)) {
            f = {b.position.x, b.position.z, t};
            return f;
        }
    }
    return f;
}

}  // namespace

TEST(soft_ground_bounces_a_dropped_ball_low_and_it_settles) {
    Physics p(nullptr, nullptr, 1);
    p.throwBall({0, 2.0 + R, 0}, {0, 0, 0});
    double t = 0, apex = 0;
    int bounces = 0;
    bool down = true;
    for (int i = 0; i < 8 * 60; ++i) {
        p.step(kDt, t += kDt);
        const Ball& b = p.balls()[0];
        if (down && b.velocity.y > 0.05) {
            down = false;
            ++bounces;
        }
        if (!down && b.velocity.y < 0) down = true;
        if (bounces == 1) apex = std::max(apex, b.position.y - R);
    }
    const Ball& b = p.balls()[0];
    CHECK(bounces >= 1);
    CHECK(apex > 0.1);   // it does bounce...
    CHECK(apex < 0.65);  // ...but nothing like off a perfect surface (2 m)
    CHECK(std::abs(b.position.y - R) < 0.015);
    CHECK(length(b.velocity) < 0.05);
}

TEST(a_ball_rolled_over_grass_stops) {
    Physics p(nullptr, nullptr, 1);
    p.throwBall({0, R + 0.001, 0}, {4, 0, 0});
    double t = 0;
    for (int i = 0; i < 8 * 60; ++i) p.step(kDt, t += kDt);
    const Ball& b = p.balls()[0];
    CHECK(length(b.velocity) < 0.05);
    CHECK(b.position.x > 1.0);
    CHECK(b.position.x < 12.0);
}

TEST(air_drag_shortens_a_throw) {
    Physics p(nullptr, nullptr, 1);
    const double a = 35.0 * 3.14159265358979 / 180.0, v = 20.0, y0 = 1.5;
    p.throwBall({0, y0, 0}, {v * std::cos(a), v * std::sin(a), 0});
    const Flight f = flyUntilLanding(p, 10.0);
    const double vy = v * std::sin(a), drop = y0 - R;
    const double vacuum = v * std::cos(a) * (vy + std::sqrt(vy * vy + 2 * 9.81 * drop)) / 9.81;
    CHECK(f.seconds > 0);
    CHECK(f.landX < 0.9 * vacuum);
    CHECK(f.landX > 0.5 * vacuum);
}

TEST(the_wind_carries_a_falling_ball) {
    WindField wind(5);
    wind.setMean({8, 0, 0});
    Physics windy(nullptr, &wind, 1), still(nullptr, nullptr, 1);
    windy.throwBall({0, 10, 0}, {0, 0, 0});
    still.throwBall({0, 10, 0}, {0, 0, 0});
    const Flight a = flyUntilLanding(windy, 5.0), b = flyUntilLanding(still, 5.0);
    CHECK(a.landX > 0.25);
    CHECK(std::abs(b.landX) < 0.01);
}

TEST(old_balls_go_past_the_limit) {
    Physics p(nullptr, nullptr, 1);
    for (int i = 0; i < k::BALL_MAX + 5; ++i) p.throwBall({double(i), 1, 0}, {0, 0, 0});
    CHECK(int(p.balls().size()) == k::BALL_MAX);
    CHECK(p.balls().front().id == 6u);
}

TEST(the_wind_is_not_one_vector_everywhere) {
    WindField w(11);
    w.setMean({5, 0, 0});
    w.step(30000.0);  // far into a day: the pattern has ridden the wind a long way
    double sum = 0, sumZ = 0, sumZ2 = 0, low = 0;
    const int n = 400;
    for (int i = 0; i < n; ++i) {
        const dvec3 p{i * 7.3, 10.0, i * 3.1};
        const dvec3 a = w.at(p, 10.0);
        sum += a.x;
        sumZ += a.z;
        sumZ2 += a.z * a.z;
        low += w.at(p, 0.5).x;
    }
    const double mean = sum / n, mz = sumZ / n, sdZ = std::sqrt(std::max(sumZ2 / n - mz * mz, 0.0));
    CHECK(std::abs(mean - 5.0) < 1.0);
    CHECK(sdZ > 0.75 * (0.28 * 5.0 + 0.22));  // eddies across the wind as strong as meant
    CHECK(sdZ < 1.25 * (0.28 * 5.0 + 0.22));
    CHECK(low / n < 0.75 * mean);  // slower near the ground
    // Coherent up and down: 10 cm apart the air is nearly the same, even this late.
    const dvec3 q{40, 1.0, 25};
    CHECK(length(w.at(q, 1.0) - w.at(q + dvec3{0, 0.1, 0}, 1.1)) < 0.3 * (0.28 * 5.0 + 0.22));
    const dvec3 before = w.at({3, 2, 4}, 2);
    CHECK(length(before - w.at({3, 2, 4}, 2)) == 0.0);
    w.step(3.0);
    CHECK(length(before - w.at({3, 2, 4}, 2)) > 0.0);  // it moves on in time
    WindField calm(11);
    const double stir = length(calm.at({10, 3, 10}, 3));
    CHECK(stir > 0.0);
    CHECK(stir < 1.5);
}

TEST(a_gale_moves_a_resting_ball_and_a_breeze_does_not) {
    for (const double gale : {10.0, 120.0}) {
        WindField wind(3);
        Physics p(nullptr, &wind, 1);
        p.throwBall({0, R + 0.001, 0}, {0, 0, 0});
        double t = 0;
        for (int i = 0; i < 60; ++i) p.step(kDt, t += kDt);
        CHECK(p.balls()[0].resting);
        const double x0 = p.balls()[0].position.x;
        wind.setMean({gale, 0, 0});
        for (int i = 0; i < 5 * 60; ++i) p.step(kDt, t += kDt);
        const double moved = p.balls()[0].position.x - x0;
        if (gale > 50.0) CHECK(moved > 0.5);
        else CHECK(std::abs(moved) < 0.01);
    }
}

namespace {

uint64_t throwSome(World& w, double nudge) {
    const double g = w.groundHeight(2, 1);
    w.throwBall({2, g + 1.6, 1}, {9 + nudge, 6, 3});
    w.throwBall({2, g + 1.6, 1}, {-5, 4, 8});
    for (int i = 0; i < 120; ++i) w.step();
    w.throwBall({2, g + 1.6, 1}, {0, 0, 0});
    for (int i = 0; i < 300; ++i) w.step();
    return w.stateHash();
}

}  // namespace

TEST(the_same_throws_give_the_same_world) {
    WorldDesc d;
    d.seed = 7;
    World a(d), b(d), c(d);
    const uint64_t ha = throwSome(a, 0), hb = throwSome(b, 0), hc = throwSome(c, 1e-6);
    CHECK(ha == hb);
    CHECK(ha != hc);
    CHECK(a.balls().size() == 3u);
}

TEST(a_ball_floats_on_a_pond) {
    WorldDesc d;
    World w(d);
    std::vector<Pond> ponds;
    w.terrain().pondsNear(0, 0, 4000, ponds);
    const Pond* pond = nullptr;
    for (const Pond& p : ponds)
        if (p.depth > 0.6) {
            pond = &p;
            break;
        }
    CHECK(pond != nullptr);
    if (!pond) return;
    w.throwBall({pond->cx, pond->level + 1.0, pond->cz}, {0, 0, 0});
    for (int i = 0; i < 6 * 60; ++i) w.step();
    const Ball& b = w.balls()[0];
    CHECK(b.inWater);
    CHECK(b.position.y > pond->level - R);
    CHECK(b.position.y < pond->level + R);
}

namespace {

// A tall tree near the spawn, with its skeleton.
bool findTree(const World& w, TreeInstance& out, TreeSkeleton& sk) {
    std::vector<TreeInstance> trees;
    treesInArea(w.terrain(), -80, -80, 80, 80, trees);
    std::sort(trees.begin(), trees.end(), [](const TreeInstance& a, const TreeInstance& b) {
        return std::hypot(a.position.x, a.position.z) < std::hypot(b.position.x, b.position.z);
    });
    for (const TreeInstance& t : trees) {
        sk = growTree(t.kind, treeSkeletonSeed(t.kind, t.variant));
        if (sk.height * t.scale > 6.0 && sk.trunkRadius * t.scale > 0.08) {
            out = t;
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(a_trunk_throws_the_ball_back) {
    WorldDesc d;
    World w(d);
    TreeInstance t;
    TreeSkeleton sk;
    CHECK(findTree(w, t, sk));
    const dvec3 aim = t.position + dvec3{0, 1.0, 0};
    const dvec3 from = aim + dvec3{4, 0, 0};
    w.throwBall(from, {-10, 0, 0});
    double nearest = 1e9, lastX = 0;
    for (int i = 0; i < 60; ++i) {
        w.step();
        nearest = std::min(nearest, w.balls()[0].position.x - t.position.x);
        lastX = w.balls()[0].position.x;
    }
    CHECK(nearest > -0.2);                // it did not go through the trunk
    CHECK(lastX > t.position.x + nearest + 0.3);  // it came back
}

namespace {

dvec3 treeToWorld(const TreeInstance& t, const fvec3& p) {
    const double c = std::cos(t.yaw), s = std::sin(t.yaw);
    return t.position + dvec3{p.x * c + p.z * s, p.y, -p.x * s + p.z * c} * double(t.scale);
}

double pointToSegment(const dvec3& p, const dvec3& a, const dvec3& b) {
    const dvec3 ab = b - a;
    const double u = std::clamp(dot(p - a, ab) / std::max(dot(ab, ab), 1e-12), 0.0, 1.0);
    return length(p - (a + ab * u));
}

// A throw through the crown that stays clear of the trunk and every bough (capsules), past as many leaves as can be.
bool crownThrow(const World& w, const TreeInstance& t, const TreeSkeleton& sk, dvec3& from, dvec3& vel) {
    struct Seg { dvec3 a, b; double r; };
    std::vector<Seg> thick;
    std::vector<TreeInstance> near;
    treesInArea(w.terrain(), t.position.x - 25, t.position.z - 25, t.position.x + 25, t.position.z + 25, near);
    for (const TreeInstance& n : near) {
        const TreeSkeleton s = growTree(n.kind, treeSkeletonSeed(n.kind, n.variant));
        for (const TreeSegment& g : s.segments)
            if (std::max(g.ra, g.rb) >= 0.03) thick.push_back({treeToWorld(n, g.a), treeToWorld(n, g.b), std::max(g.ra, g.rb) * n.scale});
    }
    const double h = sk.height * t.scale, cr = sk.crownRadius * t.scale;
    int best = 0;
    for (double fy = 0.4; fy <= 0.86; fy += 0.08) {
        for (double fz = -0.7; fz <= 0.71; fz += 0.1) {
            const dvec3 f = t.position + dvec3{-6, fy * h, fz * cr}, v{18, 3, 0};
            // The path in still air, with drag, as the ball will fly it until something touches it.
            std::vector<dvec3> path;
            const double cd = 0.5 * 1.2 * 0.47 * 3.14159265358979 * R * R / k::BALL_MASS, dt = 0.004;
            dvec3 pp = f, pv = v;
            for (double s = 0; s < 0.6; s += dt) {
                path.push_back(pp);
                pv = pv + (dvec3{0, -9.81, 0} - pv * (cd * length(pv))) * dt;
                pp = pp + pv * dt;
            }
            bool clear = true;
            for (const dvec3& p : path) {
                for (const Seg& g : thick)
                    if (pointToSegment(p, g.a, g.b) < R + g.r + 0.15) clear = false;
                if (!clear) break;
            }
            if (!clear) continue;
            int leaves = 0;
            for (const LeafCard& l : sk.leaves) {
                const dvec3 c = treeToWorld(t, l.center);
                for (size_t i = 1; i < path.size(); ++i)
                    if (pointToSegment(c, path[i - 1], path[i]) < R + 0.3 * l.size * t.scale) {
                        ++leaves;
                        break;
                    }
            }
            if (leaves > best) {
                best = leaves;
                from = f;
                vel = v;
            }
        }
    }
    return best >= 6;
}

}  // namespace

TEST(a_crown_slows_the_ball) {
    WorldDesc d;
    d.settings.windSpeed = 0;  // still air: only the plants change the path
    World w(d);
    TreeInstance t;
    TreeSkeleton sk;
    CHECK(findTree(w, t, sk));
    dvec3 from, vel;
    CHECK(crownThrow(w, t, sk, from, vel));
    w.throwBall(from, vel);
    Physics open(nullptr, nullptr, 1);
    open.throwBall({0, 30, 0}, vel);
    double s = 0;
    for (int i = 0; i < 36; ++i) {
        w.step();
        open.step(kDt, s += kDt);
    }
    const Ball& b = w.balls()[0];
    const PhysicsStats st = w.physicsStats();
    CHECK(st.leafHits + st.twigHits > 0);
    CHECK(b.position.x > from.x + 3.0);  // into the crown, not thrown back: only yielding plants were in the way
    CHECK(b.velocity.x > 0.0);
    CHECK(length(b.velocity) < 0.95 * length(open.balls()[0].velocity));
}
