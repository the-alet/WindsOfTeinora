// Shared ray-tracing helpers: the scene TLAS (frame constants scene.x) and the leaf alpha test.
<<<<<<< Updated upstream
// Tree instances carry customIndex = entry in the alpha table (scene.y: first leaf index, leaf texture);
// terrain instances are 0xFFFFFF and opaque.
=======
// Tree instances carry customIndex = entry in the alpha table (scene.y: first leaf index, leaf texture), and in bits
// 16-22 how far fire has charred the tree;
// terrain instances are 0xFFFFFF, the campfire 0xFFFFFE, balls 0xFFFFFD: all opaque, never in the alpha table.
>>>>>>> Stashed changes
#ifndef TAELIA_RT_HLSLI
#define TAELIA_RT_HLSLI

#include "common.hlsli"

static const uint TERRAIN_INSTANCE = 0xFFFFFF;

bool rtEnabled() { return g_frame.flags.w != 0 && g_frame.scene.x != 0; }

<<<<<<< Updated upstream
// True if the leaf card triangle is solid at the hit point (its texture's alpha).
bool leafAlphaHit(uint entry, uint prim, float2 bc) {
    if (entry == TERRAIN_INSTANCE) return true;
=======
// True if the leaf card triangle is solid at the hit point (its texture's alpha). A tree's id carries in bits 16-22 how
// far fire has charred it: its leaves burn away as tree.hlsl's LeafPS shows them.
bool leafAlphaHit(uint id, uint prim, float2 bc) {
    if (id >= FIRST_PROP_INSTANCE) return true;
    const uint entry = id & 0xFFFFu;
    const float charred = float((id >> 16) & 127u) / 127.0;
    if (charred >= 0.999) return false;
>>>>>>> Stashed changes
    const uint2 info = g_buffers[g_frame.scene.y].Load2(entry * 8);
    const uint3 tri = g_buffers[g_frame.scene.w].Load3((info.x + prim * 3) * 4);
    ByteAddressBuffer vb = g_buffers[g_frame.scene.z];
    const float3 w = float3(1.0 - bc.x - bc.y, bc.x, bc.y);
    if (charred > 0.0) {
        const float3 p = asfloat(vb.Load3(tri.x * 32)) * w.x + asfloat(vb.Load3(tri.y * 32)) * w.y + asfloat(vb.Load3(tri.z * 32)) * w.z;
        if (charred > leafBurnNoise(p)) return false;
    }
    const float2 uv0 = asfloat(vb.Load2(tri.x * 32 + 16)), uv1 = asfloat(vb.Load2(tri.y * 32 + 16)), uv2 = asfloat(vb.Load2(tri.z * 32 + 16));
    const float2 uv = uv0 * w.x + uv1 * w.y + uv2 * w.z;
    return g_tex2D[info.y].SampleLevel(s_linearClamp, uv, 1).a > 0.5;
}

#endif
