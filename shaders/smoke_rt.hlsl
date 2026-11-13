// Shaders for tests/rhi/rhi_smoke.cpp, ray tracing and indirect part: GPU-written indirect arguments, inline ray
// queries against a TLAS of two instances (one opaque, one non-opaque accepted in the query loop).
#include "bindless.hlsli"

struct RtPush {
    uint tlas;        // bindless TLAS index
    uint outUav;      // RWTexture2D (RGBA8) 64x64
    uint argsUav;     // RWByteAddressBuffer for indirect arguments
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<RtPush> g_push : register(b1, space0);

// Writes dispatch args (8, 8, 1) at offset 0 and draw args (3 vertices, 1 instance, 0, 0) at offset 16.
[numthreads(1, 1, 1)]
void ArgsMain() {
    g_rwBuffers[g_push.argsUav].Store3(0, uint3(8, 8, 1));
    g_rwBuffers[g_push.argsUav].Store4(16, uint4(3, 1, 0, 0));
}

[numthreads(8, 8, 1)]
void TraceMain(uint3 id : SV_DispatchThreadID) {
    // Rays along +z from a 64x64 grid spanning x, y in [-1, 1] at z = -1.
    float2 p = (float2(id.xy) + 0.5) / 32.0 - 1.0;
    RayDesc ray;
    ray.Origin = float3(p.x, -p.y, -1.0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0.0;
    ray.TMax = 10.0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(g_tlas[g_push.tlas], RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) q.CommitNonOpaqueTriangleHit();
    }
    float4 c = float4(0, 0, 0, 1);
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        c = q.CommittedInstanceID() == 7 ? float4(1, 0, 0, 1) : float4(0, 1, 0, 1);
    }
    g_rwTex2D[g_push.outUav][id.xy] = c;
}

// Copies an RGBA16F texture (g_push.tlas = its srv) into an RGBA8 storage image (g_push.outUav) for readback.
[numthreads(8, 8, 1)]
void ConvertMain(uint3 id : SV_DispatchThreadID) {
    const float4 c = g_tex2D[g_push.tlas].Load(int3(id.xy, 0));
    g_rwTex2D[g_push.outUav][id.xy] = float4(saturate(c.rgb), 1);
}
