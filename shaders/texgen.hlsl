// Procedural ground textures, generated once on the GPU: tileable (periodic noise), linear albedo + height,
// tangent normal + roughness + occlusion. Mips come from downsample.hlsl.
#include "noise.hlsli"

struct TexPush {
    uint albedoUav;  // RGBA16F: rgb albedo, a height
    uint detailUav;  // RGBA8: xy normal, z roughness, w occlusion
    uint size;
    uint material;   // 0 grass ground, 1 dirt, 2 rock, 3 sand
};
[[vk::push_constant]] ConstantBuffer<TexPush> g_push : register(b1, space0);

struct Ground {
    float3 albedo;
    float height;
    float rough;
    float ao;
};

Ground grassGround(float2 uv) {
    float patches = fbmP(uv, int2(6, 6), 5, 0.5, 11u);
    float fine = fbmP(uv, int2(48, 48), 3, 0.5, 12u);
    float streaks = smoothstep(0.1, 0.6, fbmP(uv, int2(96, 24), 3, 0.55, 13u));  // flattened dead blades
    Ground g;
    float3 soil = float3(0.105, 0.080, 0.052), green = float3(0.070, 0.095, 0.038), dry = float3(0.19, 0.16, 0.085);
    g.albedo = lerp(soil, green, smoothstep(-0.35, 0.35, patches));
    g.albedo = lerp(g.albedo, dry, streaks * 0.55) * (0.85 + 0.3 * (fine * 0.5 + 0.5));
    g.height = saturate(0.5 + 0.25 * patches + 0.15 * fine + 0.2 * streaks);
    g.rough = 0.9;
    g.ao = 0.75 + 0.25 * g.height;
    return g;
}

Ground dirt(float2 uv) {
    float n = fbmP(uv, int2(8, 8), 5, 0.5, 21u);
    float3 w = worleyP(uv, 40, 22u);
    float pebble = smoothstep(0.32, 0.15, w.x) * step(0.86, w.z);  // small, scattered stones
    Ground g;
    g.albedo = float3(0.155, 0.115, 0.075) * (0.8 + 0.3 * (n * 0.5 + 0.5));
    g.albedo = lerp(g.albedo, float3(0.16, 0.145, 0.12) * (0.7 + 0.4 * w.z), pebble);
    g.height = saturate(0.3 + 0.2 * n + pebble * (0.4 + 0.3 * w.z));
    g.rough = lerp(0.88, 0.65, pebble);
    g.ao = 0.7 + 0.3 * g.height;
    return g;
}

Ground rock(float2 uv) {
    float big = fbmP(uv, int2(3, 3), 6, 0.55, 31u);
    float3 w = worleyP(uv + 0.03 * float2(big, -big), 9, 32u);
    float crack = 1.0 - smoothstep(0.0, 0.05, w.y - w.x);
    float strata = sin((uv.y + 0.08 * big) * 6.2831853 * 7.0) * 0.5 + 0.5;
    float lichen = smoothstep(0.35, 0.6, fbmP(uv, int2(12, 12), 4, 0.5, 33u));
    Ground g;
    g.albedo = lerp(float3(0.21, 0.20, 0.19), float3(0.31, 0.30, 0.28), saturate(big * 0.5 + 0.5)) * (0.9 + 0.1 * strata);
    g.albedo = lerp(g.albedo, float3(0.14, 0.15, 0.11), lichen * 0.45);
    g.albedo *= 1.0 - 0.6 * crack;
    g.height = saturate(0.55 + 0.35 * big + 0.08 * strata - 0.5 * crack);
    g.rough = 0.72 + 0.15 * lichen;
    g.ao = 1.0 - 0.6 * crack;
    return g;
}

Ground sand(float2 uv) {
    float warp = fbmP(uv, int2(4, 4), 3, 0.5, 41u);
    float ripple = sin((uv.x * 14.0 + uv.y * 3.0 + warp * 1.5) * 6.2831853) * 0.5 + 0.5;
    float grain = fbmP(uv, int2(128, 128), 2, 0.5, 42u);
    Ground g;
    g.albedo = float3(0.27, 0.235, 0.175) * (0.9 + 0.12 * grain) * (0.95 + 0.05 * ripple);
    g.height = saturate(0.45 + 0.25 * ripple + 0.1 * grain + 0.1 * warp);
    g.rough = 0.93;
    g.ao = 0.9 + 0.1 * ripple;
    return g;
}

Ground evalGround(uint m, float2 uv) {
    if (m == 0) return grassGround(uv);
    if (m == 1) return dirt(uv);
    if (m == 2) return rock(uv);
    return sand(uv);
}

[numthreads(8, 8, 1)]
void GroundMain(uint3 id : SV_DispatchThreadID) {
    const uint size = g_push.size;
    if (any(id.xy >= size)) return;
    const float e = 1.0 / float(size);
    const float2 uv = (float2(id.xy) + 0.5) * e;
    Ground g = evalGround(g_push.material, uv);
    const float hx = evalGround(g_push.material, uv + float2(e, 0)).height - evalGround(g_push.material, uv - float2(e, 0)).height;
    const float hy = evalGround(g_push.material, uv + float2(0, e)).height - evalGround(g_push.material, uv - float2(0, e)).height;
    const float bump = g_push.material == 2 ? 5.0 : 3.0;  // rock stands out more
    const float3 n = normalize(float3(-hx * bump * float(size) / 256.0, -hy * bump * float(size) / 256.0, 1.0));
    g_rwTex2D[g_push.albedoUav][id.xy] = float4(g.albedo, g.height);
    g_rwTex2D[g_push.detailUav][id.xy] = float4(n.xy * 0.5 + 0.5, g.rough, g.ao);
}
