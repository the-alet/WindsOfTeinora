#include "taelia/sim/sky.h"

#include <algorithm>
#include <cmath>

#include "taelia/shared_constants.h"
#include "taelia/sim/hash.h"

namespace taelia {

namespace {

// Inertial frame: Z = the world's axis, X = toward the planet, which sits where both suns' orbits cross.
double day() { return double(k::HOURS_PER_DAY) * k::MINUTES_PER_HOUR * k::SECONDS_PER_MINUTE; }
double doubleEclipse() { return k::DOUBLE_ECLIPSE_DAY * day(); }

dvec3 sunInertial(int i, double t) {
    const double period = (i == 0 ? k::SUN_A_PERIOD_DAYS : k::SUN_B_PERIOD_DAYS) * day();
    const double tilt = (i == 0 ? 1.0 : -1.0) * k::SUN_TILT_DEG * kDegToRad;  // opposite ways
    const double th = 2.0 * kPi * (t - doubleEclipse()) / period;  // both at the planet together every 90 days
    return dvec3{std::cos(th), std::sin(th) * std::cos(tilt), std::sin(th) * std::sin(tilt)} * k::SKY_SPHERE_KM;
}

dvec3 planetInertial() { return dvec3{k::SKY_SPHERE_KM, 0, 0}; }

// The world turns so the yellow-orange sun's day is exactly one calendar day; at the double eclipse
// (noon of that day) the spawn point faces the planet.
double longitude(double t) {
    const double rate = 2.0 * kPi / day() + 2.0 * kPi / (k::SUN_A_PERIOD_DAYS * day());
    return rate * (t - doubleEclipse());
}

struct Observer {
    dvec3 pos, east, up, north;
    dvec3 local(const dvec3& v) const { return {dot(v, east), dot(v, up), dot(v, north)}; }
};

Observer observer(double t) {
    const double lat = k::OBSERVER_LATITUDE_DEG * kDegToRad, lon = longitude(t);
    Observer o;
    o.up = {std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
    o.pos = o.up * k::WORLD_RADIUS_KM;
    o.east = {-std::sin(lon), std::cos(lon), 0};
    o.north = cross(o.up, o.east);
    return o;
}

dvec3 blackbody(double kelvin) {  // approximate colour of a black body, linear, luminance 1
    const double t = kelvin / 100.0;
    double r, g, b;
    if (t <= 66.0) {
        r = 255.0;
        g = 99.4708025861 * std::log(t) - 161.1195681661;
        b = t <= 19.0 ? 0.0 : 138.5177312231 * std::log(t - 10.0) - 305.0447927307;
    } else {
        r = 329.698727446 * std::pow(t - 60.0, -0.1332047592);
        g = 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
        b = 255.0;
    }
    auto lin = [](double c) { return std::pow(std::clamp(c, 0.0, 255.0) / 255.0, 2.2); };
    dvec3 c{lin(r), lin(g), lin(b)};
    const double lum = 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z;
    return c * (1.0 / std::max(lum, 1e-6));
}

}  // namespace

double Sky::daySeconds() { return day(); }

double Sky::discCoverage(double d, double rs, double rp) {
    if (d >= rs + rp) return 0.0;
    if (d <= rp - rs) return 1.0;                    // the sun entirely behind the planet
    if (d <= rs - rp) return (rp * rp) / (rs * rs);  // the planet entirely inside the sun's disc
    const double a = std::acos(std::clamp((d * d + rs * rs - rp * rp) / (2 * d * rs), -1.0, 1.0));
    const double b = std::acos(std::clamp((d * d + rp * rp - rs * rs) / (2 * d * rp), -1.0, 1.0));
    const double overlap = rs * rs * (a - std::sin(2 * a) * 0.5) + rp * rp * (b - std::sin(2 * b) * 0.5);
    return std::clamp(overlap / (kPi * rs * rs), 0.0, 1.0);
}

double Sky::centralSeparation(int sun, double seconds) {
    const dvec3 s = normalize(sunInertial(sun, seconds)), p = normalize(planetInertial());
    return std::acos(std::clamp(dot(s, p), -1.0, 1.0));
}

SkyState Sky::at(double t) {
    SkyState st;
    const Observer o = observer(t);
    const dvec3 planet = planetInertial();
    const dvec3 toPlanet = normalize(planet - o.pos);
    st.planetDir = o.local(toPlanet);
    st.planetUp = st.planetDir.y > 0;
    const double rp = double(k::PLANET_RADIUS_DEG) * kDegToRad;
    for (int i = 0; i < 2; ++i) {
        const dvec3 s = sunInertial(i, t);
        const dvec3 toSun = normalize(s - o.pos);
        st.sun[i].dir = o.local(toSun);
        st.sun[i].up = st.sun[i].dir.y > 0;
        // The planet is drawn in front of the suns: wherever their discs overlap, it covers them.
        const double rs = double(i == 0 ? k::SUN_A_RADIUS_DEG : k::SUN_B_RADIUS_DEG) * kDegToRad;
        const double d = std::acos(std::clamp(dot(toSun, toPlanet), -1.0, 1.0));
        st.sun[i].coverage = discCoverage(d, rs, rp);
        // It hangs in front of the suns (it eclipses them), so it is lit as a near body is: from where we see
        // each sun. At an eclipse its face toward us is dark; with the sun behind us it is full.
        st.planetLitFrom[i] = st.sun[i].dir;
    }
    // The planet turns slowly; its axis precesses, so the tilted rings open and close over the days.
    const double precession = 2.0 * kPi * t / (k::PLANET_PRECESSION_DAYS * day());
    st.planetSpin = std::fmod(2.0 * kPi * t / (k::PLANET_SPIN_DAYS * day()), 2.0 * kPi);
    st.ringOpening = double(k::PLANET_RING_TILT_DEG) * kDegToRad * std::sin(precession);
    st.axisAngle = 0.35 + 0.25 * std::cos(precession);
    const dvec3 rows[3] = {o.east, o.up, o.north};
    for (int r = 0; r < 3; ++r) {
        st.starRotation[r * 3 + 0] = rows[r].x;
        st.starRotation[r * 3 + 1] = rows[r].y;
        st.starRotation[r * 3 + 2] = rows[r].z;
    }
    st.worldNorth = o.local({0, 0, 1});
    return st;
}

dvec3 galacticPole(uint64_t seed) {
    const uint64_t h = mix64(seed ^ 0x6a1a);
    const double z = hashToUnit(h) * 2.0 - 1.0, a = hashToUnit(mix64(h + 1)) * 2.0 * kPi;
    const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
}

std::vector<Star> generateStars(uint64_t seed, int count) {
    std::vector<Star> stars;
    stars.reserve(size_t(count));
    const dvec3 pole = galacticPole(seed);
    const dvec3 t0 = normalize(cross(pole, std::fabs(pole.z) < 0.9 ? dvec3{0, 0, 1} : dvec3{1, 0, 0}));
    const dvec3 t1 = cross(pole, t0);
    uint64_t h = mix64(seed ^ 0x57a25);
    auto next = [&] {
        h = mix64(h + 0x9e3779b97f4a7c15ull);
        return hashToUnit(h);
    };
    for (int i = 0; i < count; ++i) {
        dvec3 d;
        if (next() < 0.42) {
            // Crowded toward the galaxy's plane.
            const double lon = next() * 2.0 * kPi;
            const double u1 = std::max(next(), 1e-12), u2 = next();
            const double lat = std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2) * 0.2;  // about 11 degrees
            d = normalize((t0 * std::cos(lon) + t1 * std::sin(lon)) * std::cos(lat) + pole * std::sin(lat));
        } else {
            const double z = next() * 2.0 - 1.0, a = next() * 2.0 * kPi, r = std::sqrt(std::max(0.0, 1.0 - z * z));
            d = {r * std::cos(a), r * std::sin(a), z};
        }
        // Many faint, few bright: counts grow about 2.8x per magnitude.
        const double mag = std::max(-1.4, 6.6 + std::log10(std::max(next(), 1e-9)) / 0.45);
        const double kelvin = 3100.0 + 10500.0 * std::pow(next(), 1.9);
        const dvec3 c = blackbody(kelvin);
        Star s;
        s.dir[0] = float(d.x);
        s.dir[1] = float(d.y);
        s.dir[2] = float(d.z);
        s.magnitude = float(mag);
        s.color[0] = float(c.x);
        s.color[1] = float(c.y);
        s.color[2] = float(c.z);
        stars.push_back(s);
    }
    return stars;
}

}  // namespace taelia
