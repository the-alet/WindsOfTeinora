// Shaders for tests/rhi/rhi_smoke.cpp: exercise bindless buffers, textures, storage images, frame and push constants.
#include "bindless.hlsli"

struct SmokePush {
    uint bufferSrv;      // ByteAddressBuffer with a float4
    uint textureSrv;     // Texture2D sampled with s_pointClamp
    uint storageSrv;     // Texture2D written by CSMain
    uint storageUav;     // RWTexture2D for CSMain
    float4 quadColor;    // used by the indexed quad
    uint mode;           // 0: fullscreen triangle, 1: indexed quad on the left half
    uint vertexBuffer;   // ByteAddressBuffer of float2 positions for mode 1
    uint2 pad;
};
[[vk::push_constant]] ConstantBuffer<SmokePush> g_push : register(b1, space0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    if (g_push.mode == 0) {
        o.uv = float2((id << 1) & 2, id & 2);
        o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
    } else {
        float2 p = g_buffers[g_push.vertexBuffer].Load<float2>(id * 8);
        o.uv = p;
        o.pos = float4(p, 0.75, 1);  // nearer in reverse-Z: passes GreaterEqual over the triangle at 0.5
    }
    return o;
}

float4 PSMain(VSOut i) : SV_Target0 {
    if (g_push.mode == 1) return g_push.quadColor;
    float4 b = g_buffers[g_push.bufferSrv].Load<float4>(0);
    float4 t = g_tex2D[g_push.textureSrv].SampleLevel(s_pointClamp, i.uv, 0);
    float4 s = g_tex2D[g_push.storageSrv].SampleLevel(s_linearClamp, i.uv, 0);
    // g_frame.ambient.a carries a test value from the frame constants.
    return float4(b.x, t.g, s.b, g_frame.ambient.a);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    g_rwTex2D[g_push.storageUav][id.xy] = float4(0, 0, 0.5, 1);
}
