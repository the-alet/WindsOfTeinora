#include <algorithm>
#include <cmath>
#include <vector>

#include "taelia/shared_constants.h"
#include "taelia/sim/clouds.h"
#include "taelia/sim/magic.h"
#include "taelia/sim/terrain.h"
#include "taelia/sim/world.h"
#include "test.h"

using namespace taelia;

TEST(magic_thresholds_come_from_the_motes) {
    const double th = magic::theta0();
    for (const Aspect a : {Aspect::Earth, Aspect::Water, Aspect::Nature, Aspect::Fire, Aspect::Air, Aspect::Light}) {
        const double rho = magic::showThreshold(a);
        CHECK_NEAR(magic::numberDensity(a, rho) * magic::excludedVolume(a), 1.0 / 3.0, 1e-6);  // condensation
    }
    // Inertia and pace: lighter motes move faster and even out sooner (light, air, fire, nature, water, earth).
    const Aspect order[6] = {Aspect::Light, Aspect::Air, Aspect::Fire, Aspect::Nature, Aspect::Water, Aspect::Earth};
    const double nTot = 1.0 / 4e-6;
    for (int i = 0; i + 1 < 6; ++i) {
        CHECK(magic::moteSpeed(order[i], th) > magic::moteSpeed(order[i + 1], th));
        CHECK(magic::diffusivity(order[i], nTot, th) > magic::diffusivity(order[i + 1], nTot, th));
        CHECK(magic::inertiaTau(order[i]) < magic::inertiaTau(order[i + 1]));
    }
    // Under pressure: fire turns to lightning past 675, to light past 1100; 675 of lightning stays lightning.
    CHECK(magic::fireConversion(674.0, th) == magic::Conversion::None);
    CHECK(magic::fireConversion(676.0, th) == magic::Conversion::ToLightning);
    CHECK(magic::fireConversion(1101.0, th) == magic::Conversion::ToLight);
    CHECK(magic::lightningConversion(675.0, th) == magic::Conversion::None);
    CHECK(magic::lightningConversion(830.0, th) == magic::Conversion::ToLight);
    // Graham: a light rune vents 32 times faster than an earth one.
    CHECK_NEAR(magic::effusionFlux(Aspect::Light, 100.0, th) / magic::effusionFlux(Aspect::Earth, 100.0, th), 32.0, 1e-9);
    CHECK(magic::effectFactor(49.9) == 0.0);
    CHECK(magic::effectFactor(51.0) > 0.0);
    CHECK(!magic::visible(29.9));
    CHECK(magic::visible(30.1));
}

TEST(the_baseline_holds_earth_low_water_by_ponds_and_free_magic_everywhere) {
    const MagicBaseline flat(nullptr, 0.0);
    const MagicDensities ground = flat.at({0, 0.1, 0}), up = flat.at({0, 4, 0}), high = flat.at({0, 300, 0});
    CHECK(ground[Aspect::Earth] > 1.8);
    CHECK(ground[Aspect::Earth] < 3.0);
    CHECK(up[Aspect::Earth] < 0.5);
    CHECK_NEAR(high[Aspect::Air], 0.5, 0.05);
    CHECK_NEAR(high[Aspect::Neutral], 0.5, 0.05);
    CHECK(flat.at({0, 2500, 0}).total() < 0.05);  // nothing past about 2 km

    const Terrain terrain(1);
    const MagicBaseline land(&terrain);
    std::vector<Pond> ponds;
    terrain.pondsNear(0, 0, 4000, ponds);
    const Pond* pond = nullptr;
    for (const Pond& p : ponds)
        if (p.depth > 0.6) {
            pond = &p;
            break;
        }
    CHECK(pond != nullptr);
    if (pond) {
        CHECK(land.at({pond->cx, pond->level + 0.2, pond->cz})[Aspect::Water] > 1.5);
        CHECK(land.at({pond->cx, pond->level - 0.3, pond->cz})[Aspect::Water] > 2.0);  // in the water
    }
    // Some meadow near the spawn holds a little nature magic low down; never much.
    double nature = 0;
    for (int i = 0; i < 400; ++i) {
        const double x = (i % 20) * 9.0 - 90.0, z = (i / 20) * 9.0 - 90.0;
        nature = std::max(nature, double(land.at({x, terrain.height(x, z) + 0.3, z})[Aspect::Nature]));
    }
    CHECK(nature > 0.05);
    CHECK(nature < 1.0);
}

