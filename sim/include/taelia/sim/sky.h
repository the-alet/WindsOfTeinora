// The sky: two suns and a ringed gas giant on a sky sphere around the world's centre (40,000 km below the
// ground), eclipses, the invented star catalogue. Pure functions of the world clock: deterministic, engine-free.
// Local frame: x east, y up, z north (the spawn point's horizon).
#pragma once
#include <cstdint>
#include <vector>

#include "taelia/sim/math.h"

namespace taelia {

struct SunState {
    dvec3 dir;            // from the observer
    double coverage = 0;  // 0..1 of the disc hidden by the planet
    bool up = false;      // centre above the horizon
};

struct SkyState {
    SunState sun[2];               // 0 yellow-orange, 1 blue-green
    dvec3 planetDir;               // from the observer
    bool planetUp = false;
    // The planet's look, set from the world's centre so it changes over days, not hours:
    double planetSpin = 0;         // rotation about its own axis, radians
    double ringOpening = 0;        // how far the line of sight is above the ring plane, radians (0 = edge-on)
    double axisAngle = 0;          // the axis tilt on the sky, radians, from the world's north around the line of sight
    dvec3 planetLitFrom[2];        // from the planet toward each sun (its phase)
    double starRotation[9] = {};   // inertial -> local, row-major (stars, the galactic band)
    dvec3 worldNorth;              // the world's axis (the sky turns about it)
};

class Sky {
public:
    static double daySeconds();
    static SkyState at(double seconds);  // world clock seconds since day 0, 00:00:00
    // Angle between a sun and the planet seen from the world's centre (tests and schedules).
    static double centralSeparation(int sun, double seconds);
    // Fraction of a disc of radius rs covered by a disc of radius rp whose centre is d away (all radians).
    static double discCoverage(double d, double rs, double rp);
};

struct Star {
    float dir[3];        // inertial direction
    float magnitude;     // visual magnitude (smaller is brighter)
    float color[3];      // linear colour, luminance 1
};
// The star catalogue: generated, so none of them is a real star or constellation. Deterministic per seed.
std::vector<Star> generateStars(uint64_t seed, int count);
dvec3 galacticPole(uint64_t seed);  // the invented galaxy's pole (inertial)

}  // namespace taelia
