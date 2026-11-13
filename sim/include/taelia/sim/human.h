// The human (step 6): one adult, about 1.75 m, ordinary proportions, plain clothes, one fixed body and face. Its body,
// eyes, brows, lashes, hair and clothes come from MakeHuman's CC0 assets, converted by .utilities/makehuman.py into
// assets/human/human.bin (the same file for every front end), skinned to MakeHuman's full skeleton of 163 bones (the
// spine, neck, jaw, lips, tongue, eyes and lids, every finger and toe). For now it stands in an idle pose: it breathes
// slowly, shifts its weight, blinks and glances about.
//
// It holds life (nature) magic, densest in the heart and the brain, and the magic flows like blood (simplified): out
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

// The idle pose at this moment (simulated seconds): standing, arms hanging, fingers relaxed; it breathes slowly (the
// chest fills, the shoulders rise a little), shifts its weight, blinks now and then and glances about. The feet stay put.
HumanPose humanIdlePose(double seconds);

// Where a rest-space vertex goes in a pose, skinned like the mesh.
fvec3 humanSkin(const HumanPose& pose, const HumanVertex& v);

// The life (nature) magic it holds at a point of its body (model space, kg/m3) when its share is full: its share
// everywhere, far denser in the heart and the brain. Scale by how full its share is.
double humanLifeMagicAt(const dvec3& p);
// The heartbeat now: 0 between beats, up to 1 at each of the two close knocks of a beat.
double humanHeartbeat(double seconds);

}  // namespace taelia