namespace {

MagicOptions box(int threads) {
    MagicOptions o;
    o.threads = threads;
    o.levels = 1;
    o.columns = 16;
    o.layers[0] = 8;
    o.closed = true;
    o.takingRule = false;
    o.flatGround = true;
    return o;
}

}  // namespace

TEST(a_closed_box_keeps_its_magic_and_light_evens_out_before_earth) {
    MagicField f(nullptr, nullptr, box(4));
    f.setFocus({0, 1, 0});
    f.step(2);  // starts the field
    const dvec3 mid{1.0, 7.0, 1.0};
    CHECK(f.addDensity(mid, Aspect::Earth, 20.0));
    CHECK(f.addDensity(mid, Aspect::Light, 20.0));
    double before[kAspects];
    for (int a = 0; a < kAspects; ++a) before[a] = f.totalMass(Aspect(a));
    const double earth0 = f.sample(mid).rho[Aspect::Earth], light0 = f.sample(mid).rho[Aspect::Light];
    for (int64_t s = 3; s < 3 + 2000; ++s) f.step(s);
    for (int a = 0; a < kAspects; ++a) CHECK_NEAR(f.totalMass(Aspect(a)), before[a], 2e-4 * std::max(before[a], 1.0));
    CHECK(f.minDensity() >= 0.0);
    const MagicSample after = f.sample(mid);
    CHECK(after.rho[Aspect::Light] / light0 < after.rho[Aspect::Earth] / earth0);
}

TEST(the_field_does_not_depend_on_the_thread_count_or_the_time_speed) {
    const auto run = [](int threads) {
        MagicField f(nullptr, nullptr, box(threads));
        f.setFocus({0, 1, 0});
        f.step(2);
        f.addDensity({1, 5, 3}, Aspect::Fire, 30.0);
        f.addDensity({-5, 3, -1}, Aspect::Water, 12.0);
        for (int64_t s = 3; s < 400; ++s) f.step(s);
        return f.stateHash();
    };
    const uint64_t one = run(1);
    CHECK(one == run(3));
    CHECK(one == run(8));

    WorldDesc d;
    d.seed = 5;
    d.magic.threads = 3;
    World a(d), b(d);
    const double g = a.groundHeight(0, 0);
    a.setFocus({0, g + 2, 0});
    b.setFocus({0, g + 2, 0});
    for (int i = 0; i < 300; ++i) a.step();
    b.clock().setTimeSpeed(50.0);
    CHECK(b.advance(0.1) == 300);
    CHECK(a.stateHash() == b.stateHash());
}

TEST(an_undisturbed_world_stays_at_its_baseline_and_skips_quiet_bricks) {
    WorldDesc d;
    World w(d);
    const double g = w.groundHeight(0, 0);
    w.setFocus({0, g + 2, 0});
    for (int i = 0; i < 360; ++i) w.step();
    const MagicSample s = w.magicAt({0, g + 0.5, 0});
    CHECK(s.level == 0);
    CHECK(s.rho[Aspect::Earth] > 0.5);  // a 2 m cell averages a profile that falls off over 1.5 m
    CHECK(s.rho[Aspect::Earth] < 3.0);
    CHECK_NEAR(s.rho[Aspect::Air] + s.rho[Aspect::Neutral], 1.0, 0.2);
    CHECK(s.rho.total() < k::MAGIC_DEW_POINT);  // nothing shows in an ordinary place
    CHECK(w.magic().minDensity() >= 0.0);
    CHECK(w.magic().stats().bricksSkipped > 0);
    const MagicSample high = w.magicAt({0, g + 900, 0});
    CHECK(high.level == 2);
    CHECK_NEAR(high.rho.total(), 1.0, 0.2);
}

