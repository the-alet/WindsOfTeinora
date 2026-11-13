// The G-buffer every opaque surface writes (gbuffer.hlsli is the shader side).
#pragma once
#include "rhi/rhi.h"

namespace taelia::gbuffer {

inline constexpr rhi::Format kAlbedo = rhi::Format::RGBA8_SRGB;     // rgb albedo, a: ambient occlusion
inline constexpr rhi::Format kNormal = rhi::Format::RGBA16_FLOAT;   // xyz world normal, w: roughness
inline constexpr rhi::Format kMaterial = rhi::Format::RGBA8_UNORM;  // r: translucency, g: specular, b: wetness, a: unused
inline constexpr rhi::Format kMotion = rhi::Format::RG16_FLOAT;      // screen motion in uv (for DLSS)
inline constexpr rhi::Format kDepth = rhi::Format::D32_FLOAT;
inline constexpr uint32_t kTargets = 4;

inline void fillFormats(rhi::GraphicsPipelineDesc& d) {
    d.colorFormats[0] = kAlbedo;
    d.colorFormats[1] = kNormal;
    d.colorFormats[2] = kMaterial;
    d.colorFormats[3] = kMotion;
    d.colorCount = kTargets;
    d.depthFormat = kDepth;
    d.depthTest = true;
    d.depthWrite = true;
    d.depthCompare = rhi::Compare::GreaterEqual;
}

}  // namespace taelia::gbuffer
