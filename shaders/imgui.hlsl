// Dear ImGui over the bindless model: vertices pulled from the upload ring (ImDrawVert: pos, uv, rgba8).
#include "bindless.hlsli"

struct ImGuiPush {
    float2 scale;
    float2 translate;
    uint vbSrv;
    uint vbOffset;  // bytes: ring offset + vertex offset of this draw
    uint texSrv;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<ImGuiPush> g_push : register(b1, space0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

VSOut VSMain(uint id : SV_VertexID) {
    ByteAddressBuffer vb = g_buffers[g_push.vbSrv];
    uint addr = g_push.vbOffset + id * 20;
    float2 pos = asfloat(vb.Load2(addr));
    float2 uv = asfloat(vb.Load2(addr + 8));
    uint c = vb.Load(addr + 16);
    VSOut o;
    o.pos = float4(pos * g_push.scale + g_push.translate, 0, 1);
    o.uv = uv;
    o.col = float4(c & 255u, (c >> 8) & 255u, (c >> 16) & 255u, c >> 24) / 255.0;
    return o;
}

float4 PSMain(VSOut i) : SV_Target0 {
    return i.col * g_tex2D[g_push.texSrv].Sample(s_linearClamp, i.uv);
}
