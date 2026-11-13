#include "render/cloud_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "gpu_types.h"
#include "taelia/sim/clouds.h"

namespace taelia {
namespace {

constexpr uint32_t kMapSize = 256;      // texels per side
constexpr double kMapTexel = 400.0;     // metres: the map spans about 100 km
constexpr double kRecentre = 10000.0;   // rebuild when the camera is this far from the map's centre
constexpr double kRefresh = 30.0;       // simulated seconds before the map counts as old
constexpr uint32_t kShadowSize = 512;   // texels; their size (32 m) lives in clouds.hlsli
constexpr uint32_t kShapeSize = 128, kDetailSize = 32;
constexpr double kShapeTile = 3000.0;   // metres one shape-noise tile spans (detail: a sixth of it)

// Shared with clouds.hlsl (CloudPush): what each kernel reads is listed there.
struct CloudPush {
    uint32_t tex[8];
    uint32_t size[4];
    float offset[4];
};
static_assert(sizeof(CloudPush) <= rhi::kPushConstantBytes);

uint16_t toHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = int32_t((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffffu;
    if (exp <= 0) return uint16_t(sign);  // tiny values: zero is close enough here
    if (exp >= 31) return uint16_t(sign | 0x7bffu);
    mant += 0x1000u;  // round to nearest
    if (mant & 0x800000u) return uint16_t(sign | (uint32_t(exp + 1) << 10));
    return uint16_t(sign | (uint32_t(exp) << 10) | (mant >> 13));
}

}  // namespace

bool CloudRenderer::init(rhi::Device* device, const CloudMap* clouds, std::string* error) {
    device_ = device;
    clouds_ = clouds;
    shapePipe_ = device->createComputePipeline({"clouds.ShapeNoiseMain", "clouds.shape"}, error);
    detailPipe_ = device->createComputePipeline({"clouds.DetailNoiseMain", "clouds.detail"}, error);
    shadowPipe_ = device->createComputePipeline({"clouds.ShadowMain", "clouds.shadow"}, error);
    marchPipe_ = device->createComputePipeline({"clouds.MarchMain", "clouds.march"}, error);
    resolvePipe_ = device->createComputePipeline({"clouds.ResolveMain", "clouds.resolve"}, error);
    compositePipe_ = device->createComputePipeline({"clouds.CompositeMain", "clouds.composite"}, error);
    if (!shapePipe_ || !detailPipe_ || !shadowPipe_ || !marchPipe_ || !resolvePipe_ || !compositePipe_) return false;
    using namespace rhi::TextureUsage;
    rhi::TextureDesc d;
    d.width = d.height = kMapSize;
    d.format = rhi::Format::RGBA16_FLOAT;
    d.usage = Sampled | CopyDst;
    d.name = "clouds.map";
    map_ = device->createTexture(d);
    d.width = d.height = kShadowSize;
    d.usage = Sampled | Storage;
    d.name = "clouds.shadow";
    shadow_ = device->createTexture(d);
    d.dim = rhi::TextureDim::Tex3D;
    d.format = rhi::Format::RGBA8_UNORM;
    d.width = d.height = d.depthOrLayers = kShapeSize;
    d.name = "clouds.shapeNoise";
    shape_ = device->createTexture(d);
    d.width = d.height = d.depthOrLayers = kDetailSize;
    d.name = "clouds.detailNoise";
    detail_ = device->createTexture(d);
    if (!map_ || !shadow_ || !shape_ || !detail_) {
        if (error) *error = "clouds: texture creation failed";
        return false;
    }
    return true;
}

void CloudRenderer::shutdown() {
    if (pending_.valid()) pending_.wait();
    pending_ = {};
    shapePipe_.reset();
    detailPipe_.reset();
    shadowPipe_.reset();
    marchPipe_.reset();
    resolvePipe_.reset();
    compositePipe_.reset();
    map_.reset();
    shadow_.reset();
    shape_.reset();
    detail_.reset();
    marchColor_.reset();
    marchDepth_.reset();
    result_.reset();
    history_.reset();
    noiseReady_ = haveMap_ = shadowBuilt_ = historyValid_ = false;
    device_ = nullptr;
}

CloudRenderer::MapJob CloudRenderer::buildMap(double u0, double v0, double seconds) const {
    MapJob m;
    m.u0 = u0;
    m.v0 = v0;
    m.seconds = seconds;
    m.texels.resize(size_t(kMapSize) * kMapSize * 4);
    const uint16_t one = toHalf(1.0f);
    const std::shared_ptr<const CloudWater> water = clouds_->water();  // the weather's own cloud water (one map feeds both)
    for (uint32_t j = 0; j < kMapSize; ++j)
        for (uint32_t i = 0; i < kMapSize; ++i) {
            const double u = u0 + (i + 0.5) * kMapTexel, v = v0 + (j + 0.5) * kMapTexel;
            const double c = clouds_->coverageAt(u, v, seconds, water.get());
            uint16_t* t = &m.texels[(size_t(j) * kMapSize + i) * 4];
            t[0] = toHalf(float(c));
            t[1] = toHalf(float(clouds_->baseAt(u, v)));
            t[2] = toHalf(float(clouds_->thicknessFor(c)));
            t[3] = one;
        }
    return m;
}

// The map's corner (cloud coordinates, on the texel grid) that centres it on the camera.
void CloudRenderer::wantedMap(const dvec3& camera, double& u0, double& v0) const {
    const dvec3 disp = clouds_->displacement();
    const double half = kMapSize * 0.5;
    u0 = (std::floor((camera.x - disp.x) / kMapTexel) - half) * kMapTexel;
    v0 = (std::floor((camera.z - disp.z) / kMapTexel) - half) * kMapTexel;
}

bool CloudRenderer::stale(const MapJob& m, double u0, double v0, double seconds) const {
    return std::abs(m.u0 - u0) > kRecentre || std::abs(m.v0 - v0) > kRecentre || std::abs(m.seconds - seconds) > kRefresh;
}

void CloudRenderer::uploadMap(rhi::CommandList* cmd, MapJob&& m) {
    const size_t bytes = m.texels.size() * sizeof(uint16_t);
    rhi::Transient t = device_->allocTransient(bytes);
    if (!t.cpu) return;
    std::memcpy(t.cpu, m.texels.data(), bytes);
    cmd->barrier(map_.get(), rhi::State::CopyDst);
    cmd->copyBufferToTexture(map_.get(), 0, 0, t.buffer, t.offset);
    cmd->barrier(map_.get(), rhi::State::ShaderRead);
    mapU0_ = m.u0;
    mapV0_ = m.v0;
    mapSeconds_ = m.seconds;
    haveMap_ = true;
}

void CloudRenderer::frameData(const dvec3& camera, float clouds[4], float cloudMap[4]) const {
    const dvec3 d = clouds_ ? clouds_->displacement() : dvec3{};
    const double cover = clouds_ ? clouds_->cover(clouds_->seconds()) : 0.0;
    clouds[0] = float(d.x);
    clouds[1] = float(d.z);
    clouds[2] = float(cover);
    clouds[3] = float(kMapTexel);
    cloudMap[0] = float(mapU0_ + d.x - camera.x);
    cloudMap[1] = float(mapV0_ + d.z - camera.z);
    cloudMap[2] = haveMap_ ? float(kMapSize) : 0.0f;
    cloudMap[3] = clouds_ ? float(clouds_->thicknessFor(cover)) : 0.0f;
}

// The frame constants were filled before update() could build anything: bring this frame's up to date.
void CloudRenderer::patchConstants(const rhi::Transient& frameConstants, const dvec3& camera) const {
    auto* fc = static_cast<gpu::FrameConstants*>(frameConstants.cpu);
    if (!fc) return;
    float cl[4], cm[4];
    frameData(camera, cl, cm);
    fc->clouds = {cl[0], cl[1], cl[2], cl[3]};
    fc->cloudMap = {cm[0], cm[1], cm[2], cm[3]};
    fc->atmosphere2.y = shadowMap();
    fc->atmosphere2.z = mapTexture();
    fc->atmosphere2.w = result();
}

// Where the camera sits in the noise tiles (cloud coordinates, wrapped so floats stay exact far from the origin).
void CloudRenderer::noiseOffset(const dvec3& camera, float out[4]) const {
    const dvec3 d = clouds_->displacement();
    auto wrap = [](double x) { return float(x - std::floor(x / kShapeTile) * kShapeTile); };
    out[0] = wrap(camera.x - d.x);
    out[1] = 0.0f;
    out[2] = wrap(camera.z - d.z);
    out[3] = float(kShapeTile);
}

void CloudRenderer::update(rhi::CommandList* cmd, const rhi::Transient& frameConstants, const dvec3& camera, bool wait) {
    if (!clouds_ || !device_) return;
    cmd->beginMarker("clouds update");
    cmd->setFrameConstants(frameConstants);
    if (!noiseReady_) {
        CloudPush p{};
        cmd->barrier(shape_.get(), rhi::State::UnorderedAccess);
        cmd->setPipeline(shapePipe_.get());
        p.tex[0] = shape_->uav(0);
        p.size[0] = kShapeSize;
        cmd->pushConstants(&p, sizeof p);
        cmd->dispatch(kShapeSize / 8, kShapeSize / 8, kShapeSize);
        cmd->barrier(shape_.get(), rhi::State::ShaderRead);
        cmd->barrier(detail_.get(), rhi::State::UnorderedAccess);
        cmd->setPipeline(detailPipe_.get());
        p.tex[0] = detail_->uav(0);
        p.size[0] = kDetailSize;
        cmd->pushConstants(&p, sizeof p);
        cmd->dispatch(kDetailSize / 8, kDetailSize / 8, kDetailSize);
        cmd->barrier(detail_.get(), rhi::State::ShaderRead);
        noiseReady_ = true;
    }

    // The map: take a finished build, start a new one when this one is old (automated runs: build it now).
    double u0, v0;
    wantedMap(camera, u0, v0);
    const double now = clouds_->seconds();
    MapJob current{mapU0_, mapV0_, mapSeconds_, {}};
    if (pending_.valid() && (wait || pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
        MapJob m = pending_.get();
        if (!haveMap_ || !stale(m, u0, v0, now) || wait) {
            current = {m.u0, m.v0, m.seconds, {}};
            uploadMap(cmd, std::move(m));
        }
    }
    if (!haveMap_ || stale(current, u0, v0, now)) {
        if (wait || !haveMap_) {
            uploadMap(cmd, buildMap(u0, v0, now));
        } else if (!pending_.valid()) {
            pending_ = std::async(std::launch::async, [this, u0, v0, now] { return buildMap(u0, v0, now); });
        }
    }

    // The shadow map around the camera, every frame (the wind and the suns move it).
    if (haveMap_) {
        patchConstants(frameConstants, camera);
        CloudPush p{};
        p.tex[0] = shadow_->uav(0);
        p.tex[1] = shape_->srv();
        p.tex[2] = detail_->srv();
        p.size[0] = p.size[1] = kShadowSize;
        noiseOffset(camera, p.offset);
        cmd->barrier(shadow_.get(), rhi::State::UnorderedAccess);
        cmd->setPipeline(shadowPipe_.get());
        cmd->pushConstants(&p, sizeof p);
        cmd->dispatch(kShadowSize / 8, kShadowSize / 8);
        cmd->barrier(shadow_.get(), rhi::State::ShaderRead);
        shadowBuilt_ = true;
    }
    patchConstants(frameConstants, camera);
    lastCamera_ = camera;
    cmd->endMarker();
}

void CloudRenderer::render(rhi::CommandList* cmd, const rhi::Transient& frameConstants, rhi::Texture* scene, rhi::Texture* depth) {
    if (!clouds_ || !device_ || !haveMap_ || !noiseReady_) return;
    using namespace rhi::TextureUsage;
    const uint32_t fw = scene->width(), fh = scene->height();
    const uint32_t hw = (fw + 1) / 2, hh = (fh + 1) / 2;
    if (!marchColor_ || marchColor_->width() != hw || marchColor_->height() != hh) {
        rhi::TextureDesc d;
        d.width = hw;
        d.height = hh;
        d.format = rhi::Format::RGBA16_FLOAT;
        d.usage = Sampled | Storage;
        d.name = "clouds.march";
        marchColor_ = device_->createTexture(d);
        d.usage = Sampled | Storage | CopySrc;
        d.name = "clouds.result";
        result_ = device_->createTexture(d);
        d.usage = Sampled | CopyDst;
        d.name = "clouds.history";
        history_ = device_->createTexture(d);
        d.format = rhi::Format::R32_FLOAT;
        d.usage = Sampled | Storage;
        d.name = "clouds.marchDepth";
        marchDepth_ = device_->createTexture(d);
        historyValid_ = false;
    }
    patchConstants(frameConstants, lastCamera_);
    cmd->beginMarker("clouds");
    cmd->setFrameConstants(frameConstants);
    cmd->barrier(depth, rhi::State::ShaderRead);

    // Raymarch at half resolution, jittered per pixel and frame.
    CloudPush p{};
    p.tex[0] = marchColor_->uav(0);
    p.tex[1] = marchDepth_->uav(0);
    p.tex[2] = shape_->srv();
    p.tex[3] = detail_->srv();
    p.tex[4] = depth->srv();
    p.size[0] = hw;
    p.size[1] = hh;
    p.size[2] = fw;
    p.size[3] = fh;
    noiseOffset(lastCamera_, p.offset);
    cmd->barrier(marchColor_.get(), rhi::State::UnorderedAccess);
    cmd->barrier(marchDepth_.get(), rhi::State::UnorderedAccess);
    cmd->setPipeline(marchPipe_.get());
    cmd->pushConstants(&p, sizeof p);
    cmd->dispatch((hw + 7) / 8, (hh + 7) / 8);
    cmd->barrier(marchColor_.get(), rhi::State::ShaderRead);
    cmd->barrier(marchDepth_.get(), rhi::State::ShaderRead);

    // Temporal accumulation: this frame's samples blended into last frame's clouds, reprojected.
    CloudPush r{};
    r.tex[0] = result_->uav(0);
    r.tex[1] = marchColor_->srv();
    r.tex[2] = marchDepth_->srv();
    r.tex[3] = history_->srv();
    r.tex[4] = historyValid_ ? 1u : 0u;
    r.size[0] = hw;
    r.size[1] = hh;
    r.size[2] = fw;
    r.size[3] = fh;
    cmd->barrier(history_.get(), rhi::State::ShaderRead);
    cmd->barrier(result_.get(), rhi::State::UnorderedAccess);
    cmd->setPipeline(resolvePipe_.get());
    cmd->pushConstants(&r, sizeof r);
    cmd->dispatch((hw + 7) / 8, (hh + 7) / 8);
    cmd->barrier(result_.get(), rhi::State::CopySrc);
    cmd->barrier(history_.get(), rhi::State::CopyDst);
    cmd->copyTexture(history_.get(), result_.get());
    cmd->barrier(history_.get(), rhi::State::ShaderRead);
    cmd->barrier(result_.get(), rhi::State::ShaderRead);
    historyValid_ = true;

    // Composite over the sky and anything farther than the clouds.
    CloudPush c{};
    c.tex[0] = scene->uav(0);
    c.tex[1] = depth->srv();
    c.tex[2] = result_->srv();
    c.tex[3] = marchDepth_->srv();
    c.size[0] = fw;
    c.size[1] = fh;
    c.size[2] = hw;
    c.size[3] = hh;
    cmd->barrier(scene, rhi::State::UnorderedAccess);
    cmd->setPipeline(compositePipe_.get());
    cmd->pushConstants(&c, sizeof c);
    cmd->dispatch((fw + 7) / 8, (fh + 7) / 8);
    cmd->endMarker();
}

}  // namespace taelia