TEST(a_campfire_holds_a_hot_core_sends_fire_up_and_burns_on) {
    WorldDesc d;
    World w(d);
    const dvec3 core = w.campfireCore();
    w.setFocus(core + dvec3{6, 1.5, 0});
    for (int i = 0; i < 20 * 60; ++i) w.step();
    const MagicSample at = w.magicAt(core);
    CHECK(at.level == 0);
    CHECK(at.rho[Aspect::Fire] > 4.0);                                // a dense core (2 m cells' share of it)
    CHECK(at.theta > 1.25 * magic::theta0());                         // born hot
    CHECK(at.airT > 5.0);                                             // the fire heats the air
    const double up = w.magicAt(core + dvec3{0, 5, 0}).rho[Aspect::Fire];
    const double near = w.magicAt(core + dvec3{2, 0, 0}).rho[Aspect::Fire];
    const double side = w.magicAt(core + dvec3{5, 0, 0}).rho[Aspect::Fire];
    const double far = w.magicAt(core + dvec3{14, 0, 0}).rho[Aspect::Fire];
    CHECK(up > side);                                                 // it rises
    CHECK(at.rho[Aspect::Fire] > near);                               // thinner farther out
    CHECK(near > side);
    CHECK(side >= far);
    CHECK(at.airT < 150.0);                                           // the air it heats rises away: no runaway
    CHECK(far < 0.5);
    CHECK(w.campfireStrength() >= 1.0);                              // fed by the wind: fire outweighs air at the bed
    CHECK(w.magic().minDensity() >= 0.0);
}

TEST(light_drops_fall_by_day_and_turn_back_to_neutral) {
    MagicOptions o = box(2);
    o.closed = false;
    o.takingRule = true;  // light turns back to neutral by the taking rule
    MagicField f(nullptr, nullptr, o);
    f.setFocus({0, 1, 0});
    f.setDrops({DropSource{{0.2, 1.0, 0.1}, 4000.0}});
    for (int64_t s = 1; s <= 120; ++s) f.step(s);
    const double lit = f.totalMass(Aspect::Light);
    CHECK(lit > 0.0);       // drops stopped in the magic and left light
    f.setDrops({});         // night without the planet: no more
    for (int64_t s = 121; s <= 120 + 60 * 30; ++s) f.step(s);
    CHECK(f.totalMass(Aspect::Light) < 0.25 * lit);  // it turned back to neutral (faint traces stay in quiet bricks)
    CHECK(f.minDensity() >= 0.0);
}

TEST(nature_drinks_water_magic) {
    MagicField f(nullptr, nullptr, box(2));
    f.setFocus({0, 1, 0});
    f.step(2);
    const dvec3 at{1, 3, 1};
    f.addDensity(at, Aspect::Nature, 6.0);
    f.addDensity(at, Aspect::Water, 6.0);
    const double water0 = f.totalMass(Aspect::Water), nature0 = f.totalMass(Aspect::Nature);
    double all0 = 0, all1 = 0;
    for (int a = 0; a < kAspects; ++a) all0 += f.totalMass(Aspect(a));
    for (int64_t s = 3; s < 1200; ++s) f.step(s);
    for (int a = 0; a < kAspects; ++a) all1 += f.totalMass(Aspect(a));
    CHECK(f.totalMass(Aspect::Water) < water0);
    CHECK(f.totalMass(Aspect::Nature) > nature0);
    CHECK_NEAR(all1, all0, 1e-3 * all0);
}

TEST(things_hold_their_magic_in_ordinary_places_and_fall_apart_in_a_void) {
    double held = k::MAGIC_HELD;
    for (int i = 0; i < 60 * 60; ++i) held = magic::heldAfter(held, 1.2, 1.0);  // an hour in ordinary air
    CHECK(!magic::fallsApart(held));
    CHECK_NEAR(held, 1.2, 0.05);
    double lost = k::MAGIC_HELD;
    int seconds = 0;
    while (!magic::fallsApart(lost) && seconds < 100000) lost = magic::heldAfter(lost, 0.0, 1.0), ++seconds;
    CHECK(seconds > 600);    // slowly...
    CHECK(seconds < 3600);   // ...but it goes

    WorldDesc d;
    World w(d);
    const double g = w.groundHeight(2, 0);
    w.setFocus({0, g + 2, 0});
    const uint32_t id = w.throwBall({2, g + 0.5, 0}, {0, 0, 0});
    for (int i = 0; i < 60 * 60; ++i) w.step();
    CHECK(w.balls().size() == 1u);       // still whole a minute later
    CHECK(w.ballHeld(id) > 0.5);
}

