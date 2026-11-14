// The human (step 6): one adult, about 1.75 m, ordinary proportions, plain clothes, one fixed body and face. Its body,
// eyes, brows, lashes, hair and clothes come from MakeHuman's CC0 assets, converted by .utilities/makehuman.py into
// assets/human/human.bin (the same file for every front end), skinned to MakeHuman's full skeleton of 163 bones (the
// spine, neck, jaw, lips, tongue, eyes and lids, every finger and toe). For now it stands in an idle pose: it breathes
// slowly, shifts its weight, blinks and glances about.
//
// It holds life magic, densest in the heart and the brain, and the magic flows like blood (simplified): out
// from the heart through the arteries with each heartbeat, back through the veins. Engine-free: every front end shares it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "taelia/sim/math.h"
#include "taelia/sim/plants.h"

namespace taelia {

enum HumanMaterialFlags : uint32_t {
    kHumanAlpha = 1,     // alpha-tested (hair, brows, lashes)
    kHumanTwoSided = 2,
    kHumanSkin = 4,
    kHumanEye = 8,
    kHumanHair = 16,
    kHumanCloth = 32,
};

struct HumanMaterial {
    std::string name, albedo, normal;  // texture files in the asset directory ("" for none)
    uint32_t flags = 0;
    float roughness = 0.5f, specular = 0.5f;
};

// Model space: metres, the soles on y = 0, facing +z, its left toward +x.
struct HumanVertex {
    fvec3 position;
    fvec3 normal;
    float tangent[4] = {1, 0, 0, 1};  // xyz, handedness
    float uv[2] = {0, 0};
    uint8_t bone[4] = {0, 0, 0, 0};
    float weight[4] = {1, 0, 0, 0};   // sum to 1
    float occlusion = 1;              // how much of the sky it sees (baked)
};

struct HumanJoint {
    std::string name;
    int parent = -1;  // parents come first
    fvec3 head, tail;
};

struct HumanSubmesh {
    uint32_t material = 0, firstIndex = 0, indexCount = 0;
};

// A stretch of blood vessel: from a to b, how far along the vessels each end is from the heart (m), artery or vein.
// Airways the same way, from the nose and mouth down to the lungs (m along them).
struct HumanVessel {
    fvec3 a, b;
    float fromHeartA = 0, fromHeartB = 0;
    bool artery = true;
};

struct HumanMesh {
    std::vector<HumanJoint> joints;
    std::vector<HumanMaterial> materials;
    std::vector<HumanVertex> vertices;
    std::vector<uint32_t> indices;  // triangles, counter-clockwise seen from outside
    std::vector<HumanSubmesh> submeshes;
    std::vector<HumanVessel> vessels;
    std::vector<HumanVessel> airways;  // nose and mouth, throat, windpipe, bronchi into each lung (fromHeart: from the nose)
    dvec3 lungs[2];                    // the lungs' middles (left, right); HUMAN_LUNG_RADII across
    dvec3 heart, brain;             // where its life magic is densest
    float height = 0;               // the top of the hair above the soles
    std::string directory;          // where it was read from (its textures are there)
    bool loaded = false;

    int bone(const char* name) const;  // -1 if none
};

// Where assets/human is (set before the first use; by default the source tree's).
void setHumanAssetDirectory(const std::string& dir);
// Read on first use (the same every run), then shared. Not loaded (and empty) if the asset is missing.
const HumanMesh& humanMesh();

// A bone's pose: row-major 3x4 (rotation and scale, then translation), rest model space to posed model space.
struct HumanBoneMatrix {
    float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
};
using HumanPose = std::vector<HumanBoneMatrix>;

// What its body is doing (the player drives it, sim/player.h): each part 0..1 unless said, eased in and out by the
// caller so poses blend without jumps. Model space as the mesh's: the feet's point on the ground at the origin.
struct HumanMotion {
    double seconds = 0;      // simulated: breathing, blinking, glances, strokes
    double gait = 0;         // the gait's phase in cycles (two steps), integrated as speed / humanGaitCycle(speed)
    double speed = 0;        // m/s over the ground
    double crouch = 0;       // crouching
    double air = 0;          // off the ground (a jump, a fall) ...
    double rise = 0;         // ... rising this fast (m/s; < 0 falling)
    double land = 0;         // just landed: the knees take it (1 .. 0)
    double swim = 0;         // swimming (the origin is the point of the surface it floats at) ...
    double swimSpeed = 0;    // ... this fast (m/s)
    double fly = 0;          // flying ...
    double flySpeed = 0;     // ... this fast (m/s) ...
    double flyClimb = 0;     // ... climbing (+1) or diving (-1)
    double dash = 0;         // the burst of a dash
    double form = 0;         // its hands gathering a ball ...
    double ball = 0;         // ... this full (0..1)
    double toss = -1;        // seconds since it threw one (< 0: none lately)
    double lookYaw = 0;      // where it looks, from its body's facing (radians, + to its left) ...
    double lookPitch = 0;    // ... and up (+)
    double lean = 0;         // into a turn (radians, + to its left)
};

// The pose for a motion: standing (arms hanging, fingers relaxed, breathing, shifting its weight, blinking, glancing
// about), walking and running, crouching, jumping and landing, dashing, swimming (breaststroke; treading water when
// still), flying (streamlined at speed, upright when hovering), gathering a ball between its hands and throwing it.
// Legs and arms reach their targets by two-bone IK; feet stay flat on the ground while they stand on it.
HumanPose humanPose(const HumanMotion& motion);
// How far it goes in one gait cycle (two steps) at this speed (m): longer strides the faster it goes.
double humanGaitCycle(double speed);
// The idle pose at this moment (simulated seconds): humanPose of a body standing still.
HumanPose humanIdlePose(double seconds);
// Where the ball it gathers sits between its hands for this motion (model space).
dvec3 humanBallPoint(const HumanMotion& motion);
// Where its eyes are in a pose (model space, between them): the camera in first person.
dvec3 humanEyes(const HumanPose& pose);
// First person: its head (and all on it: eyes, hair, brows) shrunk to a point, so the eye inside sees out.
void hideHumanHead(HumanPose& pose);

// Where a rest-space vertex goes in a pose, skinned like the mesh.
fvec3 humanSkin(const HumanPose& pose, const HumanVertex& v);

// The life magic it holds at a point of its body (model space, kg/m3) when its share is full: its share
// everywhere, far denser in the heart and the brain. Scale by how full its share is.
double humanLifeMagicAt(const dvec3& p);
// The heartbeat now: 0 between beats, up to 1 at each of the two close knocks of a beat.
double humanHeartbeat(double seconds);
// Where it is in a breath (0..1: breathing in up to 0.5, out after), as the pose breathes (faster when it runs).
double humanBreathPhase(const HumanMotion& motion);

}  // namespace taelia
