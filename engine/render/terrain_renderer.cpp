#include "render/terrain_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gpu_types.h"
#include "render/gbuffer.h"
#include "taelia/shared_constants.h"
#include "taelia/sim/terrain.h"

namespace taelia {

namespace {

constexpr int kGrid = k::TERRAIN_GRID;
constexpr int kVerts = kGrid + 1;
constexpr uint32_t kTileBytes = uint32_t(kVerts * kVerts * k::TERRAIN_VERTEX_BYTES);
constexpr uint32_t kPoolSlots = 2048;

struct TerrainPush {
    uint32_t nodesSrv, nodesOffset, poolSrv, pad;
    uint32_t albedo[4];   // grass, dirt, rock, sand
    uint32_t normal[4];
};

uint32_t packUnorm16x2(double a, double b) {
    auto q = [](double v) { return uint32_t(std::lround(std::clamp(v, 0.0, 1.0) * 65535.0)); };
    return q(a) | (q(b) << 16);
}

// Octahedral encoding of a unit normal into [0, 1]^2.
uint32_t packNormal(const dvec3& n) {
    const double s = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    double x = n.x / s, z = n.z / s;
    if (n.y < 0) {
        const double ox = (1.0 - std::fabs(z)) * (x >= 0 ? 1.0 : -1.0);
        const double oz = (1.0 - std::fabs(x)) * (z >= 0 ? 1.0 : -1.0);
        x = ox;
        z = oz;
    }
    return packUnorm16x2(x * 0.5 + 0.5, z * 0.5 + 0.5);
}

uint32_t packUnorm8x4(float a, float b, float c, float d) {
    auto q = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return q(a) | (q(b) << 8) | (q(c) << 16) | (q(d) << 24);
}

uint32_t floatBits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

}  // namespace

bool Frustum::boxVisible(const dvec3& lo, const dvec3& hi) const {
    for (const auto& p : planes) {
        const double x = p[0] >= 0 ? hi.x : lo.x, y = p[1] >= 0 ? hi.y : lo.y, z = p[2] >= 0 ? hi.z : lo.z;
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0) return false;
    }
    return true;
}

TerrainRenderer::~TerrainRenderer() { shutdown(); }

uint64_t TerrainRenderer::makeKey(int level, int64_t ix, int64_t iz) {
    return (uint64_t(level) << 60) | ((uint64_t(ix) & 0x3fffffffull) << 30) | (uint64_t(iz) & 0x3fffffffull);
}

bool TerrainRenderer::init(rhi::Device* device, const Terrain* terrain, int detail, std::string* error) {
    device_ = device;
    terrain_ = terrain;
    static const double kLeaf[4] = {32, 32, 32, 16}, kRange[4] = {1.8, 2.2, 2.6, 3.0};
    detail = std::clamp(detail, 0, 3);
    leafSize_ = kLeaf[detail];
    rangeFactor_ = kRange[detail];
    levels_ = int(std::lround(std::log2(k::TERRAIN_ROOT_SIZE / leafSize_))) + 1;

    poolSlots_ = kPoolSlots;
    pool_ = device_->createBuffer({uint64_t(kTileBytes) * poolSlots_, rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst,
                                   rhi::Memory::GpuOnly, "terrain.pool"});
    freeSlots_.clear();
    for (int s = int(poolSlots_) - 1; s >= 0; --s) freeSlots_.push_back(s);

    std::vector<uint16_t> idx;
    idx.reserve(kGrid * kGrid * 6);
    for (int z = 0; z < kGrid; ++z)
        for (int x = 0; x < kGrid; ++x) {
            const uint16_t a = uint16_t(z * kVerts + x), b = uint16_t(a + 1), c = uint16_t(a + kVerts), d = uint16_t(c + 1);
            // Every quad split along the same diagonal, so a fully morphed tile matches its parent's triangles.
            idx.insert(idx.end(), {a, c, d, a, d, b});
        }
    rayTracing_ = device_->rayTracingSupported();
    const uint32_t idxUsage = rhi::BufferUsage::Index | (rayTracing_ ? rhi::BufferUsage::AccelerationInput | rhi::BufferUsage::Storage : 0u);
    indices_ = device_->createBuffer({idx.size() * 2, idxUsage, rhi::Memory::GpuOnly, "terrain.indices"}, idx.data());
    if (rayTracing_) {
        positions_ = device_->createBuffer({uint64_t(poolSlots_) * kVerts * kVerts * 12,
                                            rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationInput | rhi::BufferUsage::CopyDst,
                                            rhi::Memory::GpuOnly, "terrain.positions"});
        slotBlas_.clear();
        slotBlas_.resize(poolSlots_);
    }

    rhi::GraphicsPipelineDesc d;
    d.vs = "terrain.VSMain";
    d.ps = "terrain.PSMain";
    gbuffer::fillFormats(d);
    d.cull = rhi::Cull::Back;
    d.depthTest = d.depthWrite = true;
    d.name = "terrain";
    pipeline_ = device_->createGraphicsPipeline(d, error);
    if (!pipeline_ || !pool_ || !indices_) return false;

    quit_ = false;
    const unsigned n = std::max(2u, std::thread::hardware_concurrency() > 4 ? std::thread::hardware_concurrency() - 2 : 2u);
    for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this] { workerLoop(); });
    return true;
}