TEST(fireflies_count_as_the_spec_says_and_ten_merge) {
    CHECK(magic::fireflyCount(39.9) == 0);
    CHECK(magic::fireflyCount(40.0) == 3);
    CHECK(magic::fireflyCount(43.0) == 4);
    CHECK(magic::fireflyCount(49.0) == 6);
    int big = 0, small = 0;
    magic::fireflyGroups(40.0 + 3.0 * 9.0, big, small);  // 12 fireflies: one big, two small
    CHECK(big == 1);
    CHECK(small == 2);
}

namespace {

MagicOptions open(int threads) {
    MagicOptions o = box(threads);
    o.closed = false;
    return o;
}

}  // namespace

TEST(dense_magic_condenses_and_shows_then_evaporates) {
    MagicField f(nullptr, nullptr, open(2));
    f.setFocus({0, 1, 0});
    f.step(2);
    const dvec3 at{1, 5, 1};
    f.addDensity(at, Aspect::Light, 60.0);
    f.step(4);
    CHECK(!f.fireflies().empty());              // past 40: fireflies
    for (int64_t s = 5; s < 60 * 60; ++s) f.step(s);
    CHECK(f.fireflies().empty());               // it thinned out: they went

    f.addDensity(at, Aspect::Earth, 90.0);
    for (int64_t s = 3600; s < 3600 + 120; ++s) f.step(s);
    CHECK(!f.manifestations().empty());         // a mineral grew ...
    CHECK(f.manifestations()[0].kind == ManifestKind::Mineral);
    for (int64_t s = 3720; s < 3720 + 60 * 60; ++s) f.step(s);
    CHECK(!f.manifestations().empty());         // ... and stays when the magic thins
    CHECK(f.minDensity() >= 0.0);
}

TEST(dense_air_magic_spins_a_whirlwind) {
    MagicField f(nullptr, nullptr, open(2));
    f.setFocus({0, 1, 0});
    f.step(2);
    f.addDensity({1, 5, 1}, Aspect::Air, 70.0);
    for (int64_t s = 3; s < 12; ++s) f.step(s);
    const MagicSample east = f.sample({5, 5, 1}), west = f.sample({-3, 5, 1});
    CHECK(east.u.z * west.u.z < 0.0);           // the air goes round: opposite ways on either side
}

TEST(only_magic_past_50_pushes_a_ball) {
    for (const double rho : {49.0, 120.0}) {
        Physics p(nullptr, nullptr, 1);
        p.setMagicSampler([rho](const dvec3&, double& r, dvec3& u, double& air) {
            r = rho;
            u = {6, 0, 0};
            air = 0.0;
        });
        p.throwBall({0, k::BALL_RADIUS + 0.001, 0}, {0, 0, 0});
        double t = 0;
        for (int i = 0; i < 60; ++i) p.step(1.0 / 60.0, t += 1.0 / 60.0);
        const double x = p.balls()[0].position.x;
        if (rho > 50) CHECK(x > 0.1);
        else CHECK(std::abs(x) < 1e-3);
    }
}

TEST(rain_falls_only_from_a_closed_sky_and_a_fair_day_keeps_its_cover) {
    CloudMap clouds(3);
    clouds.setFocus(0, 0);
    double t = 0;
    int rainy = 0;
    for (int i = 0; i < 4000; ++i) {
        clouds.step(1.0, {3, 0, 1}, t += 1.0);
        if (i % 40) continue;
        for (int j = 0; j < 8; ++j) {
            const double x = (j - 4) * 9000.0 + i, z = (j % 3) * 7000.0;
            const double rain = clouds.rainAt(x, z), water = clouds.waterAt(x, z);
            if (rain > 0.0) {
                ++rainy;
                CHECK(water > k::WEATHER_RAIN);  // rain only past 30: under a closed sky
            }
            if (water < k::WEATHER_STORM) CHECK(clouds.stormAt(x, z) == 0.0);
        }
    }
    // A fair day's cover is the tendency the sky had before there was weather.
    CloudMap fair(3);
    fair.setOverride(0.5);
    fair.setFocus(0, 0);
    fair.step(1.0, {0, 0, 0}, 1.0);
    const double w = fair.waterAt(0, 0);
    CHECK_NEAR((w - 6.0) / 24.0, 0.5, 1e-6);
    CHECK(fair.rainAt(0, 0) == 0.0);
}

