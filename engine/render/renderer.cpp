#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gpu_types.h"
#include "render/gbuffer.h"
#include "taelia/shared_constants.h"
#include "taelia/sim/clock.h"
#include "taelia/sim/hash.h"
#include "taelia/sim/terrain.h"

namespace taelia {

namespace {

struct Mat4 {
    double m[16] = {};  // row-major, column vectors: clip = M * v
    static Mat4 identity() {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
        return r;
    }
    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                double s = 0;
                for (int k = 0; k < 4; ++k) s += m[i * 4 + k] * b.m[k * 4 + j];
                r.m[i * 4 + j] = s;
            }
        return r;
    }
};

bool invert(const Mat4& a, Mat4& out) {
    const double* m = a.m;
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-300) return false;
    for (int i = 0; i < 16; ++i) out.m[i] = inv[i] / det;
    return true;
}

void store(const Mat4& a, gpu::float4x4& out) {
    for (int i = 0; i < 16; ++i) out.m[i] = float(a.m[i]);
}

gpu::float4 f4(double x, double y, double z, double w) { return {float(x), float(y), float(z), float(w)}; }

struct LightPush {
    uint32_t albedo, normal, material, depth, outUav, width, height, pad;
};
struct TexPush {
    uint32_t albedoUav, detailUav, size, material;
};
struct DownPush {
    uint32_t srcUav, dstUav, srcW, srcH, dstW, dstH, pad[2];
};
struct PostPush {
    uint32_t hdrSrv, mipCount, adaptSrv;
    float blurPower, vignette, hdrHeight, pad[2];
};

constexpr uint32_t kMaxMips = 6;
constexpr uint32_t kGroundTexSize = 1024;

std::unique_ptr<rhi::Texture> makeTarget(rhi::Device* dev, uint32_t w, uint32_t h, rhi::Format f, uint32_t usage, const char* name) {
    rhi::TextureDesc d;
    d.width = w;
    d.height = h;
    d.format = f;
    d.usage = usage;
    d.name = name;
    return dev->createTexture(d);
}

}  // namespace

bool Renderer::init(rhi::Device* device, const Terrain* terrain, const CloudMap* clouds, int detail, std::string* error) {
    device_ = device;
    lighting_ = device_->createComputePipeline({"lighting.CSMain", "lighting"}, error);
    texgen_ = device_->createComputePipeline({"texgen.GroundMain", "texgen.ground"}, error);
    downsample_ = device_->createComputePipeline({"downsample.CSMain", "downsample"}, error);
    motionPipe_ = device_->createComputePipeline({"motion.CSMain", "motion"}, error);
    adaptPipe_ = device_->createComputePipeline({"adapt.CSMain", "adapt"}, error);
    const float eye[4] = {0, 1, 1, 1};
    adaptState_ = device_->createBuffer({16, rhi::BufferUsage::Storage, rhi::Memory::GpuOnly, "eye"}, eye);
    adaptFirst_ = true;
    if (!adaptPipe_ || !adaptState_) return false;
    if (!motionPipe_) return false;
    if (!lighting_ || !texgen_ || !downsample_) return false;
    for (int i = 0; i < 4; ++i) {
        rhi::TextureDesc d;
        d.width = d.height = kGroundTexSize;
        d.mips = 11;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
        d.format = rhi::Format::RGBA16_FLOAT;
        d.name = "ground.albedo";
        groundAlbedo_[i] = device_->createTexture(d);
        d.format = rhi::Format::RGBA8_UNORM;
        d.name = "ground.detail";
        groundDetail_[i] = device_->createTexture(d);
    }
    treetex_ = device_->createComputePipeline({"treetex.TreeTexMain", "texgen.trees"}, error);
    if (!treetex_) return false;
    for (int k = 0; k < 3; ++k) {
        rhi::TextureDesc d;
        d.width = d.height = 512;
        d.mips = 10;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
        d.format = rhi::Format::RGBA16_FLOAT;
        d.name = "bark.albedo";
        barkAlbedo_[k] = device_->createTexture(d);
        d.name = "leaf.albedo";
        leafAlbedo_[k] = device_->createTexture(d);
        d.format = rhi::Format::RGBA8_UNORM;
        d.name = "bark.detail";
        barkDetail_[k] = device_->createTexture(d);
        d.name = "leaf.detail";
        leafDetail_[k] = device_->createTexture(d);
    }
    if (!trees_.init(device_, terrain, detail, error)) return false;
    trees_.setRockTextures(groundAlbedo_[2]->srv(), groundDetail_[2]->srv());
    if (!grass_.init(device_, detail, error)) return false;
    if (!balls_.init(device_, detail, error)) return false;
    if (!human_.init(device_, error)) return false;
    if (!manifest_.init(device_, error)) return false;
    if (!magicBalls_.init(device_, error)) return false;
    if (!atmosphere_.init(device_, error) || !cloudRenderer_.init(device_, clouds, error)) return false;
    if (!magic_.init(device_, error)) return false;
    weatherPipe_ = device_->createComputePipeline({"weather.CSMain", "weather"}, error);
    if (!weatherPipe_) return false;
    if (device_->rayTracingSupported()) tlas_ = device_->createTlas(kMaxInstances, "scene.tlas");
    texturesReady_ = false;
    terrainSim_ = terrain;
    rhi::GraphicsPipelineDesc wd;
    wd.vs = "water.VSMain";
    wd.ps = "water.PSMain";
    wd.colorFormats[0] = rhi::Format::RGBA16_FLOAT;
    wd.colorCount = 1;
    wd.depthFormat = gbuffer::kDepth;
    wd.depthTest = true;
    wd.depthWrite = false;
    wd.depthCompare = rhi::Compare::GreaterEqual;
    wd.name = "water";
    water_ = device_->createGraphicsPipeline(wd, error);
    if (!water_) return false;
    if (!terrain_.init(device_, terrain, detail, error)) return false;
    return imgui_.init(device, error);
}

