#include "render/atmosphere.h"

namespace taelia {

namespace {

// Table sizes: keep in step with ATMO_*_SIZE in shaders/atmosphere.hlsli.
constexpr uint32_t kTransmittanceW = 256, kTransmittanceH = 64;
constexpr uint32_t kMultiScatteringSize = 32;
constexpr uint32_t kSkyViewW = 192, kSkyViewH = 108;
constexpr uint32_t kAerialSize = 32;
constexpr uint64_t kIrradianceBytes = 10 * 16;  // SH9 rgb (float4 each) + overcast grey

struct AtmoPush {
    uint32_t outIndex, pad[3];
};

std::unique_ptr<rhi::Texture> makeTable(rhi::Device* dev, uint32_t w, uint32_t h, uint32_t d, const char* name) {
    rhi::TextureDesc t;
    t.width = w;
    t.height = h;
    t.depthOrLayers = d;
    t.dim = d > 1 ? rhi::TextureDim::Tex3D : rhi::TextureDim::Tex2D;
    t.format = rhi::Format::RGBA16_FLOAT;
    t.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
    t.name = name;
    return dev->createTexture(t);
}

void run(rhi::CommandList* cmd, rhi::Pipeline* pipe, rhi::Texture* out, uint32_t x, uint32_t y) {
    cmd->setPipeline(pipe);
    cmd->barrier(out, rhi::State::UnorderedAccess);
    const AtmoPush p{out->uav(0), {0, 0, 0}};
    cmd->pushConstants(&p, sizeof p);
    cmd->dispatch(x, y);
    cmd->barrier(out, rhi::State::ShaderRead);
}

}  // namespace

bool AtmosphereRenderer::init(rhi::Device* device, std::string* error) {
    device_ = device;
    transmittancePipe_ = device->createComputePipeline({"atmosphere_luts.TransmittanceMain", "atmosphere.transmittance"}, error);
    multiScatteringPipe_ = device->createComputePipeline({"atmosphere_luts.MultiScatteringMain", "atmosphere.multiscattering"}, error);
    skyViewPipe_ = device->createComputePipeline({"atmosphere_luts.SkyViewMain", "atmosphere.skyview"}, error);
    aerialPipe_ = device->createComputePipeline({"atmosphere_luts.AerialPerspectiveMain", "atmosphere.aerial"}, error);
    irradiancePipe_ = device->createComputePipeline({"atmosphere_luts.IrradianceMain", "atmosphere.irradiance"}, error);
    if (!transmittancePipe_ || !multiScatteringPipe_ || !skyViewPipe_ || !aerialPipe_ || !irradiancePipe_) return false;
    // Created here so the renderer's getters give valid indices from the first frame's constants on.
    transmittance_ = makeTable(device, kTransmittanceW, kTransmittanceH, 1, "atmosphere.transmittance");
    multiScattering_ = makeTable(device, kMultiScatteringSize, kMultiScatteringSize, 1, "atmosphere.multiscattering");
    skyView_ = makeTable(device, kSkyViewW, kSkyViewH, 1, "atmosphere.skyview");
    aerial_ = makeTable(device, kAerialSize, kAerialSize, kAerialSize, "atmosphere.aerial");
    rhi::BufferDesc b;
    b.size = kIrradianceBytes;
    b.usage = rhi::BufferUsage::Storage;
    b.memory = rhi::Memory::GpuOnly;
    b.name = "atmosphere.irradiance";
    irradiance_ = device->createBuffer(b);
    if (!transmittance_ || !multiScattering_ || !skyView_ || !aerial_ || !irradiance_) {
        if (error) *error = "atmosphere: could not create the tables";
        return false;
    }
    staticReady_ = false;
    return true;
}

void AtmosphereRenderer::shutdown() {
    transmittance_.reset();
    multiScattering_.reset();
    skyView_.reset();
    aerial_.reset();
    irradiance_.reset();
    transmittancePipe_.reset();
    multiScatteringPipe_.reset();
    skyViewPipe_.reset();
    aerialPipe_.reset();
    irradiancePipe_.reset();
    staticReady_ = false;
    device_ = nullptr;
}

void AtmosphereRenderer::update(rhi::CommandList* cmd, const rhi::Transient& frameConstants) {
    if (!device_) return;
    cmd->beginMarker("atmosphere");
    cmd->setFrameConstants(frameConstants);
    if (!staticReady_) {
        run(cmd, transmittancePipe_.get(), transmittance_.get(), (kTransmittanceW + 7) / 8, (kTransmittanceH + 7) / 8);
        run(cmd, multiScatteringPipe_.get(), multiScattering_.get(), kMultiScatteringSize, kMultiScatteringSize);
        staticReady_ = true;
    }
    run(cmd, skyViewPipe_.get(), skyView_.get(), (kSkyViewW + 7) / 8, (kSkyViewH + 7) / 8);
    run(cmd, aerialPipe_.get(), aerial_.get(), kAerialSize / 8, kAerialSize / 8);
    cmd->setPipeline(irradiancePipe_.get());
    cmd->barrier(irradiance_.get(), rhi::State::UnorderedAccess);
    const AtmoPush p{irradiance_->uav(), {0, 0, 0}};
    cmd->pushConstants(&p, sizeof p);
    cmd->dispatch(1);
    cmd->barrier(irradiance_.get(), rhi::State::ShaderRead);
    cmd->endMarker();
}

}  // namespace taelia
