#include "render/tree_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/gbuffer.h"
#include "taelia/sim/hash.h"
#include "taelia/sim/terrain.h"

namespace taelia {

namespace {

constexpr double kChunk = kTreeCell * 16.0;  // metres per forest chunk

using TreeVertex = TreeVertexOut;
static_assert(sizeof(TreeVertex) == 32, "tree.hlsl reads 32-byte vertices");

uint32_t octNormal(double x, double y, double z) {
    const double l = std::sqrt(x * x + y * y + z * z);
    x /= l;
    y /= l;
    z /= l;
    const double s = std::fabs(x) + std::fabs(y) + std::fabs(z);
    double u = x / s, v = z / s;
    if (y < 0) {
        const double ou = (1.0 - std::fabs(v)) * (u >= 0 ? 1.0 : -1.0), ov = (1.0 - std::fabs(u)) * (v >= 0 ? 1.0 : -1.0);
        u = ou;
        v = ov;
    }
    auto q = [](double a) { return uint32_t(std::lround(std::clamp(a * 0.5 + 0.5, 0.0, 1.0) * 65535.0)); };
    return q(u) | (q(v) << 16);
}

struct TreePush {
    uint32_t vbSrv, instSrv, instOffset, kind;
    uint32_t albedo, detail;
    float tint, uvScale;  // albedo multiplier (charred wood), texture repeats
};

}  // namespace

TreeRenderer::~TreeRenderer() { shutdown(); }

uint64_t TreeRenderer::chunkKey(int64_t cx, int64_t cz) { return (uint64_t(cx) << 32) ^ (uint64_t(cz) & 0xffffffffull); }

void TreeRenderer::buildMeshes() {
    std::vector<TreeVertex> verts;
    std::vector<uint32_t> idx;
    meshes_.assign(size_t(TreeKind::Count) * kTreeVariants, Mesh{});
    for (int k = 0; k < int(TreeKind::Count); ++k)
        for (int var = 0; var < kTreeVariants; ++var) {
            const TreeSkeleton sk = growTree(TreeKind(k), uint64_t(var) * 977 + uint64_t(k) * 13 + 7);
            Mesh& m = meshes_[size_t(k) * kTreeVariants + var];
            m.height = sk.height;
            m.radius = std::max(sk.crownRadius, sk.height * 0.25f);
            // Bark coordinate along each limb: continue from the parent when the limb continues it.
            std::vector<float> vEnd(sk.segments.size(), 0.0f);
            for (int lod = 0; lod < kLods; ++lod) {
                m.bark[lod].firstIndex = uint32_t(idx.size());
                for (size_t si = 0; si < sk.segments.size(); ++si) {
                    const TreeSegment& s = sk.segments[si];
                    if (int(s.order) > 2 - lod) continue;
                    const int sides = lod == 0 ? (s.order == 0 ? 10 : (s.order == 1 ? 6 : 4)) : (s.order == 0 ? 6 : 3);
                    const dvec3 a{s.a.x, s.a.y, s.a.z}, b{s.b.x, s.b.y, s.b.z};
                    const dvec3 axis = normalize(b - a);
                    const dvec3 side0 = normalize(cross(axis, std::fabs(axis.y) < 0.95 ? dvec3{0, 1, 0} : dvec3{1, 0, 0}));
                    const dvec3 side1 = cross(axis, side0);
                    const float len = float(length(b - a));
                    float v0 = 0;
                    if (s.parent >= 0) {
                        const TreeSegment& p = sk.segments[size_t(s.parent)];
                        if (p.b.x == s.a.x && p.b.y == s.a.y && p.b.z == s.a.z) v0 = vEnd[size_t(s.parent)];
                    }
                    const float v1 = v0 + len / 1.6f;
                    vEnd[si] = v1;
                    const uint32_t base = uint32_t(verts.size());
                    for (int ring = 0; ring < 2; ++ring) {
                        const dvec3 c = ring ? b : a;
                        const double r = ring ? s.rb : s.ra;
                        const double sway = std::clamp(c.y / std::max(sk.height, 1.0f), 0.0, 1.0);
                        const uint32_t w = uint32_t(std::lround(sway * sway * (s.order == 0 ? 0.25 : 1.0) * 255.0));
                        for (int i = 0; i <= sides; ++i) {
                            const double ang = 2.0 * kPi * double(i) / sides;
                            const dvec3 n = side0 * std::cos(ang) + side1 * std::sin(ang);
                            const dvec3 p = c + n * r;
                            TreeVertex tv{};
                            tv.pos[0] = float(p.x);
                            tv.pos[1] = float(p.y);
                            tv.pos[2] = float(p.z);
                            tv.normal = octNormal(n.x, n.y, n.z);
                            tv.uv[0] = float(i) / sides * std::max(1.0f, std::round(float(r) * 2.0f * 3.14159f / 0.35f));
                            tv.uv[1] = ring ? v1 : v0;
                            tv.info = w;
                            verts.push_back(tv);
                        }
                    }
                    for (int i = 0; i < sides; ++i) {
                        const uint32_t a0 = base + uint32_t(i), a1 = a0 + 1, b0 = a0 + uint32_t(sides) + 1, b1 = b0 + 1;
                        idx.insert(idx.end(), {a0, b0, b1, a0, b1, a1});
                    }
                }
                m.bark[lod].indexCount = uint32_t(idx.size()) - m.bark[lod].firstIndex;

                // Leaf cards: fewer and bigger further away, so the crown keeps its volume.
                m.leaves[lod].firstIndex = uint32_t(idx.size());
                const int stride = lod == 0 ? 1 : (lod == 1 ? 2 : 6);
                const float grow = lod == 0 ? 1.0f : (lod == 1 ? 1.4f : 2.4f);
                for (size_t li = 0; li < sk.leaves.size(); li += size_t(stride)) {
                    const LeafCard& c = sk.leaves[li];
                    const dvec3 ctr{c.center.x, c.center.y, c.center.z}, n{c.normal.x, c.normal.y, c.normal.z}, up{c.up.x, c.up.y, c.up.z};
                    const dvec3 right = normalize(cross(up, n)), u2 = normalize(cross(n, right));
                    const double h = c.size * grow * 0.5;
                    const double sway = std::clamp(ctr.y / std::max(sk.height, 1.0f), 0.0, 1.0);
                    const uint32_t w = uint32_t(std::lround((0.4 + 0.6 * sway) * 255.0)) | 0x100u;
                    const uint32_t base = uint32_t(verts.size());
                    const double corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                    for (const auto& cc : corners) {
                        const dvec3 p = ctr + right * (cc[0] * h) + u2 * (cc[1] * h);
                        TreeVertex tv{};
                        tv.pos[0] = float(p.x);
                        tv.pos[1] = float(p.y);
                        tv.pos[2] = float(p.z);
                        tv.normal = octNormal(n.x, n.y, n.z);
                        tv.uv[0] = float(cc[0] * 0.5 + 0.5);
                        tv.uv[1] = float(0.5 - cc[1] * 0.5);
                        tv.info = w;
                        verts.push_back(tv);
                    }
                    idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
                }
                m.leaves[lod].indexCount = uint32_t(idx.size()) - m.leaves[lod].firstIndex;
            }
        }
    buildCampfire(verts, idx);
    vertexCount_ = uint32_t(verts.size());
    vertices_ = device_->createBuffer({verts.size() * sizeof(TreeVertex), rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationInput,
                                       rhi::Memory::GpuOnly, "trees.vertices"}, verts.data());
    indices_ = device_->createBuffer({idx.size() * 4, rhi::BufferUsage::Index | rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationInput,
                                      rhi::Memory::GpuOnly, "trees.indices"}, idx.data());
}

bool TreeRenderer::init(rhi::Device* device, const Terrain* terrain, int detail, std::string* error) {
    device_ = device;
    terrain_ = terrain;
    static const double kScale[4] = {0.6, 0.8, 1.0, 1.3};
    const double s = kScale[std::clamp(detail, 0, 3)];
    lodDistance_[0] = 120 * s;
    lodDistance_[1] = 450 * s;
    lodDistance_[2] = 1500 * s;
    buildMeshes();
    rhi::GraphicsPipelineDesc d;
    d.vs = "tree.VSMain";
    d.ps = "tree.BarkPS";
    gbuffer::fillFormats(d);
    d.cull = rhi::Cull::Back;
    d.name = "trees.bark";
    barkPipe_ = device_->createGraphicsPipeline(d, error);
    d.ps = "tree.LeafPS";
    d.cull = rhi::Cull::None;
    d.name = "trees.leaves";
    leafPipe_ = device_->createGraphicsPipeline(d, error);
    if (!barkPipe_ || !leafPipe_ || !vertices_ || !indices_) return false;
    buckets_.assign(meshes_.size() * kLods, {});
    quit_ = false;
    for (int i = 0; i < 3; ++i) workers_.emplace_back([this] { workerLoop(); });
    return true;
}

void TreeRenderer::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    for (auto& t : workers_) t.join();
    workers_.clear();
    chunks_.clear();
    results_.clear();
    barkPipe_.reset();
    blas_.clear();
    fireBlas_.reset();
    alphaTable_.reset();
    leafPipe_.reset();
    vertices_.reset();
    indices_.reset();
}

