// Thrown balls: a unit sphere pulled from a buffer, instanced per ball (turned by its quaternion), written into the
// G-buffer with its own screen motion (this frame's pose against last frame's) for DLSS.
#include "common.hlsli"
#include "gbuffer.hlsli"
#include "ball.hlsli"

struct BallPush {
    uint vbSrv, instSrv, instOffset;
    float radius;
};
[[vk::push_constant]] ConstantBuffer<BallPush> g_push : register(b1, space0);

struct VSOut {
    float4 pos : SV_Position;
    float3 normal : NORMAL;
    float3 local : TEXCOORD0;
    float4 curClip : TEXCOORD1;   // unjittered, this frame
    float4 prevClip : TEXCOORD2;  // last frame
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    const float3 p = asfloat(g_buffers[g_push.vbSrv].Load3(vid * 12));
    ByteAddressBuffer inst = g_buffers[g_push.instSrv];
    const uint base = g_push.instOffset + iid * 64;
    const float3 rel = asfloat(inst.Load3(base));
    const float4 q = asfloat(inst.Load4(base + 16));
    const float3 prevRel = asfloat(inst.Load3(base + 32));
    const float4 prevQ = asfloat(inst.Load4(base + 48));

    VSOut o;
    o.normal = rotateByQuat(q, p);
    o.local = p;
    o.pos = mul(g_frame.viewProj, float4(rel + o.normal * g_push.radius, 1));
    // The scene renders jittered (DLSS); motion is measured without it.
    o.curClip = o.pos;
    o.curClip.x -= 2.0 * g_frame.jitter.x / g_frame.jitter.z * o.pos.w;
    o.curClip.y += 2.0 * g_frame.jitter.y / g_frame.jitter.w * o.pos.w;
    o.prevClip = mul(g_frame.prevViewProj, float4(prevRel + rotateByQuat(prevQ, p) * g_push.radius, 1));
    return o;
}

GBufferOut PSMain(VSOut i) {
    const float3 local = normalize(i.local);
    const float4 a = ballAlbedo(local, fwidth(ballSeam(local)));
    float2 motion = 0;
    if (g_frame.cameraDelta.w > 0 && i.prevClip.w > 1e-6) {
        const float2 cur = i.curClip.xy / i.curClip.w * float2(0.5, -0.5) + 0.5;
        const float2 prev = i.prevClip.xy / i.prevClip.w * float2(0.5, -0.5) + 0.5;
        motion = prev - cur;
    }
    GBufferOut o;
    o.albedo = float4(a.rgb, 1.0 - 0.4 * a.w);  // the seam groove is a little shaded
    o.normal = float4(normalize(i.normal), BALL_ROUGHNESS);
    o.material = float4(0, BALL_SPECULAR, 0, packMagic(MAGIC_HELD_NATURE, 1.0, true));  // its own motion in the motion target
    o.motion = motion;
    return o;
}
