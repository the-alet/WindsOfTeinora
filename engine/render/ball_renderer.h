// Thrown balls: an instanced UV sphere drawn into the G-buffer with its own motion (for DLSS), and one ray-tracing
// instance per ball (shadows, water reflections). Poses come from the simulation (World::ballPoses).
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "render/terrain_renderer.h"
#include "rhi/rhi.h"
#include "taelia/sim/physics.h"

namespace taelia {

class BallRenderer {
public:
    // detail 0..3: sphere tessellation (more polygons at the highest).
    bool init(rhi::Device* device, int detail, std::string* error);
    void shutdown();
    // Once per frame, before the G-buffer pass: this frame's camera-relative poses, last frame's kept per ball id.
    void update(const std::vector<BallPose>* poses, const dvec3& camera, const Frustum& frustum);
    // Inside the G-buffer pass.
    void draw(rhi::CommandList* cmd, const rhi::Transient& frameConstants);
    // Ray tracing: the sphere structure (built once, outside render passes) and one instance per ball.
    void buildStructures(rhi::CommandList* cmd);
    void addInstances(std::vector<rhi::TlasInstance>& out) const;

private:
    struct InstanceGpu {  // 80 bytes, read by ball.hlsl
        float rel[4];      // centre relative to the camera
        float rot[4];      // quaternion x, y, z, w
        float prevRel[4];  // last frame's centre relative to last frame's camera
        float prevRot[4];
        float squash[4];   // xyz: along what it is squashed (world), w: by how much (share of its radius)
    };
    struct Prev {
        dvec3 position;
        float rotation[4];
    };

    rhi::Device* device_ = nullptr;
    std::unique_ptr<rhi::Buffer> vertices_, indices_;
    uint32_t vertexCount_ = 0, indexCount_ = 0;
    std::unique_ptr<rhi::Pipeline> pipe_;
    std::unique_ptr<rhi::AccelerationStructure> blas_;
    std::unordered_map<uint32_t, Prev> prev_;
    dvec3 prevCamera_;
    bool havePrevCamera_ = false;
    std::vector<InstanceGpu> visible_;
    std::vector<rhi::TlasInstance> rt_;
};

}  // namespace taelia
