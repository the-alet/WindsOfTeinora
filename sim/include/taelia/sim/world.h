// The world: seed, clock, settings, and the fixed step. No engine or graphics code here.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <memory>
#include <vector>

#include "taelia/shared_constants.h"
#include "taelia/sim/clock.h"
#include "taelia/sim/math.h"
#include "taelia/sim/clouds.h"
#include "taelia/sim/magic.h"
#include "taelia/sim/physics.h"
#include "taelia/sim/terrain.h"
#include "taelia/sim/wind.h"

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
    MagicOptions magic;
};

class World {
public:
    explicit World(const WorldDesc& d);
    ~World();
    World(const World&) = delete;  // the physics holds on to the terrain and the wind
    World& operator=(const World&) = delete;

    // Runs the fixed steps owed for this much real time; returns how many ran.
    int advance(double realSeconds);
    void step();

    double groundHeight(double x, double z) const { return terrain_.height(x, z); }
    const Terrain& terrain() const { return terrain_; }
    const CloudMap& clouds() const { return clouds_; }
    dvec3 wind() const;                            // m/s: the set wind, at WIND_REFERENCE_HEIGHT
    dvec3 windAt(const dvec3& p) const;            // the air at p, gusts and eddies included

    // Throws a ball from `from` (its centre) at `velocity`; returns its id. Past BALL_MAX the oldest goes.
    uint32_t throwBall(const dvec3& from, const dvec3& velocity) { return physics_->throwBall(from, velocity); }
    const std::vector<Ball>& balls() const { return physics_->balls(); }
    // Balls to draw, between the last two steps by the clock's leftover time.
    void ballPoses(std::vector<BallPose>& out) const { physics_->poses(clock_.stepFraction(), out); }
    PhysicsStats physicsStats() const { return physics_->stats(); }

    // The magic field runs around this point (the camera); before the first focus it is only the baseline.
    void setFocus(const dvec3& p) {
        focus_ = p;
        magic_->setFocus(p);
        clouds_.setFocus(p.x, p.z);
    }
    // The weather here: rain and storm, 0..1 (the one cloud map's).
    double rainAt(const dvec3& p) const { return clouds_.rainAt(p.x, p.z); }
    double stormAt(const dvec3& p) const { return clouds_.stormAt(p.x, p.z); }
    // Lightning strikes of the last few seconds: where each hit the ground, and when (simulated seconds).
    struct Strike {
        dvec3 position;
        double seconds = 0;
    };
    const std::vector<Strike>& strikes() const { return strikes_; }
    const dvec3& focus() const { return focus_; }
    // The campfire: where its core of fire magic sits, and how strongly it burns (1 as it starts).
    dvec3 campfireCore() const { return terrain_.campfirePosition() + dvec3{0, 0.4, 0}; }
    double campfireStrength() const { return fire_; }
    // What a ball holds of its own aspect (nature), kg/m3; it falls apart below MAGIC_FALL_APART of MAGIC_HELD.
    double ballHeld(uint32_t id) const {
        const auto it = held_.find(id);
        return it == held_.end() ? k::MAGIC_HELD : it->second;
    }
    // The magic at p, kg/m3 by aspect: the field where it runs, else the baseline.
    MagicSample magicAt(const dvec3& p) const { return magic_->sample(p); }
    const MagicField& magic() const { return *magic_; }
    MagicField& magic() { return *magic_; }

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
    WindField wind_;
    std::unique_ptr<Physics> physics_;
    std::unique_ptr<MagicField> magic_;
    dvec3 focus_{};
    double fire_ = 1.0;
    std::map<uint32_t, double> held_;  // balls' held magic
    std::set<uint32_t> rocks_;         // minerals already rock to the physics
    std::vector<Strike> strikes_;
};

}  // namespace taelia