void TreeRenderer::workerLoop() {
    for (;;) {
        uint64_t key;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            key = jobs_.front();
            jobs_.pop_front();
            ++busy_;
        }
        const int64_t cx = int64_t(int32_t(key >> 32)), cz = int64_t(int32_t(key & 0xffffffffull));
        std::vector<TreeInstance> trees;
        treesInArea(*terrain_, double(cx) * kChunk, double(cz) * kChunk, double(cx + 1) * kChunk, double(cz + 1) * kChunk, trees);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            results_.emplace_back(key, std::move(trees));
            --busy_;
        }
        done_.notify_all();
    }
}

void TreeRenderer::select(const dvec3& cam, const Frustum& f, bool wait) {
    ++frame_;
    const double reach = lodDistance_[kLods - 1];
    const int64_t x0 = int64_t(std::floor((cam.x - reach) / kChunk)), x1 = int64_t(std::floor((cam.x + reach) / kChunk));
    const int64_t z0 = int64_t(std::floor((cam.z - reach) / kChunk)), z1 = int64_t(std::floor((cam.z + reach) / kChunk));
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [key, trees] : results_) {
                auto it = chunks_.find(key);
                if (it == chunks_.end()) continue;
                it->second.trees = std::move(trees);
                it->second.ready = true;
            }
            results_.clear();
        }
        int missing = 0;
        for (auto& b : buckets_) b.clear();
        rtTrees_.clear();
        drawn_ = 0;
        for (int64_t cz = z0; cz <= z1; ++cz)
            for (int64_t cx = x0; cx <= x1; ++cx) {
                const double dx = std::max({double(cx) * kChunk - cam.x, 0.0, cam.x - double(cx + 1) * kChunk});
                const double dz = std::max({double(cz) * kChunk - cam.z, 0.0, cam.z - double(cz + 1) * kChunk});
                if (dx * dx + dz * dz > reach * reach) continue;
                const uint64_t key = chunkKey(cx, cz);
                auto [it, inserted] = chunks_.try_emplace(key);
                if (inserted) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        jobs_.push_back(key);
                    }
                    wake_.notify_one();
                }
                Chunk& c = it->second;
                c.lastUsed = frame_;
                if (!c.ready) {
                    ++missing;
                    continue;
                }
                for (const TreeInstance& t : c.trees) {
                    const size_t mesh = size_t(t.kind) * kTreeVariants + t.variant;
                    const Mesh& m = meshes_[mesh];
                    const dvec3 rel = t.position - cam;
                    const double h = m.height * t.scale, r = m.radius * t.scale;
                    const double dist = length(rel + dvec3{0, h * 0.5, 0});
                    if (dist > reach) continue;
                    if (!f.boxVisible(rel - dvec3{r, 0, r}, rel + dvec3{r, h, r})) continue;
                    int lod = 0;
                    while (lod < kLods - 1 && dist > lodDistance_[lod]) ++lod;
                    InstanceGpu g{};
                    g.posScale[0] = float(rel.x);
                    g.posScale[1] = float(rel.y);
                    g.posScale[2] = float(rel.z);
                    g.posScale[3] = t.scale;
                    g.rot[0] = std::cos(t.yaw);
                    g.rot[1] = std::sin(t.yaw);
                    g.rot[2] = float(std::fmod(t.position.x * 0.37 + t.position.z * 0.23, 6.2831853));
                    buckets_[mesh * kLods + size_t(lod)].push_back(g);
                    if (dist < rtDistance_) {
                        RtTree rt{};
                        const float rc = std::cos(t.yaw) * t.scale, rs = std::sin(t.yaw) * t.scale;
                        const float xf[12] = {rc, 0, rs, float(rel.x), 0, t.scale, 0, float(rel.y), -rs, 0, rc, float(rel.z)};
                        std::memcpy(rt.transform, xf, sizeof xf);
                        rt.entry = uint32_t(mesh * kLods + size_t(lod));
                        rtTrees_.push_back(rt);
                    }
                    ++drawn_;
                }
            }
        {
            // The campfire by the spawn point.
            const dvec3 rel = terrain_->campfirePosition() - cam;
            fireVisible_ = length(rel) < 400.0 && f.boxVisible(rel - dvec3{1, 0.5, 1}, rel + dvec3{1, 1, 1});
            const float inst[8] = {float(rel.x), float(rel.y), float(rel.z), 1.0f, std::cos(0.3f), std::sin(0.3f), 0, 0};
            std::memcpy(fireInstance_, inst, sizeof inst);
            const float xf[12] = {inst[4], 0, inst[5], inst[0], 0, 1, 0, inst[1], -inst[5], 0, inst[4], inst[2]};
            std::memcpy(fireTransform_, xf, sizeof xf);
        }
        if (!wait || missing == 0) break;
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return !results_.empty() || (jobs_.empty() && busy_ == 0); });
    }
    if (frame_ % 120 == 0)
        for (auto it = chunks_.begin(); it != chunks_.end();)
            it = (it->second.ready && frame_ - it->second.lastUsed > 600) ? chunks_.erase(it) : std::next(it);
}

