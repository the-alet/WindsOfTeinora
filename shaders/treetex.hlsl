// Generated bark (tileable) and leaf-card textures for the three invented species.
// Bark: albedo + height, tangent normal + roughness + occlusion. Leaves: albedo + alpha, normal + roughness + translucency.
#include "noise.hlsli"

struct TreeTexPush {
    uint albedoUav, detailUav, size, kind;  // kind 0 Veyl, 1 Ombrel, 2 Sarn
    uint leaf, pad0, pad1, pad2;
};
[[vk::push_constant]] ConstantBuffer<TreeTexPush> g_push : register(b1, space0);

struct Tex {
    float3 albedo;
    float height;   // bark: relief; leaves: alpha
    float rough;
    float extra;    // bark: occlusion; leaves: translucency
    float2 slope;   // leaves: normal tilt
};

Tex bark(float2 uv, uint kind) {
    Tex t;
    t.slope = 0;
    if (kind == 0) {  // pale, smooth, dark dashes across the trunk
        float n = fbmP(uv, int2(4, 8), 4, 0.5, 51u);
        float dash = smoothstep(0.55, 0.8, noiseP(uv * float2(5, 70), int2(5, 70), 52u)) * smoothstep(-0.2, 0.3, fbmP(uv, int2(8, 16), 2, 0.5, 53u));
        float patchy = smoothstep(0.35, 0.7, fbmP(uv, int2(3, 6), 3, 0.5, 54u));
        t.albedo = float3(0.50, 0.49, 0.43) * (0.85 + 0.25 * n);
        t.albedo = lerp(t.albedo, float3(0.22, 0.21, 0.19), patchy * 0.6);
        t.albedo = lerp(t.albedo, float3(0.08, 0.07, 0.06), dash);
        t.height = saturate(0.55 + 0.2 * n - 0.45 * dash);
        t.rough = 0.55;
        t.extra = 1.0 - 0.4 * dash;
    } else if (kind == 1) {  // dark, deep vertical furrows
        float r = 1.0 - abs(noiseP(uv * float2(9, 2), int2(9, 2), 55u) + 0.35 * noiseP(uv * float2(18, 5), int2(18, 5), 56u));
        r = pow(saturate(r), 2.5);
        float cross = smoothstep(0.6, 0.85, noiseP(uv * float2(9, 22), int2(9, 22), 57u));
        float n = fbmP(uv, int2(8, 8), 3, 0.5, 58u);
        t.albedo = lerp(float3(0.045, 0.037, 0.030), float3(0.19, 0.15, 0.11), r) * (0.85 + 0.3 * (n * 0.5 + 0.5));
        t.height = saturate(r - 0.3 * cross);
        t.rough = 0.85;
        t.extra = 0.45 + 0.55 * r;
    } else {  // reddish, flaking plates
        float3 w = worleyP(uv, 7, 59u);
        float edge = 1.0 - smoothstep(0.0, 0.09, w.y - w.x);
        float n = fbmP(uv, int2(6, 6), 3, 0.5, 60u);
        t.albedo = float3(0.27, 0.15, 0.095) * (0.75 + 0.45 * w.z) * (0.9 + 0.2 * n);
        t.albedo = lerp(t.albedo, float3(0.07, 0.045, 0.03), edge);
        t.height = saturate(0.65 - 0.55 * edge + 0.15 * w.z);
        t.rough = 0.8;
        t.extra = 1.0 - 0.5 * edge;
    }
    return t;
}

// Signed distance from p to segment ab, and the position along it (0..1).
float2 segmentDistance(float2 p, float2 a, float2 b) {
    float2 ab = b - a;
    float s = saturate(dot(p - a, ab) / dot(ab, ab));
    return float2(length(p - (a + ab * s)), s);
}