void TerrainRenderer::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    for (auto& t : workers_) t.join();
    workers_.clear();
    tiles_.clear();
    results_.clear();
    pipeline_.reset();
    slotBlas_.clear();
    positions_.reset();
    indices_.reset();
    pool_.reset();
}

void TerrainRenderer::workerLoop() {
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
        Tile t;
        generate(key, t.data, t.minH, t.maxH);
        t.generated = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            results_.emplace_back(key, std::move(t));
            --busy_;
        }
        done_.notify_all();
    }
}

void TerrainRenderer::generate(uint64_t key, std::vector<uint32_t>& out, float& minH, float& maxH) const {
    const int level = int(key >> 60);
    auto sext = [](uint64_t v) { return int64_t(v << 34) >> 34; };  // 30-bit signed
    const int64_t ix = sext((key >> 30) & 0x3fffffffull), iz = sext(key & 0x3fffffffull);
    const double size = tileSize(level), spacing = size / kGrid;
    const double ox = double(ix) * size, oz = double(iz) * size;
    constexpr int B = kVerts + 2;  // one sample of border for the normals
    std::vector<double> h(B * B);
    for (int j = 0; j < B; ++j)
        for (int i = 0; i < B; ++i) h[j * B + i] = terrain_->height(ox + (i - 1) * spacing, oz + (j - 1) * spacing);

    out.assign(kVerts * kVerts * 4, 0);
    minH = 1e30f;
    maxH = -1e30f;
    for (int j = 0; j < kVerts; ++j)
        for (int i = 0; i < kVerts; ++i) {
            const double hc = h[(j + 1) * B + i + 1];
            const double dx = (h[(j + 1) * B + i + 2] - h[(j + 1) * B + i]) / (2.0 * spacing);
            const double dz = (h[(j + 2) * B + i + 1] - h[j * B + i + 1]) / (2.0 * spacing);
            const dvec3 n = normalize(dvec3{-dx, 1.0, -dz});
            const double x = ox + i * spacing, z = oz + j * spacing;
            const GroundMaterials m = terrain_->materials(x, z, hc, n);
            const bool wet = terrain_->waterLevelAt(x, z, hc) > hc;
            uint32_t* v = &out[(j * kVerts + i) * 4];
            v[0] = floatBits(float(hc));
            v[1] = packNormal(n);
            v[2] = packUnorm8x4(m.grass, m.dirt, m.rock, m.sand);
            v[3] = packUnorm8x4(m.meadow, wet ? 1.0f : 0.0f, 0, 0);
            minH = std::min(minH, float(hc));
            maxH = std::max(maxH, float(hc));
        }
    minH -= 0.5f;
    maxH += 0.5f;
}

TerrainRenderer::Tile* TerrainRenderer::request(int level, int64_t ix, int64_t iz) {
    const uint64_t key = makeKey(level, ix, iz);
    auto [it, inserted] = tiles_.try_emplace(key);
    if (inserted) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(key);
        }
        wake_.notify_one();
    }
    it->second.lastUsed = frame_;
    return &it->second;
}

void TerrainRenderer::selectNode(int level, int64_t ix, int64_t iz, const dvec3& cam, const Frustum& f, float pMin, float pMax) {
    Tile* t = request(level, ix, iz);
    const double size = tileSize(level);
    const float tMin = t->generated ? t->minH : pMin, tMax = t->generated ? t->maxH : pMax;
    const dvec3 lo{double(ix) * size - cam.x, double(tMin) - cam.y, double(iz) * size - cam.z};
    const dvec3 hi{lo.x + size, double(tMax) - cam.y, lo.z + size};
    if (!f.boxVisible(lo, hi)) return;
    auto sphereHits = [&](double r) {
        const double cx = std::clamp(0.0, lo.x, hi.x), cy = std::clamp(0.0, lo.y, hi.y), cz = std::clamp(0.0, lo.z, hi.z);
        return cx * cx + cy * cy + cz * cz < r * r;
    };
    if (level > 0 && sphereHits(range(level - 1))) {
        bool ready = true;
        for (int c = 0; c < 4; ++c) ready &= request(level - 1, ix * 2 + (c & 1), iz * 2 + (c >> 1))->generated;
        if (ready) {
            for (int c = 0; c < 4; ++c) selectNode(level - 1, ix * 2 + (c & 1), iz * 2 + (c >> 1), cam, f, tMin, tMax);
            return;
        }
        ++missing_;  // finer tiles still on their way: draw this one meanwhile
    }
    if (t->generated) drawList_.push_back({makeKey(level, ix, iz), level, ix, iz});
    else ++missing_;
}