void Renderer::shutdown() {
    imgui_.shutdown();
    terrain_.shutdown();
    trees_.shutdown();
    grass_.shutdown();
    balls_.shutdown();
    human_.shutdown();
    manifest_.shutdown();
    magicBalls_.shutdown();
    atmosphere_.shutdown();
    cloudRenderer_.shutdown();
    magic_.shutdown();
    weatherPipe_.reset();
    tlas_.reset();
    treetex_.reset();
    for (int k = 0; k < 3; ++k) {
        barkAlbedo_[k].reset();
        barkDetail_[k].reset();
        leafAlbedo_[k].reset();
        leafDetail_[k].reset();
    }
    post_.clear();
    lighting_.reset();
    water_.reset();
    texgen_.reset();
    downsample_.reset();
    for (auto& t : groundAlbedo_) t.reset();
    for (auto& t : groundDetail_) t.reset();
    hdr_.reset();
    sceneColor_.reset();
    sceneHdr_.reset();
    motion_.reset();
    dlssOut_.reset();
    motionPipe_.reset();
    starsPipe_.reset();
    adaptPipe_.reset();
    adaptState_.reset();
    stars_.reset();
    depth_.reset();
    gbAlbedo_.reset();
    gbNormal_.reset();
    gbMaterial_.reset();
    gbMotion_.reset();
    device_ = nullptr;
}

void Renderer::generateTextures(rhi::CommandList* cmd) {
    cmd->beginMarker("generated textures");
    cmd->setPipeline(texgen_.get());
    for (uint32_t m = 0; m < 4; ++m) {
        cmd->barrier(groundAlbedo_[m].get(), rhi::State::UnorderedAccess);
        cmd->barrier(groundDetail_[m].get(), rhi::State::UnorderedAccess);
        TexPush p{groundAlbedo_[m]->uav(0), groundDetail_[m]->uav(0), kGroundTexSize, m};
        cmd->pushConstants(&p, sizeof p);
        cmd->dispatch(kGroundTexSize / 8, kGroundTexSize / 8);
    }
    cmd->setPipeline(treetex_.get());
    for (uint32_t k = 0; k < 3; ++k)
        for (uint32_t leaf = 0; leaf < 2; ++leaf) {
            rhi::Texture* a = leaf ? leafAlbedo_[k].get() : barkAlbedo_[k].get();
            rhi::Texture* d = leaf ? leafDetail_[k].get() : barkDetail_[k].get();
            cmd->barrier(a, rhi::State::UnorderedAccess);
            cmd->barrier(d, rhi::State::UnorderedAccess);
            const uint32_t p[8] = {a->uav(0), d->uav(0), a->width(), k, leaf, 0, 0, 0};
            cmd->pushConstants(p, sizeof p);
            cmd->dispatch(a->width() / 8, a->width() / 8);
        }
    // Mips: each level from the one above (UAV barriers between levels).
    cmd->setPipeline(downsample_.get());
    std::vector<rhi::Texture*> all;
    for (uint32_t m = 0; m < 4; ++m) all.insert(all.end(), {groundAlbedo_[m].get(), groundDetail_[m].get()});
    for (uint32_t k = 0; k < 3; ++k) all.insert(all.end(), {barkAlbedo_[k].get(), barkDetail_[k].get(), leafAlbedo_[k].get(), leafDetail_[k].get()});
    for (rhi::Texture* t : all) makeMips(cmd, t);
    cmd->endMarker();
    texturesReady_ = true;
}

// Each mip from the one above (UAV barriers between levels); the downsample pipeline must be set.
void Renderer::makeMips(rhi::CommandList* cmd, rhi::Texture* t) {
    uint32_t w = t->width(), h = t->height();
    for (uint32_t mip = 1; mip < t->desc().mips; ++mip) {
        cmd->barrier(t, rhi::State::UnorderedAccess);
        const uint32_t dw = std::max(1u, w / 2), dh = std::max(1u, h / 2);
        DownPush dp{t->uav(mip - 1), t->uav(mip), w, h, dw, dh, {0, 0}};
        cmd->pushConstants(&dp, sizeof dp);
        cmd->dispatch((dw + 7) / 8, (dh + 7) / 8);
        w = dw;
        h = dh;
    }
    cmd->barrier(t, rhi::State::ShaderRead);
}

rhi::Pipeline* Renderer::postPipeline(rhi::Format format) {
    auto it = post_.find(int(format));
    if (it != post_.end()) return it->second.get();
    rhi::GraphicsPipelineDesc d;
    d.vs = "post.VSMain";
    d.ps = "post.PSMain";
    d.colorFormats[0] = format;
    d.colorCount = 1;
    d.name = "post";
    std::string err;
    auto p = device_->createGraphicsPipeline(d, &err);
    rhi::Pipeline* raw = p.get();
    post_[int(format)] = std::move(p);
    return raw;
}

void Renderer::ensureTargets(uint32_t width, uint32_t height, uint32_t postW, uint32_t postH) {
    using namespace rhi::TextureUsage;
    if (!hdr_ || hdr_->width() != postW || hdr_->height() != postH) {
        // The post input: scene (or DLSS output) copied in, then the blur pyramid on its mips.
        mipCount_ = 1;
        for (uint32_t s = std::max(postW, postH); s > 16 && mipCount_ < kMaxMips; s /= 2) ++mipCount_;
        rhi::TextureDesc h;
        h.width = postW;
        h.height = postH;
        h.mips = mipCount_;
        h.format = rhi::Format::RGBA16_FLOAT;
        h.usage = Sampled | Storage | CopyDst;
        h.name = "hdr";
        hdr_ = device_->createTexture(h);
        dlssOut_ = makeTarget(device_, postW, postH, rhi::Format::RGBA16_FLOAT, Storage | Sampled | CopySrc, "dlss.output");
        resetHistory_ = true;
    }
    if (sceneHdr_ && sceneHdr_->width() == width && sceneHdr_->height() == height) return;
    resetHistory_ = true;
    sceneHdr_ = makeTarget(device_, width, height, rhi::Format::RGBA16_FLOAT, RenderTarget | Sampled | Storage | CopySrc, "scene");
    motion_ = makeTarget(device_, width, height, rhi::Format::RG16_FLOAT, Storage | Sampled, "motion");
    depth_ = makeTarget(device_, width, height, gbuffer::kDepth, DepthStencil | Sampled, "depth");
    gbAlbedo_ = makeTarget(device_, width, height, gbuffer::kAlbedo, RenderTarget | Sampled, "gbuffer.albedo");
    gbNormal_ = makeTarget(device_, width, height, gbuffer::kNormal, RenderTarget | Sampled, "gbuffer.normal");
    gbMaterial_ = makeTarget(device_, width, height, gbuffer::kMaterial, RenderTarget | Sampled, "gbuffer.material");
    gbMotion_ = makeTarget(device_, width, height, gbuffer::kMotion, RenderTarget | Sampled, "gbuffer.motion");
    sceneColor_ = makeTarget(device_, width, height, rhi::Format::RGBA16_FLOAT, Sampled | CopyDst, "sceneColor");
}

