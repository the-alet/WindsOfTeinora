// What dense magic shows (step 5f): minerals (clusters of amber crystals, earth) and sprouts (small leafy plants, nature)
// in the G-buffer, minerals also in the ray-traced scene; fireflies (light, not insects) as soft glowing sprites; and
// for the frame constants, the box where magic is past the dew point.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rhi/rhi.h"
#include "taelia/sim/magic.h"

namespace taelia {

namespace gpu {
struct FrameConstants;
}

class ManifestRenderer {
public:
    bool init(rhi::Device* device, std::string* error);
    void shutdown();
    void update(const MagicField* field, const dvec3& camera);
    void draw(rhi::CommandList* cmd, const rhi::Transient& frameConstants);  // inside the G-buffer pass
    // After the scene is lit (sprites over it, depth tested).
    void drawFireflies(rhi::CommandList* cmd, const rhi::Transient& frameConstants, rhi::Texture* hdr, rhi::Texture* depth);
    void buildStructures(rhi::CommandList* cmd);
    void addInstances(std::vector<rhi::TlasInstance>& out) const;
    void frameData(gpu::FrameConstants& fc, const MagicField* field, const dvec3& camera) const;

private:
    struct InstanceGpu {  // 32 bytes, read by manifest.hlsl
        float rel[3];
        float scale;
        float cosYaw, sinYaw, fill;
        uint32_t seed;
    };
    struct FireflyGpu {  // 16 bytes
        float rel[3];
        uint32_t packed;  // bit 0: big; the rest a seed
    };
    rhi::Device* device_ = nullptr;
    std::unique_ptr<rhi::Buffer> vertices_, indices_;
    std::unique_ptr<rhi::Pipeline> pipe_, fireflyPipe_;
    std::unique_ptr<rhi::AccelerationStructure> blas_;
    uint32_t crystalIndices_ = 0, sproutFirst_ = 0, sproutIndices_ = 0, crystalVertices_ = 0;
    std::vector<InstanceGpu> minerals_, sprouts_;
    std::vector<FireflyGpu> fireflies_;
    std::vector<rhi::TlasInstance> rt_;
};

}  // namespace taelia
