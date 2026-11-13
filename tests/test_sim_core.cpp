// Clock, time units, determinism, hashes at large coordinates, shared constants.
#include "taelia/shared_constants.h"
#include "taelia/sim/clock.h"
#include "taelia/sim/hash.h"
#include "taelia/sim/world.h"
#include "test.h"

using namespace taelia;

TEST(time_units_are_invented_whole_numbers) {
    CHECK(k::HOURS_PER_DAY <= 30);
    CHECK(k::MINUTES_PER_HOUR > 40);
    CHECK(k::SECONDS_PER_MINUTE > 50);
    CHECK(k::HOURS_PER_DAY != 24 || k::MINUTES_PER_HOUR != 60 || k::SECONDS_PER_MINUTE != 60);
}

TEST(clock_starts_at_the_asked_hour) {
    WorldClock c(13.25);
    CHECK_NEAR(c.hourOfDay(), 13.25, 1e-6);
    CalendarTime t = c.calendar();
    CHECK(t.day == 0 && t.hour == 13);
    CHECK(t.minute == k::MINUTES_PER_HOUR / 4);
    WorldClock wrap(double(k::HOURS_PER_DAY) + 2.0);
    CHECK_NEAR(wrap.hourOfDay(), 2.0, 1e-6);
}

TEST(time_speed_changes_step_count_not_step_size) {
    WorldClock a(0), b(0);
    b.setTimeSpeed(50);
    int na = 0, nb = 0;
    for (int f = 0; f < 120; ++f) {
        na += a.advance(1.0 / 60);
        nb += b.advance(1.0 / 60);
    }
    CHECK(na == 2 * k::SIM_STEP_HZ);
    CHECK(nb == 50 * na);
    WorldClock slow(0);
    slow.setTimeSpeed(1.0 / 50);
    int ns = 0;
    for (int f = 0; f < 50 * 60; ++f) ns += slow.advance(1.0 / 60);
    CHECK(ns == k::SIM_STEP_HZ);  // 50 real seconds at x1/50 = one simulated second
}

TEST(time_speed_ladder_spans_x1_50_to_x50) {
    WorldClock c(0);
    for (int i = 0; i < 20; ++i) c.faster();
    CHECK_NEAR(c.timeSpeed(), 50.0, 1e-9);
    for (int i = 0; i < 40; ++i) c.slower();
    CHECK_NEAR(c.timeSpeed(), 1.0 / 50, 1e-12);
    c.setTimeSpeed(1000);
    CHECK_NEAR(c.timeSpeed(), k::TIME_SPEED_MAX, 1e-9);
}

TEST(jump_to_hour_moves_forward) {
    WorldClock c(10.0);
    c.jumpToHour(4.0);  // earlier hour: the next day
    CHECK_NEAR(c.hourOfDay(), 4.0, 1e-6);
    CHECK(c.calendar().day == 1);
    c.jumpToHour(20.0);  // later the same day
    CHECK_NEAR(c.hourOfDay(), 20.0, 1e-6);
    CHECK(c.calendar().day == 1);
}

TEST(clock_survives_a_stall) {
    WorldClock c(0);
    c.setTimeSpeed(50);
    CHECK(c.advance(3600.0) == WorldClock::kMaxStepsPerAdvance);
    CHECK(c.advance(0.0) == 0);  // the backlog was dropped, not carried
}

TEST(world_is_deterministic_from_flags) {
    WorldDesc d;
    d.seed = 77;
    d.startHour = 6.0;
    World a(d), b(d);
    for (int f = 0; f < 300; ++f) {
        a.advance(1.0 / 60);
        b.advance(1.0 / 60);
    }
    CHECK(a.stateHash() == b.stateHash());
    d.seed = 78;
    World c(d);
    for (int f = 0; f < 300; ++f) c.advance(1.0 / 60);
    CHECK(c.stateHash() != a.stateHash());
}

TEST(lattice_noise_is_smooth_far_away) {
    // At 10,000 km a sin()-hash tears; integer mixing must stay as smooth as at the origin.
    const double cell = 50.0;
    for (double base : {0.0, 1.0e7, -3.3e7}) {
        double maxStep = 0, prev = latticeNoise2(base, base * 0.5, cell, 9);
        for (int i = 1; i <= 2000; ++i) {
            const double v = latticeNoise2(base + i * 0.05, base * 0.5, cell, 9);
            maxStep = std::fmax(maxStep, std::fabs(v - prev));
            prev = v;
        }
        CHECK(maxStep < 0.01);  // 5 cm steps on a 50 m lattice never jump
    }
    CHECK(latticeNoise2(1.0e7 + 13.7, 5.0, cell, 9) != latticeNoise2(1.0e7 + 63.7, 5.0, cell, 9));
}

TEST(hash_spreads_neighbouring_cells) {
    int same = 0;
    for (int i = 0; i < 1000; ++i)
        same += (hashLattice(i, 0, 0, 1) >> 60) == (hashLattice(i + 1, 0, 0, 1) >> 60);
    CHECK(same < 120);  // ~1/16 expected
    const double u = hashToUnit(~0ull);
    CHECK(u < 1.0 && u > 0.99);
}

TEST(frame_constants_budget_is_8kb) {
    CHECK(k::FRAME_CONSTANTS_BYTES == 8192);
    CHECK(k::PUSH_CONSTANTS_BYTES <= 128);
}

TEST(wind_is_gentle_by_default) {
    World w(WorldDesc{});
    CHECK(length(w.wind()) > 0.5 && length(w.wind()) < 5.0);
}