void TreeRenderer::draw(rhi::CommandList* cmd, const rhi::Transient& fc, const uint32_t barkTex[2][3], const uint32_t leafTex[2][3]) {
    if (drawn_ == 0 && !fireVisible_) return;
    cmd->beginMarker("trees");
    cmd->setIndexBuffer(indices_.get(), 0, rhi::IndexType::U32);
    for (int pass = 0; pass < 2; ++pass) {
        cmd->setPipeline(pass == 0 ? barkPipe_.get() : leafPipe_.get());
        cmd->setFrameConstants(fc);
        for (size_t mesh = 0; mesh < meshes_.size(); ++mesh)
            for (int lod = 0; lod < kLods; ++lod) {
                const auto& inst = buckets_[mesh * kLods + size_t(lod)];
                if (inst.empty()) continue;
                const MeshPart& part = pass == 0 ? meshes_[mesh].bark[lod] : meshes_[mesh].leaves[lod];
                if (part.indexCount == 0) continue;
                rhi::Transient tr = device_->allocTransient(inst.size() * sizeof(InstanceGpu));
                std::memcpy(tr.cpu, inst.data(), inst.size() * sizeof(InstanceGpu));
                const uint32_t kind = uint32_t(mesh / kTreeVariants);
                TreePush p{vertices_->srv(), tr.buffer->srv(), uint32_t(tr.offset), kind,
                           pass == 0 ? barkTex[0][kind] : leafTex[0][kind], pass == 0 ? barkTex[1][kind] : leafTex[1][kind], 1.0f, 1.0f};
                cmd->pushConstants(&p, sizeof p);
                cmd->drawIndexed(part.indexCount, uint32_t(inst.size()), part.firstIndex);
            }
    }
    if (fireVisible_) {
        // The campfire: rock-textured stones, charred logs (bark of the broad species, darkened).
        cmd->setPipeline(barkPipe_.get());
        cmd->setFrameConstants(fc);
        rhi::Transient tr = device_->allocTransient(sizeof fireInstance_);
        std::memcpy(tr.cpu, fireInstance_, sizeof fireInstance_);
        TreePush stones{vertices_->srv(), tr.buffer->srv(), uint32_t(tr.offset), 0, rockTex_[0], rockTex_[1], 1.0f, 2.0f};
        cmd->pushConstants(&stones, sizeof stones);
        cmd->drawIndexed(fireStones_.indexCount, 1, fireStones_.firstIndex);
        TreePush logs{vertices_->srv(), tr.buffer->srv(), uint32_t(tr.offset), 1, barkTex[0][1], barkTex[1][1], 0.55f, 1.0f};
        cmd->pushConstants(&logs, sizeof logs);
        cmd->drawIndexed(fireLogs_.indexCount, 1, fireLogs_.firstIndex);
    }
    cmd->endMarker();
}

}  // namespace taelia

