// What dense magic shows (step 5f). Minerals: clusters of amber crystals (earth); sprouts: small leafy plants (life),
// instanced into the G-buffer (they stand still: the motion pass works their motion out from the camera's). Fireflies:
// light, not insects: soft glowing sprites that drift and flicker, over the lit scene and behind nearer things.
#include "common.hlsli"
#include "gbuffer.hlsli"
#include "magic.hlsli"

struct ManifestPush {
    uint vbSrv, instSrv, instOffset, kind;  // kind 0 mineral, 1 sprout
};
[[vk::push_constant]] ConstantBuffer<ManifestPush> g_push : register(b1, space0);

struct VSOut {
    float4 pos : SV_Position;
    float3 normal : NORMAL;
    float3 local : TEXCOORD0;
    nointerpolation float fill : TEXCOORD1;
    nointerpolation uint seed : TEXCOORD2;
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    ByteAddressBuffer vb = g_buffers[g_push.vbSrv];
    const float3 p = asfloat(vb.Load3(vid * 24)), n = asfloat(vb.Load3(vid * 24 + 12));
    ByteAddressBuffer inst = g_buffers[g_push.instSrv];
    const uint base = g_push.instOffset + iid * 32;
    const float4 a = asfloat(inst.Load4(base));       // rel xyz, scale
    const float4 b = asfloat(inst.Load4(base + 16));  // cos yaw, sin yaw, fill, seed
    const float c = b.x, s = b.y;
    const float3 lp = p * a.w;
    VSOut o;
    o.pos = mul(g_frame.viewProj, float4(a.xyz + float3(lp.x * c + lp.z * s, lp.y, -lp.x * s + lp.z * c), 1));
    o.normal = float3(n.x * c + n.z * s, n.y, -n.x * s + n.z * c);
    o.local = p;
    o.fill = b.z;
    o.seed = asuint(b.w);
    return o;
}

GBufferOut PSMain(VSOut i, bool front : SV_IsFrontFace) {
    GBufferOut o;
    float3 n = normalize(i.normal);
    if (g_push.kind == 0) {
        // Amber crystal, deeper toward the foot, glassy.
        const float3 albedo = lerp(float3(0.30, 0.16, 0.06), float3(0.78, 0.52, 0.26), saturate(i.local.y * 1.2));
        o.albedo = float4(albedo, 1);
        o.normal = float4(n, 0.18);
        o.material = float4(0.15, 0.8, 0, packMagic(MAGIC_HELD_EARTH, i.fill, false));
        if (magicView()) o.albedo = magicViewAlbedo(albedo, heldMagicGlow(MAGIC_HELD_EARTH, i.fill, i.local.y * 3.0 + float(i.seed & 255u) * 0.02, 1.0));
    } else {
        if (!front) n = -n;
        o.albedo = float4(lerp(float3(0.10, 0.26, 0.06), float3(0.26, 0.52, 0.14), saturate(i.local.y * 2.5)), 1);
        o.normal = float4(n, 0.6);
        o.material = float4(0.45, 0.3, 0, packMagic(MAGIC_HELD_LIFE, i.fill, false));
        if (magicView()) o.albedo = magicViewAlbedo(o.albedo.rgb, heldMagicGlow(MAGIC_HELD_LIFE, i.fill, i.local.y * 4.0 + float(i.seed & 255u) * 0.02, 1.0));
    }
    o.motion = 0;  // standing still: the motion pass uses the camera's
    return o;
}

// ---------------------------------------------------------------------------------------------- fireflies

struct FlyOut {
    float4 pos : SV_Position;
    float2 corner : TEXCOORD0;
    nointerpolation float glow : TEXCOORD1;
};

FlyOut FireflyVS(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    ByteAddressBuffer inst = g_buffers[g_push.instSrv];
    const uint4 raw = inst.Load4(g_push.instOffset + iid * 16);
    const float3 rel = asfloat(raw.xyz);
    const bool big = (raw.w & 1u) != 0;
    const float seed = float(raw.w >> 8) * (1.0 / 16777216.0);
    const float t = g_frame.fireInfo.x;
    // They drift in slow loops and flicker.
    const float3 drift = float3(sin(t * 0.7 + seed * 40.0), sin(t * 0.9 + seed * 17.0) * 0.6, cos(t * 0.6 + seed * 29.0)) * (big ? 0.15 : 0.35);
    const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
    const float size = big ? 0.32 : 0.07;
    const float3 centre = rel + drift;
    FlyOut o;
    o.corner = corners[vid];
    o.pos = mul(g_frame.viewProj, float4(centre + (g_frame.cameraRight.xyz * o.corner.x + g_frame.cameraUp.xyz * o.corner.y) * size, 1));
    o.glow = (big ? 5.0 : 1.5) * (0.7 + 0.3 * sin(t * (5.0 + seed * 7.0) + seed * 60.0));
    return o;
}

float4 FireflyPS(FlyOut i) : SV_Target0 {
    const float r2 = dot(i.corner, i.corner);
    // A bright point in a soft halo that fades to nothing before the sprite's edge.
    const float edge = saturate(1.0 - r2);
    const float core = exp(-r2 * 40.0), halo = exp(-r2 * 6.0) * 0.12 * edge * edge;
    const float dim = g_frame.flags.x != 0 ? 0.35 : 1.0;  // the magic view shows them as light magic anyway
    return float4(float3(1.0, 0.94, 0.72) * (core + halo) * i.glow * dim, 0);
}