void TerrainRenderer::select(const dvec3& cam, const Frustum& f, bool wait) {
    ++frame_;
    camera_ = cam;
    const int root = levels_ - 1;
    const double size = tileSize(root), reach = range(root);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [key, t] : results_) {
                auto it = tiles_.find(key);
                if (it == tiles_.end()) continue;  // dropped meanwhile
                it->second.data = std::move(t.data);
                it->second.minH = t.minH;
                it->second.maxH = t.maxH;
                it->second.generated = true;
            }
            results_.clear();
        }
        drawList_.clear();
        missing_ = 0;
        const int64_t x0 = int64_t(std::floor((cam.x - reach) / size)), x1 = int64_t(std::floor((cam.x + reach) / size));
        const int64_t z0 = int64_t(std::floor((cam.z - reach) / size)), z1 = int64_t(std::floor((cam.z + reach) / size));
        for (int64_t iz = z0; iz <= z1; ++iz)
            for (int64_t ix = x0; ix <= x1; ++ix) {
                const double dx = std::max({double(ix) * size - cam.x, 0.0, cam.x - double(ix + 1) * size});
                const double dz = std::max({double(iz) * size - cam.z, 0.0, cam.z - double(iz + 1) * size});
                if (dx * dx + dz * dz < reach * reach) selectNode(root, ix, iz, cam, f, -400.0f, 1500.0f);
            }
        if (!wait || missing_ == 0) break;
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return !results_.empty() || (jobs_.empty() && busy_ == 0); });
    }
    // Forget tiles nobody has needed for a while (their pool slots stay until reused).
    if (frame_ % 120 == 0) {
        for (auto it = tiles_.begin(); it != tiles_.end();) {
            if (!it->second.uploaded && it->second.generated && frame_ - it->second.lastUsed > 600) it = tiles_.erase(it);
            else ++it;
        }
    }
}

int TerrainRenderer::allocSlot() {
    if (!freeSlots_.empty()) {
        const int s = freeSlots_.back();
        freeSlots_.pop_back();
        return s;
    }
    // Reuse the slot of the tile unused for longest (never one drawn this frame).
    auto victim = tiles_.end();
    for (auto it = tiles_.begin(); it != tiles_.end(); ++it)
        if (it->second.uploaded && it->second.lastUsed < frame_ && (victim == tiles_.end() || it->second.lastUsed < victim->second.lastUsed))
            victim = it;
    if (victim == tiles_.end()) return -1;
    const int s = victim->second.slot;
    tiles_.erase(victim);
    return s;
}