namespace taelia {

void TreeRenderer::buildStructures(rhi::CommandList* cmd, const uint32_t leafAlbedo[3]) {
    if (!device_->rayTracingSupported() || !blas_.empty()) return;
    cmd->barrier(vertices_.get(), rhi::State::ShaderRead);
    cmd->barrier(indices_.get(), rhi::State::ShaderRead);
    std::vector<uint32_t> table;
    blas_.resize(meshes_.size() * kLods);
    for (size_t mesh = 0; mesh < meshes_.size(); ++mesh)
        for (int lod = 0; lod < kLods; ++lod) {
            const Mesh& m = meshes_[mesh];
            rhi::BlasDesc desc;
            // Geometry 0: bark (opaque); geometry 1: leaves (alpha-tested by the shaders).
            desc.geometries.push_back({vertices_.get(), 0, 32, vertexCount_, indices_.get(), uint64_t(m.bark[lod].firstIndex) * 4,
                                       m.bark[lod].indexCount, rhi::IndexType::U32, true});
            desc.geometries.push_back({vertices_.get(), 0, 32, vertexCount_, indices_.get(), uint64_t(m.leaves[lod].firstIndex) * 4,
                                       m.leaves[lod].indexCount, rhi::IndexType::U32, false});
            desc.name = "tree.blas";
            auto& b = blas_[mesh * kLods + size_t(lod)];
            b = device_->createBlas(desc);
            if (b) cmd->buildBlas(b.get(), desc);
            table.push_back(m.leaves[lod].firstIndex);
            table.push_back(leafAlbedo[mesh / kTreeVariants]);
        }
    alphaTable_ = device_->createBuffer({table.size() * 4, rhi::BufferUsage::Storage, rhi::Memory::GpuOnly, "trees.alpha"}, table.data());
    rhi::BlasDesc fire;
    fire.geometries.push_back({vertices_.get(), 0, 32, vertexCount_, indices_.get(), uint64_t(fireStones_.firstIndex) * 4, fireStones_.indexCount, rhi::IndexType::U32, true});
    fire.geometries.push_back({vertices_.get(), 0, 32, vertexCount_, indices_.get(), uint64_t(fireLogs_.firstIndex) * 4, fireLogs_.indexCount, rhi::IndexType::U32, true});
    fire.name = "campfire.blas";
    fireBlas_ = device_->createBlas(fire);
    if (fireBlas_) cmd->buildBlas(fireBlas_.get(), fire);
}

void TreeRenderer::addInstances(std::vector<rhi::TlasInstance>& out, double) const {
    if (blas_.empty()) return;
    if (fireVisible_ && fireBlas_) {
        rhi::TlasInstance inst;
        std::memcpy(inst.transform, fireTransform_, sizeof inst.transform);
        inst.customIndex = 0xFFFFFE;  // opaque prop
        inst.forceOpaque = true;
        inst.blas = fireBlas_.get();
        out.push_back(inst);
    }
    for (const RtTree& t : rtTrees_) {
        if (!blas_[t.entry]) continue;
        rhi::TlasInstance inst;
        std::memcpy(inst.transform, t.transform, sizeof inst.transform);
        inst.customIndex = t.entry;
        inst.doubleSided = true;
        inst.blas = blas_[t.entry].get();
        out.push_back(inst);
    }
}

}  // namespace taelia

