// The air (step 3): precomputed scattering tables for two suns and the night light, so every pass can ask
// atmosphere.hlsli for sky radiance, sun transmittance, sky irradiance and aerial perspective.
// Hillaire 2020: transmittance and multiple scattering once; sky view, aerial perspective volume and sky irradiance
// (SH9 + overcast grey) each frame.
#pragma once
#include <memory>
#include <string>

#include "rhi/rhi.h"

namespace taelia {

class AtmosphereRenderer {
public:
    bool init(rhi::Device* device, std::string* error);
    void shutdown();
    // Outside render passes, after the frame constants are filled: rebuilds this frame's tables.
    void update(rhi::CommandList* cmd, const rhi::Transient& frameConstants);
    // Bindless indices for FrameConstants::atmosphere (x..w) and atmosphere2.x; valid from init on.
    uint32_t transmittanceLut() const { return transmittance_ ? transmittance_->srv() : 0; }
    uint32_t multiScatteringLut() const { return multiScattering_ ? multiScattering_->srv() : 0; }
    uint32_t skyViewLut() const { return skyView_ ? skyView_->srv() : 0; }
    uint32_t aerialPerspectiveVolume() const { return aerial_ ? aerial_->srv() : 0; }
    uint32_t skyIrradiance() const { return irradiance_ ? irradiance_->srv() : 0; }

private:
    rhi::Device* device_ = nullptr;
    std::unique_ptr<rhi::Texture> transmittance_, multiScattering_, skyView_, aerial_;
    std::unique_ptr<rhi::Buffer> irradiance_;
    std::unique_ptr<rhi::Pipeline> transmittancePipe_, multiScatteringPipe_, skyViewPipe_, aerialPipe_, irradiancePipe_;
    bool staticReady_ = false;
};

}  // namespace taelia
