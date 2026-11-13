// The generated stars (sim/sky.h: none of them is a real star): tiny additive sprites on sky pixels only
// (depth 0), turned with the sky, dimmed near the horizon, hidden behind the planet. By day the sky outshines them.
#include "common.hlsli"
#include "skybodies.hlsli"

struct StarPush {
    uint starsSrv;
    uint count;
    float brightness;  // radiance of a magnitude-0 star at its centre
    float pad;
};
[[vk::push_constant]] ConstantBuffer<StarPush> g_push : register(b1, space0);

struct VSOut {
    float4 pos : SV_Position;
    float2 corner : TEXCOORD0;
    float3 color : COLOR0;
    float3 dir : TEXCOORD1;
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    ByteAddressBuffer stars = g_buffers[g_push.starsSrv];
    const float4 a = asfloat(stars.Load4(iid * 32));       // dir xyz, magnitude
    const float4 b = asfloat(stars.Load4(iid * 32 + 16));  // colour rgb
    const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
    VSOut o;
    o.corner = corners[vid];
    o.dir = mul((float3x3)g_frame.starRotation, a.xyz);  // inertial -> local
    float4 clip = mul(g_frame.viewProj, float4(o.dir, 0.0));  // a direction: depth 0, infinitely far
    if (o.dir.y < -0.03 || clip.w <= 1e-4) {
        o.pos = float4(0, 0, -1, 1);  // below the horizon or behind us: culled
        o.color = 0;
        return o;
    }
    const float mag = a.w;
    const float radiusPx = lerp(1.0, 2.1, saturate((3.5 - mag) / 5.0));
    clip.xy += o.corner * radiusPx * 2.0 * g_frame.viewport.zw * clip.w;
    clip.z = 0.0;
    o.pos = clip;
    // Flux by magnitude, the air's extinction toward the horizon, a faint twinkle low in the sky.
    const float twinkle = 1.0 + 0.25 * (1.0 - saturate(o.dir.y * 4.0)) * sin(g_frame.time.z * 9.0 + float(iid) * 1.37);
    o.color = b.rgb * pow(10.0, -0.4 * mag) * g_push.brightness * sunTransmittance(o.dir) * twinkle;
    return o;
}

float4 PSMain(VSOut i) : SV_Target0 {
    const PlanetFrame f = planetFrame();
    if (hitSphere(normalize(i.dir), f.c, f.R) > 0.0) discard;  // the planet hides what is behind it
    const float g = exp(-dot(i.corner, i.corner) * 3.0);
    return float4(i.color * g, 1.0);
}
