// Shared ray-tracing helpers: the scene TLAS (frame constants scene.x) and the leaf alpha test.
// Tree instances carry customIndex = entry in the alpha table (scene.y: first leaf index, leaf texture);
// terrain instances are 0xFFFFFF, the campfire 0xFFFFFE, balls 0xFFFFFD: all opaque, never in the alpha table.
#ifndef TAELIA_RT_HLSLI
#define TAELIA_RT_HLSLI

#include "common.hlsli"

static const uint TERRAIN_INSTANCE = 0xFFFFFF;
static const uint CAMPFIRE_INSTANCE = 0xFFFFFE;
static const uint BALL_INSTANCE = 0xFFFFFD;
static const uint FIRST_PROP_INSTANCE = 0xFFFFF0;  // ids from here up are opaque props, not tree entries

bool rtEnabled() { return g_frame.flags.w != 0 && g_frame.scene.x != 0; }

// True if the leaf card triangle is solid at the hit point (its texture's alpha).
bool leafAlphaHit(uint entry, uint prim, float2 bc) {
    if (entry >= FIRST_PROP_INSTANCE) return true;
    const uint2 info = g_buffers[g_frame.scene.y].Load2(entry * 8);
    const uint3 tri = g_buffers[g_frame.scene.w].Load3((info.x + prim * 3) * 4);
    ByteAddressBuffer vb = g_buffers[g_frame.scene.z];
    const float2 uv0 = asfloat(vb.Load2(tri.x * 32 + 16)), uv1 = asfloat(vb.Load2(tri.y * 32 + 16)), uv2 = asfloat(vb.Load2(tri.z * 32 + 16));
    const float2 uv = uv0 * (1.0 - bc.x - bc.y) + uv1 * bc.x + uv2 * bc.y;
    return g_tex2D[info.y].SampleLevel(s_linearClamp, uv, 1).a > 0.5;
}

#endif