namespace taelia {

void TreeRenderer::buildCampfire(std::vector<TreeVertexOut>& verts, std::vector<uint32_t>& idx) {
    auto vert = [&](const dvec3& p, const dvec3& n, double u, double v) {
        TreeVertexOut tv{};
        tv.pos[0] = float(p.x);
        tv.pos[1] = float(p.y);
        tv.pos[2] = float(p.z);
        tv.normal = octNormal(n.x, n.y, n.z);
        tv.uv[0] = float(u);
        tv.uv[1] = float(v);
        verts.push_back(tv);
    };
    auto grid = [&](uint32_t base, int rows, int cols) {
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < cols; ++j) {
                const uint32_t a = base + uint32_t(i * (cols + 1) + j), b = a + 1, c = a + uint32_t(cols) + 1, d = c + 1;
                idx.insert(idx.end(), {a, c, d, a, d, b});
            }
    };
    // A ring of rounded stones, half sunk in the trodden ground.
    fireStones_.firstIndex = uint32_t(idx.size());
    for (int s = 0; s < 9; ++s) {
        const uint64_t h = mix64(0xF1E5ull + uint64_t(s));
        const double a = s * 2.0 * kPi / 9.0 + (hashToUnit(h) - 0.5) * 0.3;
        const dvec3 c{std::cos(a) * 0.58, 0.03, std::sin(a) * 0.58};
        const double r = 0.15 + 0.07 * hashToUnit(mix64(h + 1));
        const int rings = 6, segs = 9;
        const uint32_t base = uint32_t(verts.size());
        for (int i = 0; i <= rings; ++i)
            for (int j = 0; j <= segs; ++j) {
                const double th = kPi * i / rings, ph = 2.0 * kPi * j / segs;
                const dvec3 n{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
                const double bump = 1.0 + 0.2 * (hashToUnit(mix64(h + 7 + uint64_t(i * 31 + j % segs))) - 0.5);
                vert(c + dvec3{n.x * r * 1.25 * bump, n.y * r * 0.7 * bump, n.z * r * bump}, n, double(j) / segs, double(i) / rings);
            }
        grid(base, rings, segs);
    }
    fireStones_.indexCount = uint32_t(idx.size()) - fireStones_.firstIndex;

    // Logs: a teepee of five, two more lying across.
    fireLogs_.firstIndex = uint32_t(idx.size());
    auto log = [&](const dvec3& a, const dvec3& b, double r) {
        const dvec3 axis = normalize(b - a);
        const dvec3 s0 = normalize(cross(axis, std::fabs(axis.y) < 0.95 ? dvec3{0, 1, 0} : dvec3{1, 0, 0})), s1 = cross(axis, s0);
        const int sides = 8;
        const uint32_t base = uint32_t(verts.size());
        const double len = length(b - a);
        for (int ring = 0; ring < 2; ++ring)
            for (int i = 0; i <= sides; ++i) {
                const double ang = 2.0 * kPi * i / sides;
                const dvec3 n = s0 * std::cos(ang) + s1 * std::sin(ang);
                vert((ring ? b : a) + n * r, n, double(i) / sides * 2.0, ring ? len / 0.5 : 0.0);
            }
        grid(base, 1, sides);
    };
    for (int k = 0; k < 5; ++k) {
        const double a = k * 2.0 * kPi / 5.0 + 0.4;
        log({std::cos(a) * 0.42, 0.03, std::sin(a) * 0.42}, {std::cos(a) * 0.05, 0.62, std::sin(a) * 0.05}, 0.05);
    }
    log({-0.48, 0.07, -0.12}, {0.46, 0.09, 0.14}, 0.07);
    log({-0.12, 0.08, -0.47}, {0.14, 0.1, 0.46}, 0.065);
    fireLogs_.indexCount = uint32_t(idx.size()) - fireLogs_.firstIndex;
}

}  // namespace taelia