TEST(rain_brought_by_the_settings_closes_the_sky_storms_strike_and_drops_carry_water_down) {
    CloudMap clouds(4);
    clouds.setRain(1.0);
    clouds.setFocus(0, 0);
    double t = 0;
    for (int i = 0; i < 600; ++i) clouds.step(1.0, {2, 0, 0}, t += 1.0);
    CHECK(clouds.rainAt(0, 0) > 0.5);
    CHECK(clouds.sample(0, 0).coverage > 0.5);
    size_t strikes = 0;
    for (int i = 0; i < 3600; ++i) {
        clouds.step(1.0, {2, 0, 0}, t += 1.0);
        strikes += clouds.takeStrikes().size();
    }
    CHECK(strikes > 0);  // storms where the rain's clouds meet the fair ones

    WorldDesc d;
    d.settings.rain = 1.0;
    World w(d);
    const double g = w.groundHeight(0, 0);
    w.setFocus({0, g + 2, 0});
    for (int i = 0; i < 90 * 60; ++i) w.step();
    CHECK(w.rainAt({0, g, 0}) > 0.5);
    double water = 0;
    for (int i = 0; i < 64; ++i) {
        const double x = (i % 8) * 14.0 - 56.0, z = (i / 8) * 14.0 - 56.0;
        water = std::max(water, double(w.magicAt({x, w.groundHeight(x, z) + 0.5, z}).rho[Aspect::Water]));
    }
    CHECK(water > 0.05);  // raindrops carried water magic down
}

TEST(fire_past_the_dew_point_makes_steam_over_water_and_sets_plants_burning) {
    WorldDesc d;
    World w(d);
    // The campfire alone (about 20 kg/m3 in its cell) is below the dew point: it burns nothing.
    w.setFocus(w.campfireCore() + dvec3{4, 1.5, 0});
    for (int i = 0; i < 10 * 60; ++i) w.step();
    CHECK(w.burningTrees().empty());
    CHECK(w.scorches().empty());

    // Fire over the spawn pond makes steam.
    std::vector<Pond> ponds;
    w.terrain().pondsNear(0, 0, 400, ponds);
    CHECK(!ponds.empty());
    const Pond& pond = ponds[0];
    w.setFocus({pond.cx, pond.level + 2, pond.cz});
    for (int i = 0; i < 4; ++i) w.step();
    w.magic().addDensity({pond.cx, pond.level + 0.3, pond.cz}, Aspect::Fire, 90.0);
    bool steamed = false;
    for (int i = 0; i < 40 && !steamed; ++i) {
        w.step();
        steamed = !w.steam().empty();
    }
    CHECK(steamed);

    // Fire at a tree's foot: it catches, burns as a hot thing, and stands charred; the grass there burns away.
    std::vector<TreeInstance> trees;
    treesInArea(w.terrain(), -80, -80, 80, 80, trees);
    CHECK(!trees.empty());
    const TreeInstance t = trees[0];
    w.setFocus(t.position + dvec3{5, 2, 0});
    for (int i = 0; i < 4; ++i) w.step();
    w.magic().addDensity(t.position + dvec3{0.5, 0.8, 0.0}, Aspect::Fire, 90.0);
    for (int i = 0; i < 60; ++i) w.step();
    CHECK(!w.burningTrees().empty());
    CHECK(!w.scorches().empty());
    for (int i = 0; i < int(k::PLANT_BURN_SECONDS + 5.0) * 60; ++i) w.step();
    const std::pair<int64_t, int64_t> key{int64_t(std::floor(t.position.x / kTreeCell)), int64_t(std::floor(t.position.z / kTreeCell))};
    CHECK(w.burntTrees().count(key) == 1);
    CHECK(w.magic().minDensity() >= 0.0);
}
