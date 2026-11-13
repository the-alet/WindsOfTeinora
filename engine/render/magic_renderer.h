// The magic field on the GPU (step 5c): the field's two finer levels resampled into windows of absolute height around
// the camera, as RGBA16F 3D textures (rgb: the aspects' colours weighted by their densities, a: the total density,
// kg/m3), uploaded only when the field or the window changes. lighting.hlsl marches them for the free magic.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rhi/rhi.h"
#include "taelia/sim/magic.h"

namespace taelia {

namespace gpu {
struct FrameConstants;
}

class MagicRenderer {
public:
    bool init(rhi::Device* device, std::string* error);
    void shutdown();
    // Before the frame's constants are filled: uploads what changed.
    void update(const MagicField* field, const dvec3& camera);
    void frameData(gpu::FrameConstants& fc, const dvec3& camera) const;

private:
    struct Window {
        std::unique_ptr<rhi::Texture> volume;
        uint32_t version = ~0u;
        int64_t ox = 0, oz = 0, k0 = 0;  // world column of the min corner, absolute layer of the bottom
        double dx = 0, dy = 0;
        bool valid = false;
    };
    rhi::Device* device_ = nullptr;
    Window win_[2];
    std::vector<uint16_t> texels_;
};

}  // namespace taelia
