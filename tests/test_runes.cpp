// Runes (step 7): closed vessels of magic. The tests SPEC.md asks for: below the seal the field round it stays the
// same; it draws in only its aspect (or neutral) past the dew point; the leak grows with the excess (and is faster for
// lighter motes); a break releases everything; a fire rune past 675 stays fire inside and comes out as lightning.
#include "taelia/shared_constants.h"
#include "taelia/sim/rune.h"
#include "taelia/sim/world.h"
#include "test.h"

using namespace taelia;

namespace {

// A spot in the open near the spawn point, half a metre over the ground, the field running round it.
dvec3 openSpot(World& w) {
    const dvec3 p{8.0, 0.0, -6.0};
    w.setFocus({p.x, w.groundHeight(p.x, p.z) + 1.5, p.z});
    for (int i = 0; i < 8; ++i) w.step();
    return {p.x, w.groundHeight(p.x, p.z) + 0.5, p.z};
}

Rune runeAt(const dvec3& p, Aspect a, RuneMaterial m, double density) {
    Rune r;
    r.aspect = a;
    r.material = m;
    r.position = p;
    r.normal = {0, 0, 1};
    r.size = 0.5;
    r.contents[int(a)] = density * r.volume();
    return r;
}

double fieldMass(const World& w) {
    double m = 0;
    for (int a = 0; a < kAspects; ++a) m += w.magic().totalMass(Aspect(a));
    return m;
}

}  // namespace

TEST(rune_is_half_a_metre_and_its_seal_is_by_material) {
    Rune r;
    r.size = 0.5;
    CHECK_NEAR(r.volume(), 0.1, 1e-6);
    r.material = RuneMaterial::Wood;
    CHECK_NEAR(r.seal(), 400.0, 1e-9);
    r.material = RuneMaterial::Stone;
    CHECK_NEAR(r.seal(), 600.0, 1e-9);
    r.material = RuneMaterial::Metal;
    CHECK_NEAR(r.seal(), 900.0, 1e-9);
    CHECK_NEAR(r.breakPoint(), 1350.0, 1e-9);
}

TEST(rune_below_its_seal_leaves_the_field_alone) {
    WorldDesc d;
    World plain(d), withRune(d);
    const dvec3 p = openSpot(plain);
    openSpot(withRune);
    withRune.addRune(runeAt(p, Aspect::Fire, RuneMaterial::Stone, 400.0));  // dense, but under its seal
    for (int i = 0; i < 120; ++i) {
        plain.step();
        withRune.step();
    }
    CHECK(plain.magic().stateHash() == withRune.magic().stateHash());  // to the field it does not exist
    CHECK(withRune.runes().size() == 1);
    CHECK_NEAR(withRune.runes().front().density(), 400.0, 1e-9);  // nothing faded, nothing changed inside
}

TEST(rune_draws_in_only_its_aspect_or_neutral_past_the_dew_point) {
    WorldDesc d;
    {
        World w(d);
        const dvec3 p = openSpot(w);
        w.addRune(runeAt(p, Aspect::Fire, RuneMaterial::Stone, 0.0));
        w.magic().addDensity(p, Aspect::Water, 90.0);  // another aspect passes by
        w.magic().addDensity(p, Aspect::Fire, 20.0);   // its own, but under the dew point
        for (int i = 0; i < 2; ++i) w.step();
        CHECK(w.runes().front().mass() == 0.0);
        w.magic().addDensity(p, Aspect::Fire, 90.0);   // its own past the dew point
        for (int i = 0; i < 2; ++i) w.step();
        CHECK(w.runes().front().contents[int(Aspect::Fire)] > 0.0);
        CHECK(w.runes().front().contents[int(Aspect::Water)] == 0.0);
    }
    {
        World w(d);
        const dvec3 p = openSpot(w);
        w.addRune(runeAt(p, Aspect::Fire, RuneMaterial::Stone, 0.0));
        w.magic().addDensity(p, Aspect::Neutral, 90.0);  // neutral takes its aspect
        for (int i = 0; i < 2; ++i) w.step();
        CHECK(w.runes().front().contents[int(Aspect::Fire)] > 0.0);
        CHECK(w.runes().front().contents[int(Aspect::Neutral)] == 0.0);
    }
}

TEST(rune_leaks_more_the_further_over_its_seal) {
    WorldDesc d;
    World w(d);
    const dvec3 p = openSpot(w);
    const double dt = 1.0 / 60.0;
    auto leakOf = [&](Aspect a, double density) {
        Rune r = runeAt(p, a, RuneMaterial::Stone, density);
        const double before = r.mass(), fieldBefore = fieldMass(w);
        stepRune(r, w.magic(), dt);
        const double out = before - r.mass();
        CHECK_NEAR(fieldMass(w) - fieldBefore, out, 1e-2 * out + 1e-6);  // what leaves it is in the field (float cells)
        return out;
    };
    const double little = leakOf(Aspect::Fire, 660.0), more = leakOf(Aspect::Fire, 780.0);
    CHECK(little > 0.0 && more > 1.5 * little);
    CHECK(leakOf(Aspect::Fire, 590.0) == 0.0);  // under the seal nothing leaks
    // The lighter the motes, the sooner it vents (Graham).
    CHECK(leakOf(Aspect::Light, 700.0) > leakOf(Aspect::Earth, 700.0));
}

TEST(rune_breaks_past_its_break_point_releasing_everything) {
    WorldDesc d;
    World w(d);
    const dvec3 p = openSpot(w);
    Rune r = runeAt(p, Aspect::Earth, RuneMaterial::Wood, 650.0);  // past 1.5 x 400
    const double held = r.mass(), before = fieldMass(w);
    stepRune(r, w.magic(), 1.0 / 60.0);
    CHECK(r.broken);
    CHECK(r.mass() == 0.0);
    CHECK_NEAR(fieldMass(w) - before, held, 1e-3 * held);
    // Damaging its sign does the same.
    const uint32_t id = w.addRune(runeAt(p, Aspect::Earth, RuneMaterial::Stone, 300.0));
    const double before2 = fieldMass(w);
    w.damageRune(id);
    CHECK(w.runes().empty());
    CHECK_NEAR(fieldMass(w) - before2, 300.0 * 0.1, 0.03);
}

TEST(fire_rune_past_675_stays_fire_inside_and_comes_out_as_lightning) {
    WorldDesc d;
    World w(d);
    const dvec3 p = openSpot(w);
    const uint32_t id = w.addRune(runeAt(p, Aspect::Fire, RuneMaterial::Metal, 700.0));
    for (int i = 0; i < 60; ++i) w.step();
    CHECK_NEAR(w.runes().front().contents[int(Aspect::Fire)], 70.0, 1e-5);  // no change under pressure inside
    CHECK(w.runes().front().contents[int(Aspect::Lightning)] == 0.0);
    const double fire = w.magic().totalMass(Aspect::Fire), lightning = w.magic().totalMass(Aspect::Lightning);
    w.damageRune(id);
    CHECK_NEAR(w.magic().totalMass(Aspect::Lightning) - lightning, 70.0, 0.1);
    CHECK_NEAR(w.magic().totalMass(Aspect::Fire), fire, 0.1);
    CHECK(runeOutflowAspect(Aspect::Fire, 600.0) == Aspect::Fire);
    CHECK(runeOutflowAspect(Aspect::Fire, 1200.0) == Aspect::Light);
}
