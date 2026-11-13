// What every opaque surface writes (engine/render/gbuffer.h is the C++ side).
#ifndef TAELIA_GBUFFER_HLSLI
#define TAELIA_GBUFFER_HLSLI

struct GBufferOut {
    float4 albedo : SV_Target0;    // rgb albedo (sRGB target), a: ambient occlusion
    float4 normal : SV_Target1;    // xyz world normal, w: roughness
    float4 material : SV_Target2;  // r: translucency, g: specular, b: wetness, a: unused
    float2 motion : SV_Target3;    // screen motion in uv
};

// Octahedral decode of a normal packed as two unorm16 in one uint.
float3 decodeOctNormal(uint p) {
    float2 e = float2(p & 0xffffu, p >> 16) / 65535.0 * 2.0 - 1.0;
    float3 n = float3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    if (n.y < 0) n.xz = (1.0 - abs(n.zx)) * select(n.xz >= 0, 1.0, -1.0);
    return normalize(n);
}

float4 unpackUnorm8x4(uint p) {
    return float4(p & 255u, (p >> 8) & 255u, (p >> 16) & 255u, p >> 24) / 255.0;
}

#endif