void Renderer::drawWater(rhi::CommandList* cmd, const RenderView& v, const rhi::Transient& fc) {
    std::vector<Pond> ponds;
    terrainSim_->pondsNear(v.cameraPos.x, v.cameraPos.z, 3000.0, ponds);
    if (ponds.empty()) return;
    struct PondGpu {
        float centerLevel[4];
        float shape[4];
    };
    std::vector<PondGpu> data;
    for (const Pond& p : ponds) {
        PondGpu g{};
        g.centerLevel[0] = float(p.cx - v.cameraPos.x);
        g.centerLevel[1] = float(p.level - v.cameraPos.y);
        g.centerLevel[2] = float(p.cz - v.cameraPos.z);
        g.centerLevel[3] = float(p.radius);
        for (int i = 0; i < 3; ++i) g.shape[i] = float(hashToUnit(mix64(p.seed + uint64_t(i + 1))));
        g.shape[3] = float(Pond::kFootprint);
        data.push_back(g);
    }
    rhi::Transient buf = device_->allocTransient(data.size() * sizeof(PondGpu));
    std::memcpy(buf.cpu, data.data(), data.size() * sizeof(PondGpu));

    cmd->beginMarker("water");
    cmd->barrier(sceneHdr_.get(), rhi::State::CopySrc);
    cmd->barrier(sceneColor_.get(), rhi::State::CopyDst);
    cmd->copyTexture(sceneColor_.get(), sceneHdr_.get());
    cmd->barrier(sceneColor_.get(), rhi::State::ShaderRead);
    cmd->barrier(sceneHdr_.get(), rhi::State::RenderTarget);
    cmd->barrier(depth_.get(), rhi::State::DepthRead);
    rhi::RenderPass rp;
    rp.colorCount = 1;
    rp.color[0].texture = sceneHdr_.get();
    rp.color[0].load = rhi::LoadOp::Load;
    rp.depth.texture = depth_.get();
    rp.depth.load = rhi::LoadOp::Load;
    rp.depth.readOnly = true;
    cmd->beginRenderPass(rp);
    struct WaterPush {
        uint32_t pondsSrv, pondsOffset, sceneColor, depth;
        float invSize[2], pad[2];
    } push{buf.buffer->srv(), uint32_t(buf.offset), sceneColor_->srv(), depth_->srv(),
           {1.0f / float(sceneHdr_->width()), 1.0f / float(sceneHdr_->height())}, {0, 0}};
    cmd->setPipeline(water_.get());
    cmd->setFrameConstants(fc);
    cmd->pushConstants(&push, sizeof push);
    cmd->draw(96 * 10 * 6, uint32_t(data.size()));
    cmd->endRenderPass();
    cmd->endMarker();
}

// Fires other than the campfire, steam and burnt grass for the frame constants: what is nearest the camera fills the
// slots there are.
void Renderer::fireFrameData(gpu::FrameConstants& fc, const RenderView& v) {
    const auto nearestFirst = [&](std::vector<RenderSpot>& spots) {
        std::sort(spots.begin(), spots.end(), [&](const RenderSpot& a, const RenderSpot& b) {
            return dot(a.position - v.cameraPos, a.position - v.cameraPos) < dot(b.position - v.cameraPos, b.position - v.cameraPos);
        });
    };
    const auto put = [&](gpu::float4& slot, const RenderSpot& s, double w) {
        const dvec3 rel = s.position - v.cameraPos;
        slot = {float(rel.x), float(rel.y), float(rel.z), float(w)};
    };
    // Flames: where fire condensed (a strength like the campfire's, about 1 where it condensed past its threshold by 20)
    // and trees on fire.
    std::vector<RenderSpot> spots = v.burning;
    if (v.magic)
        for (const Flame& f : v.magic->flames()) spots.push_back({f.position, std::min(f.strength / 20.0, 1.5)});
    nearestFirst(spots);
    for (size_t i = 0; i < 3; ++i) fc.flames[i] = {0, 0, 0, 0};
    for (size_t i = 0; i < spots.size() && i < 3; ++i) put(fc.flames[i], spots[i], spots[i].strength);
    // Steam: hot spots over water, gathered into columns (each within 5 m of the first spot it joins).
    spots = v.steam;
    nearestFirst(spots);
    std::vector<RenderSpot> columns;
    for (const RenderSpot& s : spots) {
        size_t k = 0;
        while (k < columns.size() && std::hypot(s.position.x - columns[k].position.x, s.position.z - columns[k].position.z) > 5.0) ++k;
        if (k == columns.size()) {
            if (columns.size() == 2) continue;
            columns.push_back({s.position, 0});
        }
        // Strength: 1 for one spot 30 past the dew point, more for more of them (up to 2.5), thinning as the steam does.
        columns[k].strength += s.strength / 30.0;
    }
    for (size_t i = 0; i < 2; ++i) fc.steam[i] = {0, 0, 0, 0};
    for (size_t i = 0; i < columns.size(); ++i) put(fc.steam[i], columns[i], std::clamp(columns[i].strength, 0.0, 2.5));
    // Burnt grass: the nearest circles.
    spots = v.scorches;
    nearestFirst(spots);
    for (size_t i = 0; i < 8; ++i) fc.scorch[i] = {0, 0, 0, 0};
    for (size_t i = 0; i < spots.size() && i < 8; ++i) put(fc.scorch[i], spots[i], spots[i].strength);
}

