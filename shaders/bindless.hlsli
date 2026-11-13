// The binding model shared by every shader (see engine/rhi/rhi.h). Index 0 is the null descriptor.
// D3D12: one table of unbounded ranges, all at heap offset 0, one register space per declaration.
// Vulkan: descriptor set 0 (aliased bindings per descriptor type), set 1 = frame constants, push constants = b1.
#ifndef TAELIA_BINDLESS_HLSLI
#define TAELIA_BINDLESS_HLSLI

#include "shared_constants.hlsli"
#include "gpu_types.h"

// set 0 binding 0: sampled images
[[vk::binding(0, 0)]] Texture2D<float4> g_tex2D[] : register(t0, space1);
[[vk::binding(0, 0)]] Texture2D<uint4> g_tex2DUint[] : register(t0, space2);
[[vk::binding(0, 0)]] Texture3D<float4> g_tex3D[] : register(t0, space3);
[[vk::binding(0, 0)]] TextureCube<float4> g_texCube[] : register(t0, space4);
[[vk::binding(0, 0)]] Texture2DArray<float4> g_tex2DArray[] : register(t0, space5);
// set 0 binding 1: storage images
[[vk::binding(1, 0)]] RWTexture2D<float4> g_rwTex2D[] : register(u0, space6);
[[vk::binding(1, 0)]] RWTexture3D<float4> g_rwTex3D[] : register(u0, space7);
[[vk::binding(1, 0)]] RWTexture2D<uint> g_rwTex2DUint[] : register(u0, space8);
// set 0 binding 2: storage buffers
[[vk::binding(2, 0)]] ByteAddressBuffer g_buffers[] : register(t0, space9);
[[vk::binding(2, 0)]] RWByteAddressBuffer g_rwBuffers[] : register(u0, space10);
// set 0 binding 3: acceleration structures (TLAS)
[[vk::binding(3, 0)]] RaytracingAccelerationStructure g_tlas[] : register(t0, space11);
// set 0 bindings 4..9: fixed samplers
[[vk::binding(4, 0)]] SamplerState s_linearWrap : register(s0, space0);
[[vk::binding(5, 0)]] SamplerState s_linearClamp : register(s1, space0);
[[vk::binding(6, 0)]] SamplerState s_pointClamp : register(s2, space0);
[[vk::binding(7, 0)]] SamplerState s_anisoWrap : register(s3, space0);
[[vk::binding(8, 0)]] SamplerState s_pointWrap : register(s4, space0);
[[vk::binding(9, 0)]] SamplerComparisonState s_shadow : register(s5, space0);

// set 1 binding 0: frame constants (b0)
[[vk::binding(0, 1)]] ConstantBuffer<FrameConstants> g_frame : register(b0, space0);

// Push constants (b1): each shader declares its own struct, at most 128 bytes:
//   [[vk::push_constant]] ConstantBuffer<MyPush> g_push : register(b1, space0);

#endif
