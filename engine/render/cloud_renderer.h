// Volumetric clouds (step 3) from the simulation's one cloud map (sim/clouds.h): the same map the weather reads.
// update() builds the map around the camera and the cloud shadow map the lighting reads; render() raymarches the
// clouds into the lit scene (over the sky and anything farther than them).
//
// The GPU map: kMapSize^2 texels of kMapTexel metres, RGBA16F = coverage, base (m above sea level), thickness (m), 1.
// Its texels sit on the clouds' own grid (u = world x - displacement.x, v = world z - displacement.z), so the wind
// moves it without rebuilding; it is rebuilt on a worker thread when the camera nears its edge or time moved on.
// Frame constants (see also shaders/clouds.hlsli):
//   clouds   xy: how far the wind carried the clouds (m), z: the day's cover 0..1, w: map texel size (m)
//   cloudMap xy: world xz of the map's corner relative to the camera, z: map size (texels),
//            w: cloud thickness for the day's cover (m), used past the map's edge
//   atmosphere2 y: cloud shadow map (rgb: light let through toward sun 0, sun 1, straight up), z: map, w: clouds
#pragma once
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "rhi/rhi.h"
#include "taelia/sim/math.h"

namespace taelia {

class CloudMap;

class CloudRenderer {
public:
    ~CloudRenderer() { shutdown(); }
    bool init(rhi::Device* device, const CloudMap* clouds, std::string* error);
    void shutdown();
    // Values for FrameConstants::clouds and ::cloudMap this frame (see shared/gpu_types.h).
    void frameData(const dvec3& camera, float clouds[4], float cloudMap[4]) const;
    // Outside render passes, after the frame constants are filled. `wait`: automated runs, the map must be current.
    void update(rhi::CommandList* cmd, const rhi::Transient& frameConstants, const dvec3& camera, bool wait);
    // After lighting: composites the clouds into `scene` (render size, RGBA16F, Storage) using `depth`.
    void render(rhi::CommandList* cmd, const rhi::Transient& frameConstants, rhi::Texture* scene, rhi::Texture* depth);
    // Bindless indices for FrameConstants::atmosphere2 (y shadow, z map, w clouds); 0 until built.
    uint32_t shadowMap() const { return shadowBuilt_ ? shadow_->srv() : 0; }
    uint32_t mapTexture() const { return haveMap_ ? map_->srv() : 0; }
    uint32_t result() const { return result_ ? result_->srv() : 0; }

private:
    struct MapJob {
        double u0 = 0, v0 = 0, seconds = 0;  // corner in cloud coordinates, simulated time
        std::vector<uint16_t> texels;        // RGBA16F
    };
    MapJob buildMap(double u0, double v0, double seconds) const;
    void wantedMap(const dvec3& camera, double& u0, double& v0) const;
    bool stale(const MapJob& m, double u0, double v0, double seconds) const;
    void uploadMap(rhi::CommandList* cmd, MapJob&& m);
    void patchConstants(const rhi::Transient& frameConstants, const dvec3& camera) const;
    void noiseOffset(const dvec3& camera, float out[4]) const;

    rhi::Device* device_ = nullptr;
    const CloudMap* clouds_ = nullptr;
    std::unique_ptr<rhi::Pipeline> shapePipe_, detailPipe_, shadowPipe_, marchPipe_, resolvePipe_, compositePipe_;
    std::unique_ptr<rhi::Texture> map_, shadow_, shape_, detail_;
    std::unique_ptr<rhi::Texture> marchColor_, marchDepth_, result_, history_;
    bool noiseReady_ = false, haveMap_ = false, shadowBuilt_ = false, historyValid_ = false;
    double mapU0_ = 0, mapV0_ = 0, mapSeconds_ = 0;
    dvec3 lastCamera_;
    std::future<MapJob> pending_;
};

}  // namespace taelia