void Renderer::fillFrameConstants(const RenderView& v, uint32_t outW, uint32_t outH, rhi::Transient& t, Frustum& frustum) {
    gpu::FrameConstants fc{};
    const double aspect = double(outW) / double(outH);
    const double tanY = std::tan(v.fovY * 0.5);
    const double nearZ = k::CAMERA_NEAR;

    Mat4 view = Mat4::identity();
    const dvec3 axes[3] = {v.right, v.up, v.forward};
    for (int r = 0; r < 3; ++r) {
        view.m[r * 4 + 0] = axes[r].x;
        view.m[r * 4 + 1] = axes[r].y;
        view.m[r * 4 + 2] = axes[r].z;
        view.m[r * 4 + 3] = 0;
    }
    // Reverse-Z, infinite far plane, left-handed: depth = near / viewZ.
    Mat4 proj;
    proj.m[0] = 1.0 / (tanY * aspect);
    proj.m[5] = 1.0 / tanY;
    proj.m[11] = nearZ;
    proj.m[14] = 1.0;
    // DLSS: the unjittered matrices are kept for it and the motion pass; the scene renders jittered.
    Mat4 plainViewProj = proj * view;
    std::memcpy(viewProj_, plainViewProj.m, sizeof viewProj_);
    std::memcpy(proj_, proj.m, sizeof proj_);
    if (upscaling_) {
        proj.m[2] += 2.0 * jitter_[0] / double(sceneHdr_->width());
        proj.m[6] -= 2.0 * jitter_[1] / double(sceneHdr_->height());
    }
    Mat4 viewProj = proj * view, inv;
    invert(viewProj, inv);
    store(viewProj, fc.viewProj);
    store(inv, fc.invViewProj);
    store(view, fc.view);
    // Camera-relative culling planes from the clip-space rows: left, right, bottom, top, near (no far plane).
    const double* m = viewProj.m;
    const int rows[5][2] = {{0, 1}, {0, -1}, {1, 1}, {1, -1}, {2, -1}};
    for (int p = 0; p < 5; ++p) {
        const int r = rows[p][0];
        const double s = rows[p][1];
        for (int c = 0; c < 4; ++c) frustum.planes[p][c] = m[12 + c] + s * m[r * 4 + c];
    }
    frustum.planes[5][0] = frustum.planes[5][1] = frustum.planes[5][2] = 0;
    frustum.planes[5][3] = 1;

    auto split = [](double x, float& hi, float& lo) {
        hi = float(x);
        lo = float(x - double(hi));
    };
    split(v.cameraPos.x, fc.cameraPosHigh.x, fc.cameraPosLow.x);
    split(v.cameraPos.y, fc.cameraPosHigh.y, fc.cameraPosLow.y);
    split(v.cameraPos.z, fc.cameraPosHigh.z, fc.cameraPosLow.z);
    fc.cameraForward = f4(v.forward.x, v.forward.y, v.forward.z, tanY);
    fc.cameraRight = f4(v.right.x, v.right.y, v.right.z, aspect);
    fc.cameraUp = f4(v.up.x, v.up.y, v.up.z, nearZ);
    fc.viewport = f4(outW, outH, 1.0 / outW, 1.0 / outH);
    fc.gaze = f4(v.gazeNdcX * 0.5 + 0.5, 0.5 - v.gazeNdcY * 0.5, k::FOCUS_RADIUS, k::FOCUS_BLUR_MAX);
    fc.time = f4(std::fmod(v.simSeconds, WorldClock::secondsPerDay()), v.hourOfDay, std::fmod(v.realSeconds, 86400.0), v.timeSpeed);

    // The sky (sim/sky.h): two suns, the planet, the stars.
    const SkyState& s = v.sky;
    constexpr double kSunScale = 3.0;  // light of a sun above the air, in our units
    const float* colors[2] = {k::SUN_A_COLOR, k::SUN_B_COLOR};
    const double intensity[2] = {k::SUN_A_INTENSITY, k::SUN_B_INTENSITY};
    const double radius[2] = {k::SUN_A_RADIUS_DEG * kDegToRad, k::SUN_B_RADIUS_DEG * kDegToRad};
    for (int i = 0; i < 2; ++i) {
        fc.suns[i] = f4(s.sun[i].dir.x, s.sun[i].dir.y, s.sun[i].dir.z, s.sun[i].coverage);
        const double e = intensity[i] * kSunScale;
        fc.sunLight[i] = f4(colors[i][0] * e, colors[i][1] * e, colors[i][2] * e, radius[i]);
        fc.planetLit[i] = f4(s.planetLitFrom[i].x, s.planetLitFrom[i].y, s.planetLitFrom[i].z, 0);
    }
    const double planetRadius = k::PLANET_RADIUS_DEG * kDegToRad;
    fc.planet = f4(s.planetDir.x, s.planetDir.y, s.planetDir.z, planetRadius);
    fc.planetPose = f4(s.planetSpin, s.ringOpening, s.axisAngle, k::PLANET_ALBEDO);
    fc.worldNorth = f4(s.worldNorth.x, s.worldNorth.y, s.worldNorth.z, v.cameraPos.y);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) fc.starRotation.m[r * 4 + c] = float(s.starRotation[r * 3 + c]);
    fc.starRotation.m[15] = 1.0f;
    const dvec3 gp = galacticPole(v.seed);
    fc.galacticPole = f4(gp.x, gp.y, gp.z, 1.0);
    // Night: the planet reflects the suns' light (Lambert sphere by its phase), diffuse, bluish; plus airglow.
    dvec3 night;
    const double solid = kPi * planetRadius * planetRadius;
    const dvec3 toObserver = -s.planetDir;
    for (int i = 0; i < 2; ++i) {
        const double a = std::acos(std::clamp(dot(s.planetLitFrom[i], toObserver), -1.0, 1.0));
        const double phase = (std::sin(a) + (kPi - a) * std::cos(a)) / kPi;
        const double e = k::PLANET_ALBEDO * intensity[i] * kSunScale * solid * phase * 2.0;
        night += dvec3{colors[i][0] * 0.62, colors[i][1] * 0.78, colors[i][2] * 1.0} * e;
    }
    night = night * smoothstep(-0.02, 0.08, s.planetDir.y);
    fc.nightLight = f4(night.x, night.y, night.z, 3e-5);
    // The older single-light fields, still read by a few passes: the brighter visible sun.
    const int main = s.sun[1].dir.y > s.sun[0].dir.y ? 1 : 0;
    const dvec3 sun = s.sun[main].dir;
    const double day = smoothstep(-0.08, 0.25, sun.y) * (1.0 - s.sun[main].coverage);
    fc.sunDir = f4(sun.x, sun.y, sun.z, 0);
    fc.sunColor = f4(colors[main][0] * kSunScale * day, colors[main][1] * kSunScale * day, colors[main][2] * kSunScale * day, 0);
    fc.ambient = f4(0.05 + 0.42 * day, 0.055 + 0.48 * day, 0.07 + 0.58 * day, 1);
    float cl[4], cm[4];
    cloudRenderer_.frameData(v.cameraPos, cl, cm);
    fc.clouds = {cl[0], cl[1], cl[2], cl[3]};
    fc.cloudMap = {cm[0], cm[1], cm[2], cm[3]};
    fc.exposure = f4(1.0, 1.0, 1.0, 1.0);
    fc.atmosphere = {atmosphere_.transmittanceLut(), atmosphere_.multiScatteringLut(), atmosphere_.skyViewLut(),
                     atmosphere_.aerialPerspectiveVolume()};
    fc.atmosphere2 = {atmosphere_.skyIrradiance(), cloudRenderer_.shadowMap(), cloudRenderer_.mapTexture(), cloudRenderer_.result()};
    magic_.frameData(fc, v.cameraPos);
    terrain_.dentsFrameData(fc, v.cameraPos);
    manifest_.frameData(fc, v.magic, v.cameraPos);
    fireFrameData(fc, v);
    {
        // Under a pond's surface the water colours everything the eye sees.
        const TerrainSample ground = terrainSim_->sample(v.cameraPos.x, v.cameraPos.z);
        const bool under = ground.underwater() && v.cameraPos.y < ground.waterLevel;
        fc.underwater = {under ? 1.0f : 0.0f, under ? float(ground.waterLevel - v.cameraPos.y) : 0.0f, 0.0f, 0.0f};
    }
    {
        // The flash: bright at once, fading over a fifth of a second with a flicker.
        const double age = v.strikeAge;
        const double flash = age >= 0.0 && age < 0.25 ? (1.0 - age / 0.25) * (0.7 + 0.3 * std::cos(age * 90.0)) : 0.0;
        fc.weather = {float(v.rain), float(v.storm), float(flash), 0.0f};
        const dvec3 g = v.strike - v.cameraPos;
        uint32_t seed = uint32_t(int64_t(std::floor(v.strike.x)) * 73856093 ^ int64_t(std::floor(v.strike.z)) * 19349663);
        float seedBits;
        std::memcpy(&seedBits, &seed, 4);
        fc.bolt[0] = {float(g.x), float(g.y), float(g.z), seedBits};
        fc.bolt[1] = {float(g.x), float(g.y + 1500.0), float(g.z), float(age)};
    }
    {
        const dvec3 bed = v.campfire - v.cameraPos;
        fc.fire = {float(bed.x), float(bed.y), float(bed.z), float(v.campfireStrength)};
        fc.fireInfo = {float(std::fmod(v.simSeconds, 3600.0)), float(v.wind.x * 0.7), float(v.wind.z * 0.7), 0.0f};
    }
    fc.wind = f4(v.wind.x, v.wind.y, v.wind.z, 0);
    fc.flags = {v.magicView ? 1u : 0u, uint32_t(v.frame), 0u, 0u};
    auto wrap = [](double x, double period) { return float(x - std::floor(x / period) * period); };
    fc.cameraWrap = {wrap(v.cameraPos.x, 256.0), wrap(v.cameraPos.x, 4096.0), wrap(v.cameraPos.z, 256.0), wrap(v.cameraPos.z, 4096.0)};
    Mat4 prev;
    std::memcpy(prev.m, prevViewProj_, sizeof prev.m);
    store(prev, fc.prevViewProj);
    const dvec3 moved = v.cameraPos - prevCamera_;
    fc.cameraDelta = f4(moved.x, moved.y, moved.z, havePrev_ ? 1.0 : 0.0);
    fc.jitter = f4(jitter_[0], jitter_[1], sceneHdr_->width(), sceneHdr_->height());
    std::memcpy(t.cpu, &fc, sizeof fc);
}

