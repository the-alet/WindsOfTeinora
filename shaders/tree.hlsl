// Trees: vertices pulled from one buffer, instances turned and scaled, swaying in the simulation's wind.
// Bark is opaque; leaf cards are alpha-tested, two-sided and let light through.
#include "common.hlsli"
#include "gbuffer.hlsli"

struct TreePush {
    uint vbSrv, instSrv, instOffset, kind;
    uint albedo, detail;
    float tint, uvScale;  // albedo multiplier (charred wood), texture repeats
};
[[vk::push_constant]] ConstantBuffer<TreePush> g_push : register(b1, space0);

struct VSOut {
    float4 pos : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 rel : TEXCOORD1;
    float3 local : TEXCOORD2;                  // the mesh position (leaves burn away by it)
    nointerpolation float charred : TEXCOORD3;  // how far fire has charred this tree (0..1)
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    ByteAddressBuffer vb = g_buffers[g_push.vbSrv];
    const uint base = vid * 32;
    const float3 p = asfloat(vb.Load3(base));
    const float3 n = decodeOctNormal(vb.Load(base + 12));
    const float2 uv = asfloat(vb.Load2(base + 16));
    const uint info = vb.Load(base + 24);
    const float4 ps = asfloat(g_buffers[g_push.instSrv].Load4(g_push.instOffset + iid * 32));
    const float4 rot = asfloat(g_buffers[g_push.instSrv].Load4(g_push.instOffset + iid * 32 + 16));

    // Turn by the instance's yaw (from +z toward +x) and scale.
    const float c = rot.x, s = rot.y;
    const float3 lp = p * ps.w;
    float3 wp = float3(lp.x * c + lp.z * s, lp.y, -lp.x * s + lp.z * c);
    const float3 wn = float3(n.x * c + n.z * s, n.y, -n.x * s + n.z * c);

    // The wind bends the crown downwind in slow gusts; leaves flutter.
    const float w = float(info & 255u) / 255.0;
    const float t = g_frame.time.z;
    const float2 wind = g_frame.wind.xz;
    const float gust = 0.65 + 0.35 * sin(t * 0.6 + rot.z) * sin(t * 0.23 + rot.z * 1.7);
    wp.xz += wind * (0.010 * gust * w * ps.w);
    if (info & 256u) {
        const float flutter = sin(t * 5.3 + rot.z * 3.0 + p.y * 2.1 + p.x * 1.7) * 0.025 * w * saturate(length(wind) / 5.0 + 0.15);
        wp += wn * flutter;
    }

    VSOut o;
    o.rel = ps.xyz + wp;
    o.pos = mul(g_frame.viewProj, float4(o.rel, 1));
    if ((info & 256u) && rot.w >= 0.999) o.pos = float4(-2, -2, 0, 1);  // burnt through: no leaves left (culled)
    o.normal = wn;
    o.uv = uv;
    o.local = p;
    o.charred = rot.w;
    return o;
}

GBufferOut BarkPS(VSOut i) {
    const float2 uv = i.uv * g_push.uvScale;
    float4 a = g_tex2D[g_push.albedo].Sample(s_anisoWrap, uv) * float4(g_push.tint.xxx, 1);
    a.rgb = lerp(a.rgb, a.rgb * 0.1 + 0.01, i.charred);  // charcoal
    const float4 d = g_tex2D[g_push.detail].Sample(s_anisoWrap, uv);
    const float3 N = normalize(i.normal);
    const float3 T = normalize(cross(float3(0, 1, 0), N) + float3(1e-4, 0, 0));
    const float3 B = cross(N, T);
    const float2 xy = d.xy * 2.0 - 1.0;
    const float3 n = normalize(N * sqrt(saturate(1.0 - dot(xy, xy))) + T * xy.x + B * xy.y);
    GBufferOut o;
    o.albedo = float4(a.rgb, d.w);
    o.normal = float4(n, d.z);
    o.material = float4(0, 0.3, 0, packMagic(MAGIC_HELD_NATURE, 1.0 - i.charred, false));
    o.motion = 0;
    return o;
}

GBufferOut LeafPS(VSOut i, bool front : SV_IsFrontFace) {
    Texture2D<float4> tex = g_tex2D[g_push.albedo];
    float4 a = tex.Sample(s_linearClamp, i.uv);
    // Mips average the alpha away; push it back up so far crowns don't thin out.
    const float lod = tex.CalculateLevelOfDetail(s_linearClamp, i.uv);
    a.a *= 1.0 + 0.3 * max(lod, 0.0);
    if (a.a < 0.5) discard;
    // Fire takes the leaves patch by patch, browning them first (rt.hlsli does the same for shadows).
    if (i.charred > 0.0) {
        const float n = leafBurnNoise(i.local);
        if (i.charred > n) discard;
        a.rgb = lerp(a.rgb, float3(0.13, 0.075, 0.03), saturate((i.charred - n + 0.3) / 0.3));
    }
    const float4 d = g_tex2D[g_push.detail].Sample(s_linearClamp, i.uv);
    float3 N = normalize(i.normal) * (front ? 1.0 : -1.0);
    const float2 xy = d.xy * 2.0 - 1.0;
    const float3 T = normalize(cross(float3(0, 1, 0), N) + float3(1e-4, 0, 0));
    N = normalize(N + T * xy.x * 0.5 + cross(N, T) * xy.y * 0.5 + float3(0, 0.3, 0));  // a little bent toward the sky
    GBufferOut o;
    o.albedo = float4(a.rgb, 1.0);
    o.normal = float4(N, d.z);
    o.material = float4(d.w, 0.35, 0, packMagic(MAGIC_HELD_NATURE, 1.0 - i.charred, false));  // translucency
    o.motion = 0;
    return o;
}
