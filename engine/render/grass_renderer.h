// Grass blades around the camera, scattered on the GPU from the terrain tiles and drawn indirect.
#pragma once
#include <memory>
#include <string>

#include "render/terrain_renderer.h"
#include "rhi/rhi.h"

namespace taelia {

class GrassRenderer {
public:
    bool init(rhi::Device* device, int detail, std::string* error);
    void shutdown();
    // Outside render passes, after the terrain tiles are uploaded.
    void generate(rhi::CommandList* cmd, const TerrainRenderer& terrain, const dvec3& camera, const rhi::Transient& frameConstants);
    // Inside the G-buffer pass.
    void draw(rhi::CommandList* cmd, const rhi::Transient& frameConstants);

private:
    rhi::Device* device_ = nullptr;
    double radius_ = 60.0;
    uint32_t maxBlades_ = 2400000;
    std::unique_ptr<rhi::Buffer> blades_, args_;
    std::unique_ptr<rhi::Texture> albedo_, detail_;
    std::unique_ptr<rhi::Pipeline> reset_, gen_, clamp_, draw_, texgen_;
    bool textureReady_ = false;
    uint8_t push_[64] = {};  // the last generate's push constants, reused by draw
};

}  // namespace taelia
