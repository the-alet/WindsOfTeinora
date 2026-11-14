#include "render/ball_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/gbuffer.h"
#include "taelia/shared_constants.h"

namespace taelia {

namespace {

constexpr uint32_t kBallInstance = 0xFFFFFD;  // rt.hlsli BALL_INSTANCE
constexpr double kRtDistance = 1000.0;

struct BallPush {
    uint32_t vbSrv, instSrv, instOffset;
    float radius;
};

}  // namespace

bool BallRenderer::init(rhi::Device* device, int detail, std::string* error) {
    device_ = device;
    // A unit UV sphere (positions are the normals); the shaders scale it by the ball's radius.
    static const int kSlices[4] = {16, 24, 32, 64};
    const int slices = kSlices[std::clamp(detail, 0, 3)], stacks = slices / 2;
    std::vector<float> verts;
    std::vector<uint32_t> idx;
    for (int i = 0; i <= stacks; ++i) {
        const double th = kPi * i / stacks;
        for (int j = 0; j <= slices; ++j) {
            const double ph = 2.0 * kPi * j / slices;
            verts.insert(verts.end(), {float(std::sin(th) * std::cos(ph)), float(std::cos(th)), float(std::sin(th) * std::sin(ph))});
        }
    }
    for (int i = 0; i < stacks; ++i)
        for (int j = 0; j < slices; ++j) {
            const uint32_t a = uint32_t(i * (slices + 1) + j), b = a + 1, c = a + uint32_t(slices) + 1, d = c + 1;
            if (i > 0) idx.insert(idx.end(), {a, c, b});
            if (i < stacks - 1) idx.insert(idx.end(), {b, c, d});
        }
    vertexCount_ = uint32_t(verts.size() / 3);
    indexCount_ = uint32_t(idx.size());
    vertices_ = device_->createBuffer({verts.size() * 4, rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationInput,
                                       rhi::Memory::GpuOnly, "balls.vertices"}, verts.data());
    indices_ = device_->createBuffer({idx.size() * 4, rhi::BufferUsage::Index | rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationInput,
                                      rhi::Memory::GpuOnly, "balls.indices"}, idx.data());
    rhi::GraphicsPipelineDesc d;
    d.vs = "ball.VSMain";
    d.ps = "ball.PSMain";
    gbuffer::fillFormats(d);
    d.cull = rhi::Cull::None;
    d.name = "balls";
    pipe_ = device_->createGraphicsPipeline(d, error);
    prev_.clear();
    havePrevCamera_ = false;
    return pipe_ && vertices_ && indices_;
}

void BallRenderer::shutdown() {
    pipe_.reset();
    blas_.reset();
    vertices_.reset();
    indices_.reset();
    prev_.clear();
    visible_.clear();
    rt_.clear();
}

void BallRenderer::update(const std::vector<BallPose>* poses, const dvec3& cam, const Frustum& f) {
    visible_.clear();
    rt_.clear();
    std::unordered_map<uint32_t, Prev> next;
    const double r = k::BALL_RADIUS;
    const dvec3 prevCam = havePrevCamera_ ? prevCamera_ : cam;
    if (poses)
        for (const BallPose& p : *poses) {
            // A new ball's last pose is its current one.
            auto it = prev_.find(p.id);
            const Prev last = it != prev_.end() ? it->second : Prev{p.position, {p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]}};
            next[p.id] = Prev{p.position, {p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]}};
            const dvec3 rel = p.position - cam, prevRel = last.position - prevCam;
            if (f.boxVisible(rel - dvec3{r, r, r}, rel + dvec3{r, r, r})) {
                InstanceGpu g{};
                g.rel[0] = float(rel.x);
                g.rel[1] = float(rel.y);
                g.rel[2] = float(rel.z);
                std::memcpy(g.rot, p.rotation, sizeof g.rot);
                g.prevRel[0] = float(prevRel.x);
                g.prevRel[1] = float(prevRel.y);
                g.prevRel[2] = float(prevRel.z);
                std::memcpy(g.prevRot, last.rotation, sizeof g.prevRot);
                g.squash[0] = p.squashAxis[0];
                g.squash[1] = p.squashAxis[1];
                g.squash[2] = p.squashAxis[2];
                g.squash[3] = p.squash;
                visible_.push_back(g);
            }
            if (length(rel) < kRtDistance) {
                // Rotation (quaternion x, y, z, w) times the radius, then the camera-relative centre.
                const double x = p.rotation[0], y = p.rotation[1], z = p.rotation[2], w = p.rotation[3];
                const double m[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                                        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                                        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
                const double t[3] = {rel.x, rel.y, rel.z};
                rhi::TlasInstance inst;
                for (int row = 0; row < 3; ++row) {
                    for (int c = 0; c < 3; ++c) inst.transform[row * 4 + c] = float(m[row][c] * r);
                    inst.transform[row * 4 + 3] = float(t[row]);
                }
                inst.customIndex = kBallInstance;
                inst.forceOpaque = true;
                rt_.push_back(inst);
            }
        }
    prev_ = std::move(next);
    prevCamera_ = cam;
    havePrevCamera_ = true;
}

void BallRenderer::draw(rhi::CommandList* cmd, const rhi::Transient& fc) {
    if (visible_.empty() || !pipe_) return;
    cmd->beginMarker("balls");
    rhi::Transient tr = device_->allocTransient(visible_.size() * sizeof(InstanceGpu));
    std::memcpy(tr.cpu, visible_.data(), visible_.size() * sizeof(InstanceGpu));
    cmd->setIndexBuffer(indices_.get(), 0, rhi::IndexType::U32);
    cmd->setPipeline(pipe_.get());
    cmd->setFrameConstants(fc);
    BallPush p{vertices_->srv(), tr.buffer->srv(), uint32_t(tr.offset), float(k::BALL_RADIUS)};
    cmd->pushConstants(&p, sizeof p);
    cmd->drawIndexed(indexCount_, uint32_t(visible_.size()), 0);
    cmd->endMarker();
}

void BallRenderer::buildStructures(rhi::CommandList* cmd) {
    if (blas_ || !device_->rayTracingSupported() || !vertices_) return;
    cmd->barrier(vertices_.get(), rhi::State::ShaderRead);
    cmd->barrier(indices_.get(), rhi::State::ShaderRead);
    rhi::BlasDesc desc;
    desc.geometries.push_back({vertices_.get(), 0, 12, vertexCount_, indices_.get(), 0, indexCount_, rhi::IndexType::U32, true});
    desc.name = "ball.blas";
    blas_ = device_->createBlas(desc);
    if (blas_) cmd->buildBlas(blas_.get(), desc);
}

void BallRenderer::addInstances(std::vector<rhi::TlasInstance>& out) const {
    if (!blas_) return;
    for (rhi::TlasInstance inst : rt_) {
        inst.blas = blas_.get();
        out.push_back(inst);
    }
}

}  // namespace taelia
