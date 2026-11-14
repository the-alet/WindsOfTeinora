#include "render/grass_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "render/gbuffer.h"

namespace taelia {

namespace {

constexpr double kCell = 0.25;  // grass.hlsl CELL

struct GrassPush {
    float cornerRel[2];
    int32_t cornerCell[2];
    uint32_t lookupSrv, lookupOffset, gridTiles, poolSrv;
    float tileSize, radius;
    uint32_t bladesUav, argsUav;
    uint32_t maxBlades, bladesSrv, bladeAlbedo, bladeDetail;
};
static_assert(sizeof(GrassPush) == 64, "grass.hlsl push constants");

}  // namespace

bool GrassRenderer::init(rhi::Device* device, int detail, std::string* error) {
    device_ = device;
    static const double kRadius[4] = {50, 75, 110, 150};  // the user: grass farther out
    radius_ = kRadius[std::clamp(detail, 0, 3)];
    blades_ = device_->createBuffer({uint64_t(maxBlades_) * 32, rhi::BufferUsage::Storage, rhi::Memory::GpuOnly, "grass.blades"});
    args_ = device_->createBuffer({16, rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect, rhi::Memory::GpuOnly, "grass.args"});
    rhi::TextureDesc t;
    t.width = 64;
    t.height = 256;
    t.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
    t.format = rhi::Format::RGBA16_FLOAT;
    t.name = "grass.albedo";
    albedo_ = device_->createTexture(t);
    t.format = rhi::Format::RGBA8_UNORM;
    t.name = "grass.detail";
    detail_ = device_->createTexture(t);
    reset_ = device_->createComputePipeline({"grass.ResetMain", "grass.reset"}, error);
    gen_ = device_->createComputePipeline({"grass.GenMain", "grass.gen"}, error);
    clamp_ = device_->createComputePipeline({"grass.ClampMain", "grass.clamp"}, error);
    texgen_ = device_->createComputePipeline({"treetex.BladeMain", "grass.texture"}, error);
    rhi::GraphicsPipelineDesc d;
    d.vs = "grass.VSMain";
    d.ps = "grass.PSMain";
    gbuffer::fillFormats(d);
    d.cull = rhi::Cull::None;
    d.name = "grass";
    draw_ = device_->createGraphicsPipeline(d, error);
    textureReady_ = false;
    return blades_ && args_ && albedo_ && detail_ && reset_ && gen_ && clamp_ && texgen_ && draw_;
}

void GrassRenderer::shutdown() {
    blades_.reset();
    args_.reset();
    albedo_.reset();
    detail_.reset();
    reset_.reset();
    gen_.reset();
    clamp_.reset();
    draw_.reset();
    texgen_.reset();
}

void GrassRenderer::generate(rhi::CommandList* cmd, const TerrainRenderer& terrain, const dvec3& cam, const rhi::Transient& fc) {
    if (!textureReady_) {
        cmd->barrier(albedo_.get(), rhi::State::UnorderedAccess);
        cmd->barrier(detail_.get(), rhi::State::UnorderedAccess);
        cmd->setPipeline(texgen_.get());
        const uint32_t p[8] = {albedo_->uav(0), detail_->uav(0), 64, 256, 0, 0, 0, 0};
        cmd->pushConstants(p, sizeof p);
        cmd->dispatch(64 / 8, 256 / 8);
        cmd->barrier(albedo_.get(), rhi::State::ShaderRead);
        cmd->barrier(detail_.get(), rhi::State::ShaderRead);
        textureReady_ = true;
    }
    const double tile = terrain.leafSize();
    const int64_t ix0 = int64_t(std::floor((cam.x - radius_) / tile)), iz0 = int64_t(std::floor((cam.z - radius_) / tile));
    const uint32_t n = uint32_t(std::ceil(2.0 * radius_ / tile)) + 1;
    std::vector<int32_t> lookup(size_t(n) * n);
    for (uint32_t j = 0; j < n; ++j)
        for (uint32_t i = 0; i < n; ++i) lookup[size_t(j) * n + i] = terrain.coveringSlot(ix0 + int64_t(i), iz0 + int64_t(j));
    rhi::Transient lt = device_->allocTransient(lookup.size() * 4);
    std::memcpy(lt.cpu, lookup.data(), lookup.size() * 4);

    GrassPush p{};
    p.cornerRel[0] = float(double(ix0) * tile - cam.x);
    p.cornerRel[1] = float(double(iz0) * tile - cam.z);
    const int64_t cellsPerTile = int64_t(std::lround(tile / kCell));
    p.cornerCell[0] = int32_t(ix0 * cellsPerTile);
    p.cornerCell[1] = int32_t(iz0 * cellsPerTile);
    p.lookupSrv = lt.buffer->srv();
    p.lookupOffset = uint32_t(lt.offset);
    p.gridTiles = n;
    p.poolSrv = terrain.pool()->srv();
    p.tileSize = float(tile);
    p.radius = float(radius_);
    p.bladesUav = blades_->uav();
    p.argsUav = args_->uav();
    p.maxBlades = maxBlades_;
    p.bladesSrv = blades_->srv();
    p.bladeAlbedo = albedo_->srv();
    p.bladeDetail = detail_->srv();
    std::memcpy(push_, &p, sizeof p);

    cmd->beginMarker("grass scatter");
    cmd->barrier(args_.get(), rhi::State::UnorderedAccess);
    cmd->barrier(blades_.get(), rhi::State::UnorderedAccess);
    cmd->setPipeline(reset_.get());
    cmd->setFrameConstants(fc);
    cmd->pushConstants(&p, sizeof p);
    cmd->dispatch(1);
    cmd->barrier(args_.get(), rhi::State::UnorderedAccess);
    cmd->setPipeline(gen_.get());
    const uint32_t cells = uint32_t(n * cellsPerTile);
    cmd->dispatch((cells + 7) / 8, (cells + 7) / 8);
    cmd->barrier(args_.get(), rhi::State::UnorderedAccess);
    cmd->setPipeline(clamp_.get());
    cmd->dispatch(1);
    cmd->barrier(args_.get(), rhi::State::IndirectArgument);
    cmd->barrier(blades_.get(), rhi::State::ShaderRead);
    cmd->endMarker();
}

void GrassRenderer::draw(rhi::CommandList* cmd, const rhi::Transient& fc) {
    cmd->beginMarker("grass");
    cmd->setPipeline(draw_.get());
    cmd->setFrameConstants(fc);
    cmd->pushConstants(push_, sizeof push_);
    cmd->drawIndirect(args_.get(), 0);
    cmd->endMarker();
}

}  // namespace taelia
