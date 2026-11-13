// Physics (step 4): thrown balls as PhysX rigid bodies with mass, friction and restitution, over soft ground,
// through elastic plants and turbulent air, floating on ponds. PhysX stays inside the sim's .cpp files: nothing here
// exposes its types, so the renderer and the Unreal port see plain data.
//
// - Ground: PhysX height field tiles streamed around the balls, one material per sample (grass, dirt, rock, sand,
//   pond bed); low restitution, rolling resistance and grass drag make it soft.
// - Plants: trunks and thick branches are elastic PhysX capsules; thin twigs and leaf sprays yield, taking from the
//   ball the share of momentum their own mass can (a twig bends away, a bough bounces it back).
// - Air: drag and spin lift against the wind field's gusts and eddies; water: buoyancy and drag.
// Deterministic: one thread, PhysX enhanced determinism, every container in a fixed order.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "taelia/sim/math.h"

namespace taelia {

class Terrain;
class WindField;

struct Ball {
    uint32_t id = 0;
    dvec3 position;                     // centre, m
    dvec3 velocity;                     // m/s
    dvec3 spin;                         // angular velocity, rad/s
    double rotation[4] = {0, 0, 0, 1};  // orientation quaternion x, y, z, w
    bool resting = false;               // asleep
    bool inWater = false;
};

// A ball to draw: between the last two steps.
struct BallPose {
    uint32_t id = 0;
    dvec3 position;
    float rotation[4] = {0, 0, 0, 1};  // quaternion x, y, z, w
};

struct PhysicsStats {
    int groundTiles = 0;    // height field tiles in the scene
    int trees = 0;          // trees with colliders
    int treeShapes = 0;     // their capsules
    uint64_t twigHits = 0;  // thin twigs and leaf sprays the balls went through, since the start
    uint64_t leafHits = 0;
};

class Physics {
public:
    // terrain == nullptr: flat grassy ground at y = 0, no plants, no water (for tests). wind may be nullptr: still air.
    Physics(const Terrain* terrain, const WindField* wind, uint64_t seed);
    ~Physics();
    Physics(const Physics&) = delete;
    Physics& operator=(const Physics&) = delete;

    // Adds a ball at `from` (its centre) moving at `velocity`; past BALL_MAX the oldest goes. Returns its id.
    uint32_t throwBall(const dvec3& from, const dvec3& velocity);
    // A ball that fell apart (or was taken away). False if there is none by that id.
    bool removeBall(uint32_t id);
    // Rocks the world grew (minerals): solid balls bounce off. Ids are the world's.
    void addRock(uint32_t id, const dvec3& position, double radius);
    void removeRock(uint32_t id);
    // A standing body (the human): an upright capsule round `centre`, soft like cloth over flesh.
    void addCapsule(uint32_t id, const dvec3& centre, double radius, double halfHeight);
    // The magic at a point: its total density and velocity (past the effect threshold dense magic drags things along)
    // and its share of air magic (moving air magic makes the wind).
    using MagicSampler = std::function<void(const dvec3& p, double& rhoTotal, dvec3& velocity, double& airShare)>;
    void setMagicSampler(MagicSampler sampler);
    // One fixed simulation step of dt seconds, ending at simulated time `seconds` (PHYSICS_SUBSTEPS PhysX steps).
    void step(double dt, double seconds);

    const std::vector<Ball>& balls() const;
    // Poses for drawing, `alpha` (0..1) of the way from the previous step to the last one.
    void poses(double alpha, std::vector<BallPose>& out) const;
    PhysicsStats stats() const;
    uint64_t stateHash() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace taelia
