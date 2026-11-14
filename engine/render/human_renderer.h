// The human (step 6): its mesh (MakeHuman's, converted into assets/human, read by the simulation on a worker so the
// game starts at once) uploaded when ready with its textures, skinned on the GPU to this moment's idle pose and drawn
// into the G-buffer piece by piece; its blood vessels for the magic view; one ray-tracing instance of its opaque parts
// (shadows, reflections in water).
#pragma once
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "render/terrain_renderer.h"
#include "rhi/rhi.h"
#include "taelia/sim/human.h"

namespace taelia {

class HumanRenderer {
public:
    bool init(rhi::Device* device, std::string* error);
    void shutdown();
    // Once per frame, before the G-buffer pass. held: how full its share of magic is (0..1). wait: block until its mesh
    // is read (automated runs), else it shows once it is.
    // motion: what its body is doing (its pose); firstPerson: the eye is in its head, so the head is not drawn.
    void update(const dvec3& feet, double yaw, double held, double water, const HumanMotion& motion, bool firstPerson, const dvec3& camera,
                const Frustum& frustum, bool wait);
    // Textures uploaded since the last call whose mips are still to be made (the renderer makes them before drawing).
    std::vector<rhi::Texture*> takeTexturesNeedingMips();
    void draw(rhi::CommandList* cmd, const rhi::Transient& frameConstants);
    // Ray tracing: its structure (rebuilt each frame from the posed shape, the head included) and its instance.
    void buildStructures(rhi::CommandList* cmd);
    void addInstances(std::vector<rhi::TlasInstance>& out) const;

private:
    void upload(const HumanMesh& mesh);
    uint32_t loadTexture(const std::string& path, bool color);

    struct Piece {
        uint32_t firstIndex = 0, indexCount = 0, albedo = 0, normal = 0, flags = 0;
        float roughness = 0.5f, specular = 0.5f;
    };

    rhi::Device* device_ = nullptr;
    std::future<const HumanMesh*> mesh_;
    std::unique_ptr<rhi::Buffer> vertices_, indices_, vessels_, posed_;  // posed_: this frame's positions for ray tracing
    uint32_t vertexCount_ = 0, vesselCount_ = 0, airwayCount_ = 0;
    std::vector<Piece> pieces_;  // opaque first, then alpha-tested
    std::vector<std::unique_ptr<rhi::Texture>> textures_;
    std::vector<rhi::Texture*> needMips_;
    std::unique_ptr<rhi::Pipeline> pipe_;
    std::unique_ptr<rhi::AccelerationStructure> blas_;
    const HumanMesh* mesh_ptr_ = nullptr;
    HumanPose pose_, prevPose_;
    std::vector<float> posedCpu_;  // the pose skinned on the CPU (head included), for the ray-tracing structure
    bool posedDirty_ = false;
    float place_[4] = {0, 0, 0, 0};  // feet relative to the camera, yaw
    float prevPlace_[4] = {0, 0, 0, 0};  // last frame's, relative to last frame's camera
    bool havePrev_ = false;
    float heart_[3] = {}, brain_[3] = {};
    float held_ = 1, seconds_ = 0, water_ = 1, breathPhase_ = 0;
    double heartbeat_ = 0;
    bool visible_ = false, nearEnough_ = false;
    float transform_[12] = {};
};

}  // namespace taelia