Tex leaves(float2 uv, uint kind) {
    Tex t;
    t.albedo = 0;
    t.height = 0;  // alpha
    t.rough = 0.55;
    t.extra = 0.6;
    t.slope = 0;
    // A slightly curved twig from the bottom up the card.
    const float2 tw0 = float2(0.5, 0.98), tw1 = float2(0.47, 0.55), tw2 = float2(0.53, 0.06);
    const float2 d0 = segmentDistance(uv, tw0, tw1), d1 = segmentDistance(uv, tw1, tw2);
    const float twigW = kind == 2 ? 0.012 : 0.008;
    if (min(d0.x, d1.x) < twigW) {
        t.albedo = float3(0.10, 0.07, 0.045);
        t.height = 1;
        t.rough = 0.8;
        t.extra = 0.1;
    }
    const uint count = kind == 0 ? 18u : (kind == 1 ? 11u : 120u);
    for (uint i = 0; i < count; ++i) {
        const uint h = pcgHash(i * 7919u + kind * 104729u + 17u);
        const float r0 = float(h & 255u) / 255.0, r1 = float((h >> 8) & 255u) / 255.0, r2 = float((h >> 16) & 255u) / 255.0;
        const float along = (float(i) + 0.5) / float(count);
        const float2 base = along < 0.5 ? lerp(tw0, tw1, along * 2.0) : lerp(tw1, tw2, along * 2.0 - 1.0);
        const float side = (i & 1u) ? 1.0 : -1.0;
        float len, width, angle;
        if (kind == 0) { len = 0.27 + 0.08 * r0; width = 0.46; angle = 0.75 + 0.35 * r1; }
        else if (kind == 1) { len = 0.40 + 0.1 * r0; width = 0.42; angle = 0.7 + 0.4 * r1; }
        else { len = 0.24 + 0.06 * r0; width = 0.06; angle = 0.8 + 0.6 * r1; }
        len *= 1.0 - 0.35 * along;  // smaller toward the tip
        const float a = angle * side;
        const float2 dir = float2(sin(a), -cos(a));  // up the card is -v
        const float2 rel = uv - base;
        const float s = dot(rel, dir) / len;
        const float q = dot(rel, float2(-dir.y, dir.x)) / len;
        if (s <= 0 || s >= 1) continue;
        float profile = width * pow(sin(3.14159 * s), kind == 1 ? 0.55 : 0.7);
        if (kind == 1) profile *= 1.0 + 0.18 * sin(s * 3.14159 * 5.0);  // lobes
        if (abs(q) >= profile * 0.5) continue;
        const float across = q / (profile * 0.5);  // -1..1
        const float rib = 1.0 - smoothstep(0.0, 0.08, abs(across));
        float3 green = kind == 0 ? float3(0.19, 0.27, 0.07) : (kind == 1 ? float3(0.075, 0.15, 0.04) : float3(0.045, 0.10, 0.065));
        green *= 0.8 + 0.4 * r2;
        t.albedo = lerp(green, green * 1.35 + 0.02, rib * 0.6);
        t.height = 1;
        t.rough = kind == 2 ? 0.6 : 0.45;
        t.extra = kind == 2 ? 0.3 : 0.65;
        t.slope = float2(-dir.y, dir.x) * across * 0.5;  // leaves curve away from the midrib
    }
    return t;
}

[numthreads(8, 8, 1)]
void TreeTexMain(uint3 id : SV_DispatchThreadID) {
    const uint size = g_push.size;
    if (any(id.xy >= size)) return;
    const float e = 1.0 / float(size);
    const float2 uv = (float2(id.xy) + 0.5) * e;
    if (g_push.leaf != 0) {
        const Tex t = leaves(uv, g_push.kind);
        const float3 n = normalize(float3(t.slope, 1.0));
        g_rwTex2D[g_push.albedoUav][id.xy] = float4(t.albedo, t.height);
        g_rwTex2D[g_push.detailUav][id.xy] = float4(n.xy * 0.5 + 0.5, t.rough, t.extra);
        return;
    }
    const Tex t = bark(uv, g_push.kind);
    const float hx = bark(uv + float2(e, 0), g_push.kind).height - bark(uv - float2(e, 0), g_push.kind).height;
    const float hy = bark(uv + float2(0, e), g_push.kind).height - bark(uv - float2(0, e), g_push.kind).height;
    const float bump = g_push.kind == 1 ? 6.0 : 3.0;
    const float3 n = normalize(float3(-hx * bump * float(size) / 256.0, -hy * bump * float(size) / 256.0, 1.0));
    g_rwTex2D[g_push.albedoUav][id.xy] = float4(t.albedo, t.height);
    g_rwTex2D[g_push.detailUav][id.xy] = float4(n.xy * 0.5 + 0.5, t.rough, t.extra);
}

// The grass blade texture (u across, v from the base to the tip): darker base, green middle, drier tip, fine veins.
[numthreads(8, 8, 1)]
void BladeMain(uint3 id : SV_DispatchThreadID) {
    const uint w = g_push.size, h = g_push.kind;  // here: width, height
    if (id.x >= w || id.y >= h) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    const float3 base = float3(0.045, 0.07, 0.02), mid = float3(0.08, 0.14, 0.03), tip = float3(0.14, 0.155, 0.05);
    float3 c = uv.y < 0.5 ? lerp(base, mid, uv.y * 2.0) : lerp(mid, tip, (uv.y - 0.5) * 2.0);
    const float veins = 0.5 + 0.5 * sin(uv.x * 3.14159 * 7.0);
    c *= 0.9 + 0.15 * veins;
    const float slope = cos(uv.x * 3.14159 * 7.0) * 0.25;
    g_rwTex2D[g_push.albedoUav][id.xy] = float4(c, 1);
    g_rwTex2D[g_push.detailUav][id.xy] = float4(0.5 + slope * 0.5, 0.5, 0.5, 0.45);
}
