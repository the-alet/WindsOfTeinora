// The land: determinism, precision far away, the spawn clearing, ponds that hold their water, materials.
#include "taelia/sim/terrain.h"
#include "test.h"

using namespace taelia;

TEST(terrain_is_deterministic_from_the_seed) {
    Terrain a(5), b(5), c(6);
    int differ = 0;
    for (int i = 0; i < 64; ++i) {
        const double x = i * 137.3 - 4000, z = i * 71.9 + 1200;
        CHECK(a.height(x, z) == b.height(x, z));
        differ += a.height(x, z) != c.height(x, z);
    }
    CHECK(differ > 60);
}

TEST(terrain_is_smooth_far_away) {
    Terrain t(3);
    for (double base : {0.0, 1.0e7, -2.5e7}) {
        double maxStep = 0, prev = t.height(base, base * 0.3);
        for (int i = 1; i <= 800; ++i) {
            const double h = t.height(base + i * 0.25, base * 0.3);
            maxStep = std::fmax(maxStep, std::fabs(h - prev));
            prev = h;
        }
        CHECK(maxStep < 0.6);  // 25 cm steps never jump, even 25,000 km out
    }
}

TEST(terrain_has_relief) {
    Terrain t(1);
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i < 400; ++i) {
        const double h = t.height(1000.0 + i * 25.0, -3000.0 + i * 11.0);
        lo = std::fmin(lo, h);
        hi = std::fmax(hi, h);
    }
    CHECK(hi - lo > 15.0);  // hills, not a plain
}

TEST(spawn_is_a_gentle_dry_clearing_near_a_pond) {
    Terrain t(1);
    const TerrainSample s = t.sample(0, 0);
    CHECK(s.normal.y > std::cos(15.0 * kDegToRad));
    CHECK(!s.underwater());
    std::vector<Pond> ponds;
    t.pondsNear(0, 0, 80, ponds);
    CHECK(!ponds.empty());
    const dvec3 fire = t.campfirePosition();
    CHECK(!t.sample(fire.x, fire.z).underwater());
    CHECK(t.sample(fire.x, fire.z).materials.dirt > 0.8f);  // trodden ground around the fire
}

TEST(ponds_hold_their_water) {
    for (uint64_t seed : {1ull, 2ull, 9ull}) {
        Terrain t(seed);
        std::vector<Pond> ponds;
        t.pondsNear(0, 0, 2500, ponds);
        CHECK(ponds.size() >= 3);
        for (const Pond& p : ponds) {
            CHECK(t.height(p.cx, p.cz) <= p.level - 0.5 * p.depth);       // a real bed under the water
            CHECK(t.waterLevelAt(p.cx, p.cz, t.height(p.cx, p.cz)) == p.level);
            double lowestShore = 1e30;
            for (int i = 0; i < 48; ++i) {
                const double a = i * (2.0 * kPi / 48.0);
                for (double rn : {1.02, 1.1, 1.2, 1.28}) {
                    const double r = p.shoreRadius(a) * rn;
                    lowestShore = std::fmin(lowestShore, t.height(p.cx + std::cos(a) * r, p.cz + std::sin(a) * r));
                }
            }
            CHECK(lowestShore >= p.level - 0.02);  // the bank never lets the water out
        }
    }
}

TEST(ground_materials_add_up) {
    Terrain t(4);
    int rocky = 0;
    for (int i = 0; i < 300; ++i) {
        const TerrainSample s = t.sample(i * 41.0 - 6000, i * 17.0 + 900);
        const GroundMaterials& m = s.materials;
        CHECK(std::fabs(m.grass + m.dirt + m.rock + m.sand - 1.0f) < 1e-4f);
        CHECK(m.meadow >= 0 && m.meadow <= m.grass + 1e-6f);
        if (s.normal.y < std::cos(48.0 * kDegToRad)) {
            CHECK(m.rock > 0.9f);
            ++rocky;
        }
    }
    (void)rocky;
}
