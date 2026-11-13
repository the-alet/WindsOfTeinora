// The sky: eclipse schedule, the order of a day, the planet by day and by night, stars, the cloud map.
#include "taelia/shared_constants.h"
#include "taelia/sim/clouds.h"
#include "taelia/sim/sky.h"
#include "test.h"

using namespace taelia;

namespace {
const double D = Sky::daySeconds();
const double kEclipse = k::DOUBLE_ECLIPSE_DAY * D;
}  // namespace

TEST(each_sun_meets_the_planet_on_its_own_period) {
    for (int n = 0; n < 4; ++n) {
        CHECK_NEAR(Sky::centralSeparation(0, kEclipse + n * k::SUN_A_PERIOD_DAYS * D), 0.0, 1e-9);
        CHECK_NEAR(Sky::centralSeparation(1, kEclipse + n * k::SUN_B_PERIOD_DAYS * D), 0.0, 1e-9);
    }
    // Both together only once every 90 days.
    int together = 0;
    for (int day = 0; day < 180; ++day) {
        const double t = (day + 0.5) * D;
        if (Sky::centralSeparation(0, t) < 0.02 && Sky::centralSeparation(1, t) < 0.02) ++together;
    }
    CHECK(together == 2);  // days 15 and 105
}

TEST(the_double_eclipse_is_seen_at_noon_from_the_spawn) {
    const SkyState s = Sky::at(kEclipse);
    CHECK(s.planetUp && s.sun[0].up && s.sun[1].up);
    CHECK_NEAR(s.sun[0].coverage, 1.0, 1e-9);
    CHECK_NEAR(s.sun[1].coverage, 1.0, 1e-9);
    CHECK(s.planetDir.y > std::sin(30.0 * kDegToRad));  // high in the sky
    // Hours later the suns are out from behind it again.
    const SkyState later = Sky::at(kEclipse + D * 0.35);  // eclipses last hours: the suns slide past slowly
    CHECK(later.sun[0].coverage == 0.0 && later.sun[1].coverage == 0.0);
}

TEST(a_day_first_sun_rises_second_joins_first_sets_second_sets_then_night) {
    // Day 0 at the spawn: sample each minute and note the order of events.
    int riseA = -1, riseB = -1, setA = -1, setB = -1, nightMinutes = 0, prevA = -1, prevB = -1;
    const int minutes = int(D / 60.0);
    for (int m = 0; m <= minutes; ++m) {
        const SkyState s = Sky::at(m * 60.0);
        const int a = s.sun[0].up, b = s.sun[1].up;
        if (prevA == 0 && a == 1 && riseA < 0) riseA = m;
        if (prevB == 0 && b == 1 && riseB < 0) riseB = m;
        if (prevA == 1 && a == 0 && riseA >= 0 && setA < 0) setA = m;
        if (prevB == 1 && b == 0 && riseB >= 0 && setB < 0) setB = m;
        nightMinutes += (!a && !b);
        prevA = a;
        prevB = b;
    }
    CHECK(riseA >= 0 && riseB >= 0 && setA >= 0 && setB >= 0);
    CHECK(riseA < riseB && riseB < setA && setA < setB);  // first rises, second joins, first sets, then the second
    CHECK(nightMinutes > minutes / 4);                    // then night
    // The yellow-orange sun is highest at noon (hour HOURS_PER_DAY / 2).
    double bestY = -2;
    int bestM = 0;
    for (int m = 0; m <= minutes; m += 5) {
        const double y = Sky::at(m * 60.0).sun[0].dir.y;
        if (y > bestY) bestY = y, bestM = m;
    }
    CHECK_NEAR(bestM * 60.0 / D, 0.5, 0.01);
}

TEST(the_planet_is_seen_by_day_and_by_night) {
    int byDay = 0, byNight = 0;
    for (int day = 0; day < 90; ++day)
        for (int h = 0; h < k::HOURS_PER_DAY; ++h) {
            const SkyState s = Sky::at(day * D + h * D / k::HOURS_PER_DAY);
            if (!s.planetUp) continue;
            const bool sunUp = s.sun[0].up || s.sun[1].up;
            byDay += sunUp;
            byNight += !sunUp;
        }
    CHECK(byDay > 50 && byNight > 50);
    // Its rings open and close over the days.
    double lo = 1, hi = -1;
    for (int day = 0; day < 70; ++day) {
        const double r = Sky::at(day * D).ringOpening;
        lo = std::fmin(lo, r);
        hi = std::fmax(hi, r);
    }
    CHECK(lo < -0.3 && hi > 0.3);
}

TEST(stars_are_generated_not_real) {
    const auto a = generateStars(7, k::STAR_COUNT), b = generateStars(7, k::STAR_COUNT), c = generateStars(8, k::STAR_COUNT);
    CHECK(int(a.size()) == k::STAR_COUNT);
    CHECK(a[100].dir[0] == b[100].dir[0] && a[100].magnitude == b[100].magnitude);
    CHECK(a[100].dir[0] != c[100].dir[0]);
    int bright = 0, faint = 0;
    for (const Star& s : a) {
        const double l = std::sqrt(double(s.dir[0]) * s.dir[0] + double(s.dir[1]) * s.dir[1] + double(s.dir[2]) * s.dir[2]);
        CHECK(std::fabs(l - 1.0) < 1e-4);
        bright += s.magnitude < 1.5;
        faint += s.magnitude > 5.0;
    }
    CHECK(bright > 3 && bright < 80);  // few bright, many faint
    CHECK(faint > k::STAR_COUNT / 2);
}

TEST(clouds_follow_the_wind_and_the_weather) {
    CloudMap m(3);
    const dvec3 wind{5, 0, 0};
    const CloudSample before = m.sample(1000, 0);
    for (int i = 0; i < 600; ++i) m.step(1.0 / 60.0, wind, 0.0);  // 10 s at 5 m/s: carried 50 m east
    CHECK_NEAR(m.sample(1050, 0).coverage, before.coverage, 1e-3);  // the cloud water's grid interpolates the cover
    m.setOverride(0.0);
    int any = 0;
    for (int i = 0; i < 200; ++i) any += m.sample(i * 337.0, i * 113.0).coverage > 0.01;
    CHECK(any == 0);  // a clear sky
    m.setOverride(1.0);
    int all = 0;
    for (int i = 0; i < 200; ++i) all += m.sample(i * 337.0, i * 113.0).coverage > 0.9;
    CHECK(all > 190);  // closed
}
