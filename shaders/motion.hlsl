// Screen motion for DLSS: where each pixel was last frame (previous uv - current uv), camera motion only,
// rebuilt from depth (the sky moves with the camera's turn alone). Surfaces that move by themselves (balls) write
// their own motion into the G-buffer and flag it in material.a.
#include "common.hlsli"

struct MotionPush {
    uint depthSrv, outUav, width, height;
    uint gbMotionSrv, gbMaterialSrv, pad0, pad1;
};
[[vk::push_constant]] ConstantBuffer<MotionPush> g_push : register(b1, space0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_push.width || id.y >= g_push.height) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(g_push.width, g_push.height);
    float2 motion = 0;
    if (g_frame.cameraDelta.w > 0 && g_tex2D[g_push.gbMaterialSrv].Load(int3(id.xy, 0)).a > 0.5) {
        motion = g_tex2D[g_push.gbMotionSrv].Load(int3(id.xy, 0)).xy;
    } else if (g_frame.cameraDelta.w > 0) {
        const float depth = g_tex2D[g_push.depthSrv].Load(int3(id.xy, 0)).r;
        const float3 dir = viewRay(uv);
        float4 prevClip;
        if (depth > 0) {
            const float viewZ = g_frame.cameraUp.w / depth;
            const float3 rel = dir * (viewZ / dot(dir, g_frame.cameraForward.xyz));
            prevClip = mul(g_frame.prevViewProj, float4(rel + g_frame.cameraDelta.xyz, 1));
        } else {
            prevClip = mul(g_frame.prevViewProj, float4(dir, 0));  // a direction: only the turn moves it
        }
        if (prevClip.w > 1e-6) {
            const float2 prevUv = prevClip.xy / prevClip.w * float2(0.5, -0.5) + 0.5;
            motion = prevUv - uv;
        }
    }
    g_rwTex2D[g_push.outUav][id.xy] = float4(motion, 0, 0);
}
