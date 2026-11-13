#include "render/manifest_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gpu_types.h"
#include "render/gbuffer.h"
#include "taelia/shared_constants.h"

namespace taelia {

namespace {

constexpr uint32_t kMineralInstance = 0xFFFFFC;  // rt.hlsli: ids from 0xFFFFF0 up are opaque props
constexpr size_t kMaxFireflies = 512;

struct ManifestPush {
    uint32_t vbSrv, instSrv, instOffset, kind;  // kind 0 mineral, 1 sprout
};

// Vertices: position xyz, normal xyz.
void addTriangle(std::vector<float>& v, std::vector<uint32_t>& idx, const dvec3& a, const dvec3& b, const dvec3& c) {
    const dvec3 n = normalize(cross(b - a, c - a));
    const uint32_t base = uint32_t(v.size() / 6);
    for (const dvec3& p : {a, b, c}) v.insert(v.end(), {float(p.x), float(p.y), float(p.z), float(n.x), float(n.y), float(n.z)});
    idx.insert(idx.end(), {base, base + 1, base + 2});
}

// A cluster of five six-sided crystals with pointed tips, leaning out from a common foot (unit size: about 1 m tall).
void crystalCluster(std::vector<float>& v, std::vector<uint32_t>& idx) {
    const double lean[5][3] = {{0, 1, 0}, {0.35, 0.9, 0.1}, {-0.3, 0.85, 0.25}, {0.1, 0.8, -0.4}, {-0.25, 0.75, -0.3}};
    const double size[5] = {1.0, 0.7, 0.62, 0.55, 0.45};
    for (int c = 0; c < 5; ++c) {
        const dvec3 up = normalize(dvec3{lean[c][0], lean[c][1], lean[c][2]});
        const dvec3 side = normalize(cross(up, std::abs(up.y) < 0.9 ? dvec3{0, 1, 0} : dvec3{1, 0, 0}));
        const dvec3 other = cross(up, side);
        const double r = 0.11 * size[c], body = 0.75 * size[c], tip = 0.25 * size[c];
        dvec3 ring[6];
        for (int i = 0; i < 6; ++i) {
            const double a = kPi / 3.0 * i;
            ring[i] = side * (std::cos(a) * r) + other * (std::sin(a) * r);
        }
        const dvec3 top = up * (body + tip);
        for (int i = 0; i < 6; ++i) {
            const dvec3 a0 = ring[i], a1 = ring[(i + 1) % 6], b0 = a0 + up * body, b1 = a1 + up * body;
            addTriangle(v, idx, a0, b0, a1);
            addTriangle(v, idx, a1, b0, b1);
            addTriangle(v, idx, b0, top, b1);
        }
    }
}

// A sprout: a short stem and four curved leaves (unit size: about 0.5 m tall), drawn from both sides.
void sprout(std::vector<float>& v, std::vector<uint32_t>& idx) {
    for (int i = 0; i < 4; ++i) {
        const double a = kPi / 2.0 * i + 0.4;
        const dvec3 out{std::cos(a), 0, std::sin(a)}, across{-std::sin(a), 0, std::cos(a)};
        const dvec3 foot = dvec3{0, 0.12 + 0.06 * i, 0};
        const dvec3 mid = foot + out * 0.18 + dvec3{0, 0.12, 0}, tip = foot + out * 0.34 + dvec3{0, 0.08, 0};
        const dvec3 w = across * 0.07;
        addTriangle(v, idx, foot, mid - w, mid + w);
        addTriangle(v, idx, mid - w, tip, mid + w);
        addTriangle(v, idx, foot, mid + w, mid - w);  // the back
        addTriangle(v, idx, mid - w, mid + w, tip);
    }
    const dvec3 s0{-0.01, 0, 0}, s1{0.01, 0, 0}, s2{0, 0.3, 0};
    addTriangle(v, idx, s0, s2, s1);
    addTriangle(v, idx, s0, s1, s2);
}

}  // namespace

bool ManifestRenderer::init(rhi::Device* device, std::string* error) {
    device_ = device;
    std::vector<float> v;
    std::vector<uint32_t> idx;
    crystalCluster(v, idx);
    crystalIndices_ = uint32_t(idx.size());
    crystalVertices_ = uint32_t(v.size() / 6);
    sproutFirst_ = uint32_t(idx.size());
    sprout(v, idx);
    sproutIndices_ = uint32_t(idx.size()) - sproutFirst_;
    vertices_ = device_->createBuffer({v.size() * 4, rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationInput,
                                       rhi::Memory::GpuOnly, "manifest.vertices"}, v.data());
    indices_ = device_->createBuffer({idx.size() * 4, rhi::BufferUsage::Index | rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationInput,
                                      rhi::Memory::GpuOnly, "manifest.indices"}, idx.data());
    rhi::GraphicsPipelineDesc d;
    d.vs = "manifest.VSMain";
    d.ps = "manifest.PSMain";
    gbuffer::fillFormats(d);
    d.cull = rhi::Cull::None;
    d.name = "manifest";
    pipe_ = device_->createGraphicsPipeline(d, error);
    rhi::GraphicsPipelineDesc f;
    f.vs = "manifest.FireflyVS";
    f.ps = "manifest.FireflyPS";
    f.colorFormats[0] = rhi::Format::RGBA16_FLOAT;
    f.colorCount = 1;
    f.blend[0] = rhi::Blend::Additive;
    f.depthFormat = gbuffer::kDepth;
    f.depthTest = true;
    f.depthWrite = false;
    f.depthCompare = rhi::Compare::GreaterEqual;
    f.name = "fireflies";
    fireflyPipe_ = device_->createGraphicsPipeline(f, error);
    return pipe_ && fireflyPipe_ && vertices_ && indices_;
}

void ManifestRenderer::shutdown() {
    pipe_.reset();
    fireflyPipe_.reset();
    blas_.reset();
    vertices_.reset();
    indices_.reset();
    minerals_.clear();
    sprouts_.clear();
    fireflies_.clear();
    rt_.clear();
}

void ManifestRenderer::update(const MagicField* field, const dvec3& camera) {
    minerals_.clear();
    sprouts_.clear();
    fireflies_.clear();
    rt_.clear();
    if (!field) return;
    for (const Manifestation& m : field->manifestations()) {
        const dvec3 rel = m.position - camera;
        if (dot(rel, rel) > 400.0 * 400.0) continue;
        // Size from its volume (the meshes are about a metre: a mineral, half a metre: a sprout).
        const double scale = m.kind == ManifestKind::Mineral ? std::cbrt(m.volume / 0.12) : std::cbrt(m.volume / 0.04);
        const double yaw = double(m.seed % 6283u) * 0.001;
        InstanceGpu g{};
        g.rel[0] = float(rel.x);
        g.rel[1] = float(rel.y);
        g.rel[2] = float(rel.z);
        g.scale = float(scale);
        g.cosYaw = float(std::cos(yaw));
        g.sinYaw = float(std::sin(yaw));
        g.fill = float(std::clamp(double(m.held) / k::MAGIC_HELD, 0.0, 1.0));
        g.seed = m.seed;
        if (m.kind == ManifestKind::Mineral) {
            minerals_.push_back(g);
            rhi::TlasInstance inst;
            const double c = g.cosYaw * scale, s = g.sinYaw * scale;
            const float xf[12] = {float(c), 0, float(s), g.rel[0], 0, float(scale), 0, g.rel[1], float(-s), 0, float(c), g.rel[2]};
            std::memcpy(inst.transform, xf, sizeof xf);
            inst.customIndex = kMineralInstance;
            inst.forceOpaque = true;
            rt_.push_back(inst);
        } else {
            sprouts_.push_back(g);
        }
    }
    for (const Firefly& f : field->fireflies()) {
        if (fireflies_.size() >= kMaxFireflies) break;
        const dvec3 rel = f.position - camera;
        FireflyGpu g{};
        g.rel[0] = float(rel.x);
        g.rel[1] = float(rel.y);
        g.rel[2] = float(rel.z);
        g.packed = (f.seed & ~1u) | (f.big ? 1u : 0u);
        fireflies_.push_back(g);
    }
}

void ManifestRenderer::draw(rhi::CommandList* cmd, const rhi::Transient& fc) {
    if ((minerals_.empty() && sprouts_.empty()) || !pipe_) return;
    cmd->beginMarker("manifest");
    cmd->setIndexBuffer(indices_.get(), 0, rhi::IndexType::U32);
    cmd->setPipeline(pipe_.get());
    cmd->setFrameConstants(fc);
    for (int kind = 0; kind < 2; ++kind) {
        const std::vector<InstanceGpu>& list = kind == 0 ? minerals_ : sprouts_;
        if (list.empty()) continue;
        rhi::Transient tr = device_->allocTransient(list.size() * sizeof(InstanceGpu));
        std::memcpy(tr.cpu, list.data(), list.size() * sizeof(InstanceGpu));
        const ManifestPush p{vertices_->srv(), tr.buffer->srv(), uint32_t(tr.offset), uint32_t(kind)};
        cmd->pushConstants(&p, sizeof p);
        cmd->drawIndexed(kind == 0 ? crystalIndices_ : sproutIndices_, uint32_t(list.size()), kind == 0 ? 0 : sproutFirst_);
    }
    cmd->endMarker();
}

void ManifestRenderer::drawFireflies(rhi::CommandList* cmd, const rhi::Transient& fc, rhi::Texture* hdr, rhi::Texture* depth) {
    if (fireflies_.empty() || !fireflyPipe_) return;
    cmd->beginMarker("fireflies");
    rhi::Transient tr = device_->allocTransient(fireflies_.size() * sizeof(FireflyGpu));
    std::memcpy(tr.cpu, fireflies_.data(), fireflies_.size() * sizeof(FireflyGpu));
    cmd->barrier(hdr, rhi::State::RenderTarget);
    cmd->barrier(depth, rhi::State::DepthRead);
    rhi::RenderPass rp;
    rp.colorCount = 1;
    rp.color[0].texture = hdr;
    rp.color[0].load = rhi::LoadOp::Load;
    rp.depth.texture = depth;
    rp.depth.load = rhi::LoadOp::Load;
    rp.depth.readOnly = true;
    cmd->beginRenderPass(rp);
    cmd->setPipeline(fireflyPipe_.get());
    cmd->setFrameConstants(fc);
    const ManifestPush p{0, tr.buffer->srv(), uint32_t(tr.offset), 0};
    cmd->pushConstants(&p, sizeof p);
    cmd->draw(6, uint32_t(fireflies_.size()));
    cmd->endRenderPass();
    cmd->endMarker();
}

void ManifestRenderer::buildStructures(rhi::CommandList* cmd) {
    if (blas_ || !device_->rayTracingSupported() || !vertices_) return;
    cmd->barrier(vertices_.get(), rhi::State::ShaderRead);
    cmd->barrier(indices_.get(), rhi::State::ShaderRead);
    rhi::BlasDesc desc;
    desc.geometries.push_back({vertices_.get(), 0, 24, crystalVertices_, indices_.get(), 0, crystalIndices_, rhi::IndexType::U32, true});
    desc.name = "mineral.blas";
    blas_ = device_->createBlas(desc);
    if (blas_) cmd->buildBlas(blas_.get(), desc);
}

void ManifestRenderer::addInstances(std::vector<rhi::TlasInstance>& out) const {
    if (!blas_) return;
    for (rhi::TlasInstance inst : rt_) {
        inst.blas = blas_.get();
        out.push_back(inst);
    }
}

void ManifestRenderer::frameData(gpu::FrameConstants& fc, const MagicField* field, const dvec3& camera) const {
    fc.denseLo = {0, 0, 0, 0};
    fc.denseHi = {0, 0, 0, 0};
    if (!field) return;
    dvec3 lo, hi;
    if (field->denseBox(lo, hi)) {
        fc.denseLo = {float(lo.x - camera.x), float(lo.y - camera.y), float(lo.z - camera.z), 1.0f};
        fc.denseHi = {float(hi.x - camera.x), float(hi.y - camera.y), float(hi.z - camera.z), 1.0f};
    }
}

}  // namespace taelia