void Renderer::render(rhi::CommandList* cmd, rhi::Texture* output, const RenderView& v, ImDrawData* ui, float renderScale) {
    const uint32_t outW = output->width(), outH = output->height();
    // DLSS picks the render size for its mode; without it the render scale does.
    uint32_t rw = 0, rh = 0;
    upscaling_ = !upscaleFailed_ && v.upscale != rhi::UpscaleMode::Off && device_->upscalerSupported() &&
                 device_->upscaleRenderSize(v.upscale, outW, outH, rw, rh) && rw > 0 && rh > 0;
    if (!upscaling_) {
        renderScale = std::clamp(renderScale, 0.25f, 1.0f);
        rw = std::max(1u, uint32_t(outW * renderScale + 0.5f));
        rh = std::max(1u, uint32_t(outH * renderScale + 0.5f));
    }
    ensureTargets(rw, rh, upscaling_ ? outW : rw, upscaling_ ? outH : rh);
    if (v.upscale != lastMode_) resetHistory_ = true;
    lastMode_ = v.upscale;
    jitter_[0] = jitter_[1] = 0.0f;
    if (upscaling_) {
        auto halton = [](uint32_t i, uint32_t b) {
            double f = 1, r = 0;
            for (; i > 0; i /= b) {
                f /= b;
                r += f * (i % b);
            }
            return r;
        };
        const uint32_t i = uint32_t(jitterIndex_++ % 32) + 1;
        jitter_[0] = float(halton(i, 2) - 0.5);
        jitter_[1] = float(halton(i, 3) - 0.5);
    }

    rhi::Transient fc = device_->allocTransient(sizeof(gpu::FrameConstants));
    Frustum frustum;
    magic_.update(v.magic, v.cameraPos);
    for (const RenderRect& r : v.dentAreas) terrain_.refresh(r.x0, r.z0, r.x1, r.z1);
    terrain_.updateDents(v.cameraPos);
    fillFrameConstants(v, outW, outH, fc, frustum);
    if (!texturesReady_) generateTextures(cmd);

    // Terrain tiles for this camera (automated runs wait for all of them, so frames repeat exactly).
    terrain_.select(v.cameraPos, frustum, v.waitForStreaming);
    trees_.select(v.cameraPos, frustum, v.waitForStreaming, v.charring);
    balls_.update(v.balls, v.cameraPos, frustum);
    if (v.human)
        human_.update(v.humanFeet, v.humanYaw, v.humanHeld, v.humanWater, v.humanMotion, v.humanFirstPerson, v.cameraPos, frustum,
                      v.waitForStreaming);
    magicBalls_.update(v.magicBalls, v.dust, v.cameraPos);
    if (std::vector<rhi::Texture*> fresh = human_.takeTexturesNeedingMips(); !fresh.empty()) {
        cmd->beginMarker("human textures");
        cmd->setPipeline(downsample_.get());
        for (rhi::Texture* t : fresh) makeMips(cmd, t);
        cmd->endMarker();
    }
    manifest_.update(v.magic, v.cameraPos);
    terrain_.upload(cmd);
    grass_.generate(cmd, terrain_, v.cameraPos, fc);
    buildScene(cmd, fc);
    atmosphere_.update(cmd, fc);
    cloudRenderer_.update(cmd, fc, v.cameraPos, v.waitForStreaming);

    // G-buffer.
    cmd->beginMarker("gbuffer");
    rhi::Texture* targets[gbuffer::kTargets] = {gbAlbedo_.get(), gbNormal_.get(), gbMaterial_.get(), gbMotion_.get()};
    rhi::RenderPass gp;
    gp.colorCount = gbuffer::kTargets;
    for (uint32_t i = 0; i < gbuffer::kTargets; ++i) {
        cmd->barrier(targets[i], rhi::State::RenderTarget);
        gp.color[i].texture = targets[i];
        gp.color[i].load = rhi::LoadOp::Clear;
    }
    cmd->barrier(depth_.get(), rhi::State::DepthWrite);
    gp.depth.texture = depth_.get();
    gp.depth.load = rhi::LoadOp::Clear;
    gp.depth.clear = 0.0f;
    cmd->beginRenderPass(gp);
    const uint32_t materials[8] = {groundAlbedo_[0]->srv(), groundAlbedo_[1]->srv(), groundAlbedo_[2]->srv(), groundAlbedo_[3]->srv(),
                                   groundDetail_[0]->srv(), groundDetail_[1]->srv(), groundDetail_[2]->srv(), groundDetail_[3]->srv()};
    terrain_.draw(cmd, fc, materials);
    uint32_t barkTex[2][3], leafTex[2][3];
    for (int k = 0; k < 3; ++k) {
        barkTex[0][k] = barkAlbedo_[k]->srv();
        barkTex[1][k] = barkDetail_[k]->srv();
        leafTex[0][k] = leafAlbedo_[k]->srv();
        leafTex[1][k] = leafDetail_[k]->srv();
    }
    trees_.draw(cmd, fc, barkTex, leafTex);
    grass_.draw(cmd, fc);
    balls_.draw(cmd, fc);
    if (v.human) human_.draw(cmd, fc);
    manifest_.draw(cmd, fc);
    cmd->endRenderPass();
    cmd->endMarker();

    // Lighting into HDR mip 0.
    cmd->beginMarker("lighting");
    for (rhi::Texture* t : targets) cmd->barrier(t, rhi::State::ShaderRead);
    cmd->barrier(depth_.get(), rhi::State::ShaderRead);
    cmd->barrier(sceneHdr_.get(), rhi::State::UnorderedAccess);
    cmd->setPipeline(lighting_.get());
    cmd->setFrameConstants(fc);
    LightPush lp{gbAlbedo_->srv(), gbNormal_->srv(), gbMaterial_->srv(), depth_->srv(), sceneHdr_->uav(0), sceneHdr_->width(), sceneHdr_->height(), 0};
    cmd->pushConstants(&lp, sizeof lp);
    cmd->dispatch((sceneHdr_->width() + 7) / 8, (sceneHdr_->height() + 7) / 8);
    cmd->endMarker();
    drawStars(cmd, v, fc);
    cloudRenderer_.render(cmd, fc, sceneHdr_.get(), depth_.get());
    drawWater(cmd, v, fc);
    manifest_.drawFireflies(cmd, fc, sceneHdr_.get(), depth_.get());
    magicBalls_.draw(cmd, fc, sceneHdr_.get(), depth_.get());
    if (weatherPipe_ && (v.rain > 0.001 || (v.strikeAge >= 0.0 && v.strikeAge < 0.3))) {
        cmd->beginMarker("weather");
        cmd->barrier(sceneHdr_.get(), rhi::State::UnorderedAccess);
        cmd->barrier(depth_.get(), rhi::State::ShaderRead);
        cmd->setPipeline(weatherPipe_.get());
        cmd->setFrameConstants(fc);
        const struct {
            uint32_t hdr, depth, width, height;
        } wp{sceneHdr_->uav(0), depth_->srv(), sceneHdr_->width(), sceneHdr_->height()};
        cmd->pushConstants(&wp, sizeof wp);
        cmd->dispatch((sceneHdr_->width() + 7) / 8, (sceneHdr_->height() + 7) / 8);
        cmd->endMarker();
    }
    upscaleOrCopy(cmd, v, fc);

    // Blur pyramid.
    cmd->beginMarker("blur pyramid");
    cmd->barrier(hdr_.get(), rhi::State::UnorderedAccess);
    cmd->setPipeline(downsample_.get());
    uint32_t w = hdr_->width(), h = hdr_->height();
    for (uint32_t m = 1; m < mipCount_; ++m) {
        const uint32_t dw = std::max(1u, w / 2), dh = std::max(1u, h / 2);
        DownPush dp{hdr_->uav(m - 1), hdr_->uav(m), w, h, dw, dh, {0, 0}};
        cmd->pushConstants(&dp, sizeof dp);
        cmd->dispatch((dw + 7) / 8, (dh + 7) / 8);
        cmd->barrier(hdr_.get(), rhi::State::UnorderedAccess);  // UAV barrier before the next mip reads this one
        w = dw;
        h = dh;
    }
    cmd->barrier(hdr_.get(), rhi::State::ShaderRead);
    // The eye adapts to what it sees (adapt.hlsl): exposure and a half white balance for the post pass.
    {
        const double dt = std::clamp(v.realSeconds - lastReal_, 0.0, 0.25);
        lastReal_ = v.realSeconds;
        struct {
            uint32_t hdrSrv, mip, stateUav, first;
            float dt, pad[3];
        } ap{hdr_->srv(), mipCount_ - 1, adaptState_->uav(), adaptFirst_ ? 1u : 0u, float(dt), {0, 0, 0}};
        cmd->barrier(adaptState_.get(), rhi::State::UnorderedAccess);
        cmd->setPipeline(adaptPipe_.get());
        cmd->setFrameConstants(fc);
        cmd->pushConstants(&ap, sizeof ap);
        cmd->dispatch(1);
        cmd->barrier(adaptState_.get(), rhi::State::ShaderRead);
        adaptFirst_ = false;
    }
    cmd->endMarker();

    // Post into the output, then the UI on top.
    cmd->beginMarker("post");
    cmd->barrier(output, rhi::State::RenderTarget);
    rhi::RenderPass pp;
    pp.colorCount = 1;
    pp.color[0].texture = output;
    pp.color[0].load = rhi::LoadOp::DontCare;
    cmd->beginRenderPass(pp);
    if (rhi::Pipeline* post = postPipeline(output->desc().format)) {
        cmd->setPipeline(post);
        cmd->setFrameConstants(fc);
        PostPush pc{hdr_->srv(), mipCount_, adaptState_->srv(), float(k::FOCUS_BLUR_POWER), float(k::VIGNETTE_EDGE), float(hdr_->height()), {0, 0}};
        cmd->pushConstants(&pc, sizeof pc);
        cmd->draw(3);
    }
    cmd->endMarker();
    if (ui) imgui_.render(cmd, output, ui);
    cmd->endRenderPass();
    // Next frame's "last frame".
    std::memcpy(prevViewProj_, viewProj_, sizeof prevViewProj_);
    prevCamera_ = v.cameraPos;
    havePrev_ = true;
    resetHistory_ = false;
}

}  // namespace taelia

