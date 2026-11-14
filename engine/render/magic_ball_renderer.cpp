#include "render/magic_ball_renderer.h"

#include <cstring>

#include "render/gbuffer.h"

namespace taelia {

namespace {

struct BallPush {
    uint32_t pad0, instSrv, instOffset, pad1;
};

}  // namespace

bool MagicBallRenderer::init(rhi::Device* device, std::string* error) {
    device_ = device;
    rhi::GraphicsPipelineDesc d;
    d.vs = "magic_ball.VSMain";
    d.ps = "magic_ball.PSMain";
    d.colorFormats[0] = rhi::Format::RGBA16_FLOAT;
    d.colorCount = 1;
    d.blend[0] = rhi::Blend::Additive;
    d.depthFormat = gbuffer::kDepth;
    d.depthTest = true;
    d.depthWrite = false;
    d.depthCompare = rhi::Compare::GreaterEqual;
    d.cull = rhi::Cull::None;
    d.name = "magic balls";
    pipe_ = device_->createGraphicsPipeline(d, error);
    d.vs = "magic_ball.DustVS";
    d.ps = "magic_ball.DustPS";
    d.blend[0] = rhi::Blend::Alpha;
    d.name = "dust";
    dustPipe_ = device_->createGraphicsPipeline(d, error);
    return pipe_ && dustPipe_;
}

void MagicBallRenderer::shutdown() {
    pipe_.reset();
    dustPipe_.reset();
    instances_.clear();
    dust_.clear();
}

void MagicBallRenderer::update(const std::vector<RenderMagicBall>& balls, const std::vector<RenderDust>& dust, const dvec3& cam) {
    instances_.clear();
    dust_.clear();
    for (const RenderDust& d : dust) {
        const dvec3 rel = d.position - cam;
        InstanceGpu g{};
        g.rel[0] = float(rel.x);
        g.rel[1] = float(rel.y);
        g.rel[2] = float(rel.z);
        g.radius = float(d.radius);
        g.aspect = float(d.age);
        g.seed = float(dust_.size()) * 0.37f;
        dust_.push_back(g);
    }
    for (const RenderMagicBall& b : balls) {
        const dvec3 rel = b.position - cam;
        if (length(rel) > 2000.0) continue;
        InstanceGpu g{};
        g.rel[0] = float(rel.x);
        g.rel[1] = float(rel.y);
        g.rel[2] = float(rel.z);
        g.radius = float(b.radius);
        g.aspect = float(b.aspect);
        g.fill = float(b.fill);
        g.seed = float(b.id % 977u) * 0.173f;
        instances_.push_back(g);
    }
}

void MagicBallRenderer::draw(rhi::CommandList* cmd, const rhi::Transient& fc, rhi::Texture* hdr, rhi::Texture* depth) {
    if ((instances_.empty() && dust_.empty()) || !pipe_ || !dustPipe_) return;
    cmd->beginMarker("magic balls");
    std::vector<InstanceGpu> all = dust_;
    all.insert(all.end(), instances_.begin(), instances_.end());
    rhi::Transient tr = device_->allocTransient(all.size() * sizeof(InstanceGpu));
    std::memcpy(tr.cpu, all.data(), all.size() * sizeof(InstanceGpu));
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
    cmd->setFrameConstants(fc);
    if (!dust_.empty()) {  // dust first (it hides what is behind it), then the glow of the balls
        cmd->setPipeline(dustPipe_.get());
        const BallPush p{0, tr.buffer->srv(), uint32_t(tr.offset), 0};
        cmd->pushConstants(&p, sizeof p);
        cmd->draw(6, uint32_t(dust_.size()));
    }
    if (!instances_.empty()) {
        cmd->setPipeline(pipe_.get());
        const BallPush p{0, tr.buffer->srv(), uint32_t(tr.offset + dust_.size() * sizeof(InstanceGpu)), 0};
        cmd->pushConstants(&p, sizeof p);
        cmd->draw(6, uint32_t(instances_.size()));
    }
    cmd->endRenderPass();
    cmd->endMarker();
}

}  // namespace taelia
