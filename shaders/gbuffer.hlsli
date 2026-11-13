// What every opaque surface writes (engine/render/gbuffer.h is the C++ side).
#ifndef TAELIA_GBUFFER_HLSLI
#define TAELIA_GBUFFER_HLSLI

struct GBufferOut {
    float4 albedo : SV_Target0;    // rgb albedo (sRGB target), a: ambient occlusion
    float4 normal : SV_Target1;    // xyz world normal, w: roughness
    float4 material : SV_Target2;  // r: translucency, g: specular, b: subsurface (skin: light spreads under it), a: packMagic()
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

// material.a: the magic a surface holds and one flag. Bits 0-2: its aspect + 1 (0: none), bits 3-6: how full its share
// is (0..15), bit 7: the motion target holds this surface's own motion (so a > 0.5 tests it).
static const uint MAGIC_HELD_EARTH = 1, MAGIC_HELD_WATER = 2, MAGIC_HELD_NATURE = 3, MAGIC_HELD_FIRE = 4;
float packMagic(uint aspect1, float fill, bool ownMotion) {
    const uint f = uint(round(saturate(fill) * 15.0));
    return float(aspect1 | (f << 3) | (ownMotion ? 128u : 0u)) / 255.0;
}
void unpackMagic(float a, out uint aspect1, out float fill) {
    const uint b = uint(round(a * 255.0));
    aspect1 = b & 7u;
    fill = float((b >> 3) & 15u) / 15.0;
}
#endif
