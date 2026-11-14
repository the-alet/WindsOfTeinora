// Magic balls (the user): condensed magic of one aspect, gathering between the player's hands or flying, drawn after
// the lighting as glowing spheres in their aspect's colour (brighter the fuller), each with its own look: fire
// flickers, lightning crackles, water and life swirl, air streaks, light blazes, earth smoulders, necrotic smokes.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "rhi/rhi.h"
#include "taelia/sim/math.h"

namespace taelia {

// A puff of dust where a thing of pure life magic crumbled: where, how big, how long ago (s).
struct RenderDust {
    dvec3 position;
    double radius = 0.1, age = 0;
};

struct RenderMagicBall {
    dvec3 position;
    double radius = 0.1;  // m
    double fill = 1;      // 0..1
    int aspect = 3;       // Aspect order
    uint32_t id = 0;
};

class MagicBallRenderer {
public:
    bool init(rhi::Device* device, std::string* error);
    void shutdown();
    void update(const std::vector<RenderMagicBall>& balls, const std::vector<RenderDust>& dust, const dvec3& camera);
    // After the lighting: into the HDR target, tested against the depth (read only).
    void draw(rhi::CommandList* cmd, const rhi::Transient& frameConstants, rhi::Texture* hdr, rhi::Texture* depth);

private:
    struct InstanceGpu {  // 32 bytes, read by magic_ball.hlsl
        float rel[3];
        float radius;
        float aspect, fill, seed, pad;
    };
    rhi::Device* device_ = nullptr;
    std::unique_ptr<rhi::Pipeline> pipe_, dustPipe_;
    std::vector<InstanceGpu> instances_, dust_;
};

}  // namespace taelia