namespace taelia {

void Renderer::setDetail(const Terrain* terrain, int detail) {
    device_->waitIdle();
    terrain_.shutdown();
    trees_.shutdown();
    std::string err;
    terrain_.init(device_, terrain, detail, &err);
    trees_.init(device_, terrain, detail, &err);
    trees_.setRockTextures(groundAlbedo_[2]->srv(), groundDetail_[2]->srv());
    grass_.shutdown();
    grass_.init(device_, detail, &err);
    balls_.shutdown();
    balls_.init(device_, detail, &err);
}

}  // namespace taelia

namespace taelia {

void Renderer::buildScene(rhi::CommandList* cmd, const rhi::Transient& fc) {
    auto* f = static_cast<gpu::FrameConstants*>(fc.cpu);
    f->flags.w = 0;
    if (!tlas_) return;
    uint32_t leafSrv[3];
    for (int k = 0; k < 3; ++k) leafSrv[k] = leafAlbedo_[k]->srv();
    trees_.buildStructures(cmd, leafSrv);
    balls_.buildStructures(cmd);
    human_.buildStructures(cmd);
    manifest_.buildStructures(cmd);
    instances_.clear();
    terrain_.addInstances(instances_);
    balls_.addInstances(instances_);  // before the trees, so a full table never drops them
    human_.addInstances(instances_);  // only once it has been placed (update)
    manifest_.addInstances(instances_);
    trees_.addInstances(instances_, 350.0);
    if (instances_.empty()) return;
    const uint32_t count = std::min<uint32_t>(uint32_t(instances_.size()), kMaxInstances);
    cmd->beginMarker("scene structure");
    cmd->buildTlas(tlas_.get(), instances_.data(), count);
    cmd->endMarker();
    f->flags.w = 1;
    f->scene = {tlas_->srv(), trees_.alphaTable() ? trees_.alphaTable()->srv() : 0u, trees_.vertexBuffer()->srv(), trees_.indexBuffer()->srv()};
}

}  // namespace taelia

