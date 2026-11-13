// Trees: meshes built from the simulation's skeletons (bark tubes and leaf cards, three levels of detail),
// placed from the simulation's forest chunks (generated on worker threads), drawn instanced into the G-buffer.
#pragma once
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "render/terrain_renderer.h"
#include "rhi/rhi.h"
#include "taelia/sim/plants.h"

namespace taelia {

struct TreeVertexOut {  // 32 bytes, read by tree.hlsl
    float pos[3];
    uint32_t normal;  // octahedral, 2 x unorm16
    float uv[2];
    uint32_t info;    // bits 0-7 sway weight, bit 8 leaf
    uint32_t pad;
};

class TreeRenderer {
public:
    ~TreeRenderer();
    bool init(rhi::Device* device, const Terrain* terrain, int detail, std::string* error);
    void shutdown();
    // Picks the trees around the camera (`wait`: block until every forest chunk in reach is generated).
    void select(const dvec3& camera, const Frustum& frustum, bool wait);
    // Inside the G-buffer pass. textures: bark albedo, bark detail, leaf albedo, leaf detail (per species).
    void draw(rhi::CommandList* cmd, const rhi::Transient& frameConstants, const uint32_t barkTex[2][3], const uint32_t leafTex[2][3]);
    uint32_t drawnTrees() const { return drawn_; }
    // Ray tracing: builds the structures once (outside render passes), lists trees near the camera, and the
    // table shaders use to alpha-test leaves (per customIndex: first leaf index, leaf texture).
    void buildStructures(rhi::CommandList* cmd, const uint32_t leafAlbedo[3]);
    void addInstances(std::vector<rhi::TlasInstance>& out, double maxDistance) const;
    rhi::Buffer* alphaTable() const { return alphaTable_.get(); }
    rhi::Buffer* vertexBuffer() const { return vertices_.get(); }
    rhi::Buffer* indexBuffer() const { return indices_.get(); }
    void setRockTextures(uint32_t albedo, uint32_t detail) { rockTex_[0] = albedo; rockTex_[1] = detail; }

    static constexpr int kLods = 3;

private:
    struct MeshPart {
        uint32_t firstIndex = 0, indexCount = 0;
    };
    struct Mesh {
        MeshPart bark[kLods], leaves[kLods];
        float height = 0, radius = 0;
    };
    struct Chunk {
        bool ready = false;
        uint64_t lastUsed = 0;
        std::vector<TreeInstance> trees;
    };
    struct InstanceGpu {
        float posScale[4];  // camera-relative base, scale
        float rot[4];       // cos yaw, sin yaw, sway phase, unused
    };

    void buildMeshes();
    void workerLoop();
    static uint64_t chunkKey(int64_t cx, int64_t cz);

    rhi::Device* device_ = nullptr;
    const Terrain* terrain_ = nullptr;
    double lodDistance_[kLods] = {120, 450, 1500};
    std::unique_ptr<rhi::Buffer> vertices_, indices_;
    std::unique_ptr<rhi::Pipeline> barkPipe_, leafPipe_;
    std::vector<Mesh> meshes_;  // kind * kTreeVariants + variant
    std::vector<std::unique_ptr<rhi::AccelerationStructure>> blas_;  // per mesh * kLods + lod
    std::unique_ptr<rhi::Buffer> alphaTable_;
    uint32_t vertexCount_ = 0;
    // The campfire by the spawn point (stones and charred logs), drawn with the bark pipeline.
    MeshPart fireStones_, fireLogs_;
    std::unique_ptr<rhi::AccelerationStructure> fireBlas_;
    float fireTransform_[12] = {};
    bool fireVisible_ = false;
    float fireInstance_[8] = {};  // InstanceGpu layout
    uint32_t rockTex_[2] = {};
    void buildCampfire(std::vector<TreeVertexOut>& verts, std::vector<uint32_t>& idx);
    double rtDistance_ = 350.0;
    struct RtTree {
        float transform[12];
        uint32_t entry;
    };
    std::vector<RtTree> rtTrees_;  // the trees selected this frame, for addInstances

    std::unordered_map<uint64_t, Chunk> chunks_;
    std::vector<std::vector<InstanceGpu>> buckets_;  // per (mesh, lod)
    uint32_t drawn_ = 0;
    uint64_t frame_ = 0;

    std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::deque<uint64_t> jobs_;
    std::vector<std::pair<uint64_t, std::vector<TreeInstance>>> results_;
    std::vector<std::thread> workers_;
    bool quit_ = false;
    int busy_ = 0;
};

}  // namespace taelia
