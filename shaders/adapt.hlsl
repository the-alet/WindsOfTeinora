// The eye: exposure from the scene's average brightness (adapting over time, only partly in the dark so night stays
// night), and a white balance halfway toward the colour of the light. State: 16 bytes in a buffer the post pass reads.
#include "atmosphere.hlsli"

struct AdaptPush {
    uint hdrSrv;    // post input with mips
    uint mip;       // the smallest mip
    uint stateUav;  // float4: exposure, white balance rgb
    uint first;     // 1 on the first frame: jump straight to the target
    float dt;       // real seconds since the last frame
    float3 pad;
};
[[vk::push_constant]] ConstantBuffer<AdaptPush> g_push : register(b1, space0);

groupshared float s_logSum[256];
groupshared float s_weight[256];

[numthreads(256, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    Texture2D<float4> hdr = g_tex2D[g_push.hdrSrv];
    uint w, h, levels;
    hdr.GetDimensions(g_push.mip, w, h, levels);
    float sum = 0, weight = 0;
    for (uint i = id.x; i < w * h; i += 256) {
        const uint2 p = uint2(i % w, i / w);
        const float3 c = hdr.Load(int3(p, g_push.mip)).rgb;
        const float lum = max(dot(c, float3(0.2126, 0.7152, 0.0722)), 1e-6);
        // The middle of the view counts more.
        const float2 d = (float2(p) + 0.5) / float2(w, h) - 0.5;
        const float wt = 1.0 - 0.6 * saturate(length(d) * 1.6);
        sum += log(lum) * wt;
        weight += wt;
    }
    s_logSum[id.x] = sum;
    s_weight[id.x] = weight;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128; s > 0; s >>= 1) {
        if (id.x < s) {
            s_logSum[id.x] += s_logSum[id.x + s];
            s_weight[id.x] += s_weight[id.x + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (id.x != 0) return;

    const float avg = exp(s_logSum[0] / max(s_weight[0], 1e-6));
    // Full adaptation by day; in the dark the eye only partly opens, so night stays dark.
    const float target = 0.16 / (avg + 0.0025);

    // The light's colour: both suns as they reach the ground, plus the sky.
    float3 light = skyIrradiance(float3(0, 1, 0));
    [unroll] for (int k = 0; k < 2; ++k) {
        const float3 L = g_frame.suns[k].xyz;
        light += g_frame.sunLight[k].rgb * (1.0 - g_frame.suns[k].w) * sunTransmittance(L) * saturate(L.y * 2.0 + 0.1);
    }
    const float lum = max(dot(light, float3(0.2126, 0.7152, 0.0722)), 1e-6);
    float3 balance = lerp(1.0.xxx, lum / max(light, lum * 0.05), 0.5);  // half way
    balance /= dot(balance, float3(0.2126, 0.7152, 0.0722));

    RWByteAddressBuffer state = g_rwBuffers[g_push.stateUav];
    float4 cur = asfloat(state.Load4(0));
    if (g_push.first != 0 || !(cur.x > 0)) {
        cur = float4(target, balance);
    } else {
        const float rate = target > cur.x ? 1.2 : 0.45;  // brighter quickly, darker slowly (per second)
        const float k = 1.0 - exp(-g_push.dt * rate);
        cur.x = exp(lerp(log(cur.x), log(target), k));
        cur.yzw = lerp(cur.yzw, balance, 1.0 - exp(-g_push.dt * 0.5));
    }
    state.Store4(0, asuint(cur));
}
