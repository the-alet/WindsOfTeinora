// Terrain as a quadtree of tiles with distance-based detail and morphing between levels (CDLOD).
// Tiles are generated on worker threads from the simulation's own height function and streamed into one GPU pool.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rhi/rhi.h"
#include "taelia/sim/math.h"

namespace taelia {

class Terrain;

// Six camera-relative planes (a, b, c, d): a*x + b*y + c*z + d >= 0 inside.
struct Frustum {
    double planes[6][4] = {};
    bool boxVisible(const dvec3& lo, const dvec3& hi) const;
};

class TerrainRenderer {
public:
    ~TerrainRenderer();
    // detail 0..3 sets the finest tile size and how far each level reaches (more polygons at the highest).
    bool init(rhi::Device* device, const Terrain* terrain, int detail, std::string* error);
    void shutdown();

    // Picks the tiles for this camera and queues the missing ones. With `wait`, blocks until every tile it
    // needs is generated (automated runs: the same flags give the same frame).
    void select(const dvec3& camera, const Frustum& frustum, bool wait);
    // Uploads finished tiles (outside render passes). Call after select, before draw.
    void upload(rhi::CommandList* cmd);
    // Draws the selected tiles into the G-buffer (inside its render pass).
    void draw(rhi::CommandList* cmd, const rhi::Transient& frameConstants, const uint32_t materialTextures[8]);

    rhi::Buffer* pool() const { return pool_.get(); }
    // Pool slot of the finest tile (ix, iz) if it is uploaded, else -1 (grass reads the ground from it).
    int leafSlot(int64_t ix, int64_t iz) const;
    double leafSize() const { return leafSize_; }
    // Ray tracing: one instance per drawn tile (its structure is built when the tile is uploaded).
    void addInstances(std::vector<rhi::TlasInstance>& out) const;
    uint32_t drawnTiles() const { return uint32_t(drawList_.size()); }
    int levels() const { return levels_; }

private:
    struct Tile {
        int slot = -1;           // pool slot once uploaded
        bool generated = false;  // CPU data ready
        bool uploaded = false;
        float minH = -400, maxH = 1500;
        uint64_t lastUsed = 0;
        std::vector<uint32_t> data;
    };
    struct DrawItem {
        uint64_t key;
        int level;
        int64_t ix, iz;
    };

    static uint64_t makeKey(int level, int64_t ix, int64_t iz);
    double tileSize(int level) const { return leafSize_ * double(1ll << level); }
    double range(int level) const { return tileSize(level) * rangeFactor_; }
    void selectNode(int level, int64_t ix, int64_t iz, const dvec3& cam, const Frustum& f, float parentMin, float parentMax);
    Tile* request(int level, int64_t ix, int64_t iz);
    void workerLoop();
    void generate(uint64_t key, std::vector<uint32_t>& out, float& minH, float& maxH) const;
    int allocSlot();

    rhi::Device* device_ = nullptr;
    const Terrain* terrain_ = nullptr;
    double leafSize_ = 32.0, rangeFactor_ = 2.5;
    int levels_ = 8;  // level 0 = finest; levels_ - 1 = root

    std::unique_ptr<rhi::Buffer> pool_, indices_, positions_;  // positions_: float3 per vertex, tile-local, for ray tracing
    std::vector<std::unique_ptr<rhi::AccelerationStructure>> slotBlas_;
    bool rayTracing_ = false;
    std::unique_ptr<rhi::Pipeline> pipeline_;
    uint32_t poolSlots_ = 0;
    std::vector<int> freeSlots_;

    std::unordered_map<uint64_t, Tile> tiles_;  // main thread only
    std::vector<DrawItem> drawList_;
    dvec3 camera_;
    uint64_t frame_ = 0;
    int missing_ = 0;

    // Worker threads: keys in, generated data out.
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::deque<uint64_t> jobs_;
    std::vector<std::pair<uint64_t, Tile>> results_;
    std::vector<std::thread> workers_;
    bool quit_ = false;
    int busy_ = 0;
};

}  // namespace taelia