namespace taelia {

void Renderer::upscaleOrCopy(rhi::CommandList* cmd, const RenderView& v, const rhi::Transient& fc) {
    if (!upscaling_) {
        cmd->barrier(sceneHdr_.get(), rhi::State::CopySrc);
        cmd->barrier(hdr_.get(), rhi::State::CopyDst);
        cmd->copyTexture(hdr_.get(), sceneHdr_.get());
        return;
    }
    cmd->beginMarker("dlss");
    // Camera motion per pixel from depth.
    cmd->barrier(depth_.get(), rhi::State::ShaderRead);
    cmd->barrier(motion_.get(), rhi::State::UnorderedAccess);
    cmd->setPipeline(motionPipe_.get());
    cmd->setFrameConstants(fc);
    const uint32_t mp[8] = {depth_->srv(), motion_->uav(0), motion_->width(), motion_->height(), gbMotion_->srv(), gbMaterial_->srv(), 0, 0};
    cmd->pushConstants(mp, sizeof mp);
    cmd->dispatch((motion_->width() + 7) / 8, (motion_->height() + 7) / 8);
    cmd->barrier(motion_.get(), rhi::State::ShaderRead);
    cmd->barrier(sceneHdr_.get(), rhi::State::ShaderRead);
    cmd->barrier(dlssOut_.get(), rhi::State::UnorderedAccess);

    rhi::UpscaleInputs in;
    in.mode = v.upscale;
    in.color = sceneHdr_.get();
    in.depth = depth_.get();
    in.motion = motion_.get();
    in.output = dlssOut_.get();
    in.renderWidth = sceneHdr_->width();
    in.renderHeight = sceneHdr_->height();
    in.jitterX = jitter_[0];
    in.jitterY = jitter_[1];
    const dvec3 moved = v.cameraPos - prevCamera_;
    in.reset = resetHistory_ || !havePrev_ || length(moved) > 50.0;
    // Current clip -> camera-relative world -> last frame's camera-relative world -> last frame's clip.
    Mat4 cur, prev, proj, inv, projInv, shift = Mat4::identity();
    std::memcpy(cur.m, viewProj_, sizeof cur.m);
    std::memcpy(prev.m, havePrev_ ? prevViewProj_ : viewProj_, sizeof prev.m);
    std::memcpy(proj.m, proj_, sizeof proj.m);
    invert(cur, inv);
    invert(proj, projInv);
    shift.m[3] = havePrev_ ? moved.x : 0.0;
    shift.m[7] = havePrev_ ? moved.y : 0.0;
    shift.m[11] = havePrev_ ? moved.z : 0.0;
    Mat4 toPrev = prev * shift * inv, fromPrev;
    invert(toPrev, fromPrev);
    for (int i = 0; i < 16; ++i) {
        in.viewToClip[i] = float(proj.m[i]);
        in.clipToView[i] = float(projInv.m[i]);
        in.clipToPrevClip[i] = float(toPrev.m[i]);
        in.prevClipToClip[i] = float(fromPrev.m[i]);
    }
    const dvec3 axes[3] = {v.right, v.up, v.forward};
    float* outAxes[3] = {in.cameraRight, in.cameraUp, in.cameraForward};
    for (int a = 0; a < 3; ++a) {
        outAxes[a][0] = float(axes[a].x);
        outAxes[a][1] = float(axes[a].y);
        outAxes[a][2] = float(axes[a].z);
    }
    in.nearZ = float(k::CAMERA_NEAR);
    in.farZ = 100000.0f;
    in.fovY = float(v.fovY);
    in.aspect = float(dlssOut_->width()) / float(dlssOut_->height());
    if (cmd->upscale(in)) {
        cmd->barrier(dlssOut_.get(), rhi::State::CopySrc);
        cmd->barrier(hdr_.get(), rhi::State::CopyDst);
        cmd->copyTexture(hdr_.get(), dlssOut_.get());
    } else {
        upscaleFailed_ = true;  // from the next frame on, render without it
    }
    cmd->endMarker();
}

}  // namespace taelia

