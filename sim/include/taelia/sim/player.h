// The player (the user: the human is the player; the camera rides it). On foot it runs, crouches, jumps and dashes;
// in deep water it swims; with F it flies. Its body turns toward where it goes (or where it looks while it aims), and
// its moves drive the human's pose (sim/human.h HumanMotion). It gathers magic balls between its hands (World).
// Engine-free: every front end shares it.
#pragma once
#include <cstdint>
#include <vector>

#include "taelia/sim/human.h"
#include "taelia/sim/magic_kinetics.h"
#include "taelia/sim/math.h"
#include "taelia/sim/plants.h"

namespace taelia {

class Terrain;

// What the keys and the mouse ask for, per frame. Held keys stay as they are; pressed ones (dash, fly) are taken once.
struct PlayerInput {
    double forward = 0, right = 0;  // -1..1, relative to where it looks (W/S, D/A)
    double yaw = 0, pitch = 0;      // where it looks (the camera), radians: +z turned toward +x, + up
    bool jump = false;              // Space held: jumps on the ground; rises in flight
    bool crouch = false;            // C held: crouches on the ground; sinks in flight
    bool dash = false;              // Shift pressed: a burst forward
    bool fly = false;               // F pressed: takes off, or stops flying
    bool face = false;              // its body turns to where it looks (aiming, first person)
    bool gather = false;            // LMB held: gathers a magic ball of `aspect`; let go, it throws it
    Aspect aspect = Aspect::Fire;   // chosen with the number keys
    dvec3 aim;                      // where a thrown ball goes (the point under the screen's centre)
};

enum class PlayerMode : uint8_t { Ground, Air, Swim, Fly };

struct Player {
    dvec3 position;         // its feet on the ground; swimming, the point of the surface it floats at
    dvec3 velocity;         // m/s
    double yaw = 0;         // its body's facing
    PlayerMode mode = PlayerMode::Ground;
    // Eased weights for the pose (0..1) and the gait.
    double crouch = 0, air = 0, swim = 0, fly = 0, dash = 0, land = 0, form = 0;
    double gait = 0;        // the gait's phase in cycles
    double dashLeft = 0;    // s left of a dash's burst
    double dashWait = 0;    // s until it can dash again
    double lean = 0;        // into a turn (radians)
    double lookYaw = 0, lookPitch = 0;
    double toss = -1;       // s since it threw a ball (< 0: none lately)
    bool jumpHeld = false;  // Space still down since the last jump
};

// One fixed step: moves it over the terrain (walking up and down slopes, falling off edges, swimming where the water is
// deeper than PLAYER_SWIM_DEPTH, flying), keeps it out of tree trunks, eases its pose weights. `trees`: the trees near it.
void stepPlayer(Player& p, const PlayerInput& in, const Terrain& terrain, const std::vector<TreeInstance>& trees, double dt);
// Its human's motion now (for the pose; `seconds`: simulated time). `ball`: how full the ball it gathers is (0..1).
HumanMotion playerMotion(const Player& p, double seconds, double ball);
// Where it looks from (its eyes, world space) and the third-person camera behind its shoulder looking along (yaw,
// pitch), kept above the ground.
dvec3 playerEyes(const Player& p, const HumanPose& pose);
dvec3 playerCamera(const Player& p, const Terrain& terrain, double yaw, double pitch);
// Turns a model-space point of its human into the world.
dvec3 playerToWorld(const Player& p, const dvec3& model);

}  // namespace taelia
