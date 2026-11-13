// The world: seed, clock, settings, and the fixed step. No engine or graphics code here.
#pragma once
#include <cstdint>

#include "taelia/sim/clock.h"
#include "taelia/sim/math.h"
#include "taelia/sim/clouds.h"
#include "taelia/sim/terrain.h"

namespace taelia {

struct WorldSettings {
    double windSpeed = 2.5;     // m/s, gentle by default
    double windFromDeg = 250;   // where the wind blows from, degrees clockwise from north (+Z)
    double rain = 0;            // 0..1 (rain arrives with the weather in step 5)
    double cloudCover = -1;     // < 0: the natural weather; 0..1: a fixed cover
};

struct WorldDesc {
    uint64_t seed = 1;
    double startHour = 9.5;
    int64_t startDay = 0;
    WorldSettings settings;
};

class World {
public:
    explicit World(const WorldDesc& d);

    // Runs the fixed steps owed for this much real time; returns how many ran.
    int advance(double realSeconds);
    void step();

    double groundHeight(double x, double z) const { return terrain_.height(x, z); }
    const Terrain& terrain() const { return terrain_; }
    const CloudMap& clouds() const { return clouds_; }
    dvec3 wind() const;                            // m/s at the camera (one vector until the magic field drives it)

    uint64_t seed() const { return seed_; }
    WorldClock& clock() { return clock_; }
    const WorldClock& clock() const { return clock_; }
    WorldSettings& settings() { return settings_; }
    const WorldSettings& settings() const { return settings_; }
    // A digest of the state, for determinism tests.
    uint64_t stateHash() const;

private:
    uint64_t seed_;
    WorldClock clock_;
    WorldSettings settings_;
    Terrain terrain_;
    CloudMap clouds_;
};

}  // namespace taelia