namespace taelia {

void Renderer::drawStars(rhi::CommandList* cmd, const RenderView& v, const rhi::Transient& fc) {
    if (!stars_) {
        // The catalogue comes from the simulation (generated per world seed: no real stars).
        const std::vector<Star> cat = generateStars(v.seed, k::STAR_COUNT);
        std::vector<float> data(cat.size() * 8);
        for (size_t i = 0; i < cat.size(); ++i) {
            const Star& s = cat[i];
            const float row[8] = {s.dir[0], s.dir[1], s.dir[2], s.magnitude, s.color[0], s.color[1], s.color[2], 0.0f};
            std::memcpy(&data[i * 8], row, sizeof row);
        }
        stars_ = device_->createBuffer({data.size() * 4, rhi::BufferUsage::Storage, rhi::Memory::GpuOnly, "stars"}, data.data());
        starCount_ = uint32_t(cat.size());
        rhi::GraphicsPipelineDesc d;
        d.vs = "stars.VSMain";
        d.ps = "stars.PSMain";
        d.colorFormats[0] = rhi::Format::RGBA16_FLOAT;
        d.colorCount = 1;
        d.blend[0] = rhi::Blend::Additive;
        d.depthFormat = gbuffer::kDepth;
        d.depthTest = true;  // only where nothing was drawn: depth 0
        d.depthWrite = false;
        d.depthCompare = rhi::Compare::GreaterEqual;
        d.name = "stars";
        std::string err;
        starsPipe_ = device_->createGraphicsPipeline(d, &err);
    }
    if (!starsPipe_ || !stars_) return;
    cmd->beginMarker("stars");
    cmd->barrier(sceneHdr_.get(), rhi::State::RenderTarget);
    cmd->barrier(depth_.get(), rhi::State::DepthRead);
    rhi::RenderPass rp;
    rp.colorCount = 1;
    rp.color[0].texture = sceneHdr_.get();
    rp.color[0].load = rhi::LoadOp::Load;
    rp.depth.texture = depth_.get();
    rp.depth.load = rhi::LoadOp::Load;
    rp.depth.readOnly = true;
    cmd->beginRenderPass(rp);
    cmd->setPipeline(starsPipe_.get());
    cmd->setFrameConstants(fc);
    const struct {
        uint32_t srv, count;
        float brightness, pad;
    } push{stars_->srv(), starCount_, 0.02f, 0.0f};
    cmd->pushConstants(&push, sizeof push);
    cmd->draw(6, starCount_);
    cmd->endRenderPass();
    cmd->endMarker();
}

}  // namespace taelia
