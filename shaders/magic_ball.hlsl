// Magic balls (the user): condensed magic of one aspect between the player's hands or in flight. A camera-facing quad
// round each; the pixel's view ray is traced through the sphere: how much of it the ray crosses (thick in the middle,
// thin at the rim) times a swirl that moves inside, in the aspect's colour, added onto the lit scene. Fuller balls
// glow brighter; the magic view shows them brighter still. Each aspect has its own look.
#include "common.hlsli"
#include "magic.hlsli"
#include "sky.hlsli"

struct BallPush {
    uint pad0, instSrv, instOffset, pad1;
};
[[vk::push_constant]] ConstantBuffer<BallPush> g_push : register(b1, space0);

struct VSOut {
    float4 pos : SV_Position;
    float3 rel : TEXCOORD0;  // this point of the quad, relative to the camera
    nointerpolation float4 ball : TEXCOORD1;  // centre (relative to the camera), radius
    nointerpolation float3 info : TEXCOORD2;  // aspect, fill, seed
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    ByteAddressBuffer inst = g_buffers[g_push.instSrv];
    const uint base = g_push.instOffset + iid * 32;
    const float4 a = asfloat(inst.Load4(base)), b = asfloat(inst.Load4(base + 16));
    const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
    const float2 c = corners[vid];
    const float size = a.w * 2.4;  // room for the halo
    // The quad stands in front of the ball (toward the eye), so nothing just behind its centre hides it.
    const float3 toEye = -normalize(a.xyz);
    const float3 centre = a.xyz + toEye * min(a.w, length(a.xyz) * 0.5);
    VSOut o;
    o.rel = centre + (g_frame.cameraRight.xyz * c.x + g_frame.cameraUp.xyz * c.y) * size;
    o.pos = mul(g_frame.viewProj, float4(o.rel, 1));
    o.ball = a;
    o.info = b.xyz;
    return o;
}

float3 ballColor(uint aspect) {
    if (aspect == 7) return K_MAGIC_COLOR_NEUTRAL;
    if (aspect == 8) return K_MAGIC_COLOR_NECROTIC;
    return aspectColor(aspect + 1);
}

float4 PSMain(VSOut i) : SV_Target0 {
    const float3 dir = normalize(i.rel);
    const float3 c = i.ball.xyz;
    const float r = i.ball.w;
    const uint aspect = uint(i.info.x + 0.5);
    const float fill = i.info.y, seed = i.info.z;
    const float t = g_frame.fireInfo.x;
    // The ray through the sphere: closest approach, how much of it lies inside.
    const float along = dot(c, dir);
    const float3 closest = dir * along - c;
    const float d2 = dot(closest, closest), r2 = r * r;
    const float inside = d2 < r2 ? 2.0 * sqrt(r2 - d2) / (2.0 * r) : 0.0;  // 0 at the rim .. 1 through the middle
    const float rim = sqrt(d2) / r;                                         // 1 at the edge
    const float halo = exp(-max(rim - 1.0, 0.0) * 5.0) * smoothstep(2.3, 1.0, rim);
    // A swirl moving inside: the noise sampled where the ray is nearest the centre, turning with time.
    const float3 q = (closest / r) * 2.2;
    const float spin = t * (aspect == 3 ? 2.6 : (aspect == 4 ? 3.2 : 1.4));
    const float cs = cos(spin), sn = sin(spin);
    const float3 qr = float3(q.x * cs - q.z * sn, q.y + t * 0.3, q.x * sn + q.z * cs) + seed;
    const float n1 = latticeNoise3(qr * 1.7), n2 = latticeNoise3(qr * 3.9 + 7.0);
    float swirl = 0.55 + 0.45 * (n1 * 0.7 + n2 * 0.3);
    float3 col = ballColor(aspect);
    float glow = 2.2;
    if (aspect == 3) {  // fire: a hot flickering core
        col = lerp(float3(1.0, 0.35, 0.05), float3(1.0, 0.85, 0.45), saturate(inside * 1.2 * swirl));
        glow = 4.0 * (0.8 + 0.2 * sin(t * 23.0 + seed * 10.0));
    } else if (aspect == 5) {  // lightning: crackling threads
        swirl = 0.35 + 2.5 * pow(saturate(1.0 - abs(n1 * 2.0 - 1.0) * 6.0), 3.0) * (0.6 + 0.4 * sin(t * 40.0 + seed));
        glow = 3.0;
    } else if (aspect == 6) {  // light: blazing white
        glow = 5.0;
    } else if (aspect == 4) {  // air: faint streaks
        swirl = 0.3 + 0.9 * pow(saturate(n2), 2.0);
        glow = 1.4;
    } else if (aspect == 0) {  // earth: smouldering dust
        glow = 1.2;
    } else if (aspect == 8) {  // necrotic: grey-violet smoke
        swirl = 0.4 + 0.6 * n1;
        glow = 1.1;
    }
    const float dense = inside * swirl + halo * 0.25;
    float3 light = col * dense * glow * (0.35 + 0.65 * fill);
    if (magicView()) light *= 2.5;  // magic glows by its own light
    return float4(light, 0);
}

// ---------------------------------------------------------------- dust: a thing of pure life magic crumbled

struct DustOut {
    float4 pos : SV_Position;
    float2 corner : TEXCOORD0;
    nointerpolation float3 info : TEXCOORD1;  // age (0..1 of its life), seed, radius
};

DustOut DustVS(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    ByteAddressBuffer inst = g_buffers[g_push.instSrv];
    const uint base = g_push.instOffset + iid * 32;
    const float4 a = asfloat(inst.Load4(base)), b = asfloat(inst.Load4(base + 16));
    const float age = saturate(b.x / K_MAGIC_DUST_SECONDS);
    const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
    // It spreads and settles as it thins.
    const float size = a.w * (1.6 + 4.0 * sqrt(age));
    const float3 centre = a.xyz + float3(0, -0.3 * age * a.w * 4.0, 0);
    DustOut o;
    o.corner = corners[vid];
    o.pos = mul(g_frame.viewProj, float4(centre + (g_frame.cameraRight.xyz * o.corner.x + g_frame.cameraUp.xyz * o.corner.y) * size, 1));
    o.info = float3(age, b.z, a.w);
    return o;
}

float4 DustPS(DustOut i) : SV_Target0 {
    const float r2 = dot(i.corner, i.corner);
    if (r2 > 1.0) discard;
    const float age = i.info.x;
    const float3 q = float3(i.corner * 2.5, i.info.y + age * 1.5);
    const float n = latticeNoise3(q * 1.7) * 0.65 + latticeNoise3(q * 4.1 + 3.0) * 0.35;
    const float cover = saturate((1.0 - r2) * 1.4) * smoothstep(0.25, 0.75, n) * pow(1.0 - age, 1.5) * 0.85;
    // Dry grey-brown, lit by the sky and the suns.
    float3 light = skyAmbient(float3(0, 1, 0));
    [unroll] for (uint s = 0; s < 2; ++s) light += g_frame.sunLight[s].rgb * (1.0 - g_frame.suns[s].w) * sunTransmittance(g_frame.suns[s].xyz) * saturate(g_frame.suns[s].y) * 0.6;
    float3 col = float3(0.42, 0.38, 0.33) * light;
    if (magicView()) col = magicDim(col) + K_MAGIC_COLOR_NECROTIC * 0.08;  // what it held is gone
    return float4(col, cover);
}
