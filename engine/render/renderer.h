// The frame: world (HDR + depth) -> blur pyramid -> post (focus blur, edge darkening, tone curve) -> UI.
#pragma once
#include <memory>
#include <string>
#include <unordered_map>

#include "render/ball_renderer.h"
#include "render/manifest_renderer.h"
#include "render/imgui_renderer.h"
#include "render/terrain_renderer.h"
#include "render/tree_renderer.h"
#include "render/grass_renderer.h"
#include "render/atmosphere.h"
#include "render/cloud_renderer.h"
#include "render/magic_renderer.h"
#include "taelia/sim/sky.h"
#include "rhi/rhi.h"
#include "taelia/sim/math.h"

struct ImDrawData;

namespace taelia {

struct RenderView {
    dvec3 cameraPos;
    dvec3 forward{0, 0, 1}, right{1, 0, 0}, up{0, 1, 0};
    double fovY = 1.0;
    double gazeNdcX = 0, gazeNdcY = 0;   // the eye's focus on screen
    double hourOfDay = 12, simSeconds = 0, realSeconds = 0, timeSpeed = 1;
    dvec3 wind;
    double groundHeight = 0;
    bool magicView = false;
    bool waitForStreaming = false;  // automated runs: block until the terrain around the camera is in
    rhi::UpscaleMode upscale = rhi::UpscaleMode::Off;  // DLSS (used only when the device supports it)
    SkyState sky;                     // suns, planet, stars for this moment (sim/sky.h)
    uint64_t seed = 1;                // the world seed (the star catalogue, the galaxy)
    uint64_t frame = 0;
    const std::vector<BallPose>* balls = nullptr;  // thrown balls to draw (World::ballPoses)
    const MagicField* magic = nullptr;              // the magic field (World::magic)
    dvec3 campfire;                                 // the campfire's bed
    double campfireStrength = 0;                    // 0: out, 1: as it starts
    double rain = 0, storm = 0;                     // the weather over the camera
    dvec3 strike;                                   // the last lightning strike's ground point
    double strikeAge = -1;                          // seconds since it (< 0: none lately)
};

class Renderer {
public:
    bool init(rhi::Device* device, const Terrain* terrain, const CloudMap* clouds, int detail, std::string* error);
    void shutdown();
    // Renders into `output` (left in RenderTarget state). renderScale in (0, 1] sizes the HDR image.
    void render(rhi::CommandList* cmd, rhi::Texture* output, const RenderView& view, ImDrawData* ui, float renderScale);
    ImGuiRenderer& imgui() { return imgui_; }
    // Detail changes rebuild the terrain tiles (waits for the GPU).
    void setDetail(const Terrain* terrain, int detail);

private:
    // Render-size targets (G-buffer, scene) and the post input (output size when DLSS upscales).
    void ensureTargets(uint32_t renderW, uint32_t renderH, uint32_t postW, uint32_t postH);
    void upscaleOrCopy(rhi::CommandList* cmd, const RenderView& v, const rhi::Transient& fc);
    bool upscaleFailed_ = false;
    rhi::Pipeline* postPipeline(rhi::Format format);
    void fillFrameConstants(const RenderView& v, uint32_t outW, uint32_t outH, rhi::Transient& t, Frustum& frustum);
    void generateTextures(rhi::CommandList* cmd);

    rhi::Device* device_ = nullptr;
    std::unique_ptr<rhi::Texture> hdr_, depth_, gbAlbedo_, gbNormal_, gbMaterial_, gbMotion_;
    std::unique_ptr<rhi::Texture> groundAlbedo_[4], groundDetail_[4];
    bool texturesReady_ = false;
    TerrainRenderer terrain_;
    TreeRenderer trees_;
    GrassRenderer grass_;
    BallRenderer balls_;
    ManifestRenderer manifest_;
    AtmosphereRenderer atmosphere_;
    CloudRenderer cloudRenderer_;
    MagicRenderer magic_;
    std::unique_ptr<rhi::Pipeline> weatherPipe_;
    std::unique_ptr<rhi::AccelerationStructure> tlas_;
    std::vector<rhi::TlasInstance> instances_;
    static constexpr uint32_t kMaxInstances = 16384;
    std::unique_ptr<rhi::Texture> barkAlbedo_[3], barkDetail_[3], leafAlbedo_[3], leafDetail_[3];
    std::unique_ptr<rhi::Pipeline> treetex_;
    uint32_t mipCount_ = 1;
    std::unique_ptr<rhi::Pipeline> lighting_, texgen_, downsample_, water_;
    std::unique_ptr<rhi::Texture> sceneColor_;  // lit scene before the water, for refraction
    std::unique_ptr<rhi::Texture> sceneHdr_;    // lit scene at render size (lighting, water)
    std::unique_ptr<rhi::Texture> motion_;      // camera motion per pixel (DLSS)
    std::unique_ptr<rhi::Texture> dlssOut_;     // DLSS output at output size
    std::unique_ptr<rhi::Pipeline> motionPipe_;
    // DLSS bookkeeping: this and last frame's unjittered matrices, the jitter sequence, history resets.
    double viewProj_[16] = {}, proj_[16] = {}, prevViewProj_[16] = {};
    dvec3 prevCamera_;
    bool havePrev_ = false, upscaling_ = false, resetHistory_ = true;
    float jitter_[2] = {};
    uint64_t jitterIndex_ = 0;
    rhi::UpscaleMode lastMode_ = rhi::UpscaleMode::Off;
    RenderView view_;
    const Terrain* terrainSim_ = nullptr;
    void drawWater(rhi::CommandList* cmd, const RenderView& v, const rhi::Transient& fc);
    void drawStars(rhi::CommandList* cmd, const RenderView& v, const rhi::Transient& fc);
    std::unique_ptr<rhi::Buffer> stars_;
    std::unique_ptr<rhi::Pipeline> starsPipe_, adaptPipe_;
    std::unique_ptr<rhi::Buffer> adaptState_;  // the eye: exposure, white balance (adapt.hlsl)
    bool adaptFirst_ = true;
    double lastReal_ = 0;
    uint32_t starCount_ = 0;
    // Ray tracing: the scene structure for this frame; fills the scene slots of the frame constants.
    void buildScene(rhi::CommandList* cmd, const rhi::Transient& fc);
    std::unordered_map<int, std::unique_ptr<rhi::Pipeline>> post_;
    ImGuiRenderer imgui_;
};

}  // namespace taelia