void TerrainRenderer::upload(rhi::CommandList* cmd) {
    constexpr uint32_t kPosBytes = kVerts * kVerts * 12;
    std::vector<int> built;
    bool any = false;
    for (const DrawItem& d : drawList_) {
        Tile& t = tiles_[d.key];
        if (t.uploaded) continue;
        if (!any) {
            cmd->barrier(pool_.get(), rhi::State::CopyDst);
            if (rayTracing_) cmd->barrier(positions_.get(), rhi::State::CopyDst);
            any = true;
        }
        t.slot = allocSlot();
        if (t.slot < 0) continue;
        rhi::Transient src = device_->allocTransient(kTileBytes);
        std::memcpy(src.cpu, t.data.data(), kTileBytes);
        cmd->copyBuffer(pool_.get(), uint64_t(t.slot) * kTileBytes, src.buffer, src.offset, kTileBytes);
        if (rayTracing_) {
            // Tile-local positions (absolute heights) for the tile's ray-tracing structure.
            const float spacing = float(tileSize(d.level) / kGrid);
            rhi::Transient pos = device_->allocTransient(kPosBytes);
            float* p = static_cast<float*>(pos.cpu);
            for (int j = 0; j < kVerts; ++j)
                for (int i = 0; i < kVerts; ++i, p += 3) {
                    p[0] = float(i) * spacing;
                    std::memcpy(&p[1], &t.data[size_t(j * kVerts + i) * 4], 4);
                    p[2] = float(j) * spacing;
                }
            cmd->copyBuffer(positions_.get(), uint64_t(t.slot) * kPosBytes, pos.buffer, pos.offset, kPosBytes);
            built.push_back(t.slot);
        }
        t.uploaded = true;
        t.data.clear();
        t.data.shrink_to_fit();
    }
    cmd->barrier(pool_.get(), rhi::State::ShaderRead);
    if (!rayTracing_ || built.empty()) return;
    cmd->barrier(positions_.get(), rhi::State::ShaderRead);
    cmd->barrier(indices_.get(), rhi::State::ShaderRead);
    for (int slot : built) {
        rhi::BlasDesc desc;
        desc.geometries.push_back({positions_.get(), uint64_t(slot) * kPosBytes, 12, uint32_t(kVerts * kVerts), indices_.get(), 0,
                                   uint32_t(kGrid * kGrid * 6), rhi::IndexType::U16, true});
        desc.name = "terrain.blas";
        slotBlas_[size_t(slot)] = device_->createBlas(desc);  // the old one (if the slot was reused) is released later
        if (slotBlas_[size_t(slot)]) cmd->buildBlas(slotBlas_[size_t(slot)].get(), desc);
    }
}

void TerrainRenderer::addInstances(std::vector<rhi::TlasInstance>& out) const {
    if (!rayTracing_) return;
    for (const DrawItem& d : drawList_) {
        auto it = tiles_.find(d.key);
        if (it == tiles_.end() || !it->second.uploaded || !slotBlas_[size_t(it->second.slot)]) continue;
        const double size = tileSize(d.level);
        rhi::TlasInstance inst;
        inst.transform[3] = float(double(d.ix) * size - camera_.x);
        inst.transform[7] = float(-camera_.y);
        inst.transform[11] = float(double(d.iz) * size - camera_.z);
        inst.customIndex = 0xFFFFFF;  // terrain: opaque, no alpha test
        inst.forceOpaque = true;
        inst.blas = slotBlas_[size_t(it->second.slot)].get();
        out.push_back(inst);
    }
}

void TerrainRenderer::draw(rhi::CommandList* cmd, const rhi::Transient& frameConstants, const uint32_t materialTextures[8]) {
    std::vector<gpu::TerrainNode> nodes;
    nodes.reserve(drawList_.size());
    const int root = levels_ - 1;
    for (const DrawItem& d : drawList_) {
        const Tile& t = tiles_[d.key];
        if (!t.uploaded) continue;
        const double size = tileSize(d.level);
        gpu::TerrainNode n{};
        n.originSize = {float(double(d.ix) * size - camera_.x), 0.0f, float(double(d.iz) * size - camera_.z), float(size)};
        // Odd vertices slide onto the parent's grid over the last quarter of this level's reach.
        const float end = d.level == root ? 1e9f : float(range(d.level));
        const float start = d.level == root ? 1e9f : end * 0.75f;
        n.info = {uint32_t(t.slot), uint32_t(d.level), floatBits(start), floatBits(end)};
        nodes.push_back(n);
    }
    if (nodes.empty()) return;
    rhi::Transient tr = device_->allocTransient(nodes.size() * sizeof(gpu::TerrainNode));
    std::memcpy(tr.cpu, nodes.data(), nodes.size() * sizeof(gpu::TerrainNode));
    TerrainPush push{};
    push.nodesSrv = tr.buffer->srv();
    push.nodesOffset = uint32_t(tr.offset);
    push.poolSrv = pool_->srv();
    for (int i = 0; i < 4; ++i) {
        push.albedo[i] = materialTextures[i];
        push.normal[i] = materialTextures[4 + i];
    }
    cmd->beginMarker("terrain");
    cmd->setPipeline(pipeline_.get());
    cmd->setFrameConstants(frameConstants);
    cmd->pushConstants(&push, sizeof push);
    cmd->setIndexBuffer(indices_.get(), 0, rhi::IndexType::U16);
    cmd->drawIndexed(kGrid * kGrid * 6, uint32_t(nodes.size()));
    cmd->endMarker();
}

}  // namespace taelia

namespace taelia {

int TerrainRenderer::leafSlot(int64_t ix, int64_t iz) const {
    auto it = tiles_.find(makeKey(0, ix, iz));
    return (it != tiles_.end() && it->second.uploaded) ? it->second.slot : -1;
}

}  // namespace taelia
