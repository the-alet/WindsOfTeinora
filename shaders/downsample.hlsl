// 2x2 box downsample of one mip into the next (the blur pyramid for the focus blur).
#include "bindless.hlsli"

struct DownPush {
    uint srcUav;
    uint dstUav;
    uint2 srcSize;
    uint2 dstSize;
    uint2 pad;
};
[[vk::push_constant]] ConstantBuffer<DownPush> g_push : register(b1, space0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_push.dstSize)) return;
    uint2 s0 = min(id.xy * 2, g_push.srcSize - 1);
    uint2 s1 = min(id.xy * 2 + 1, g_push.srcSize - 1);
    RWTexture2D<float4> src = g_rwTex2D[g_push.srcUav];
    float4 c = src[s0] + src[uint2(s1.x, s0.y)] + src[uint2(s0.x, s1.y)] + src[s1];
    g_rwTex2D[g_push.dstUav][id.xy] = c * 0.25;
}
