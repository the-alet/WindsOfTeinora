// Helpers shared by the passes.
#ifndef TAELIA_COMMON_HLSLI
#define TAELIA_COMMON_HLSLI

#include "bindless.hlsli"

struct FullscreenOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;  // 0..1, y down
};

FullscreenOut fullscreenTriangle(uint id) {
    FullscreenOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// Camera-relative world direction through a screen uv.
float3 viewRay(float2 uv) {
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float t = g_frame.cameraForward.w;
    float aspect = g_frame.cameraRight.w;
    return normalize(g_frame.cameraForward.xyz + ndc.x * t * aspect * g_frame.cameraRight.xyz + ndc.y * t * g_frame.cameraUp.xyz);
}

// Reverse-Z depth of a camera-relative point (infinite far plane): near / view depth.
float reverseDepth(float3 rel) {
    float viewZ = dot(rel, g_frame.cameraForward.xyz);
    return g_frame.cameraUp.w / max(viewZ, 1e-4);
}

float3 srgbEncode(float3 c) {
    c = saturate(c);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

// Khronos PBR Neutral: hue-preserving, a soft shoulder, no harsh overexposure.
float3 tonemapNeutral(float3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(color, newPeak.xxx, g);
}

// Interleaved gradient noise (per pixel, 0..1).
float ign(float2 px) {
    return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
}

uint pcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// A burning tree loses its leaves where this falls below how far it has charred (p: the tree mesh's own space, so the
// G-buffer and the shadow rays agree). 0.05..0.95, patches about a metre across.
float leafBurnNoise(float3 p) {
    const float3 q = p * 1.1;
    const float3 i = floor(q), f = q - i, u = f * f * (3.0 - 2.0 * f);
    const int3 c = int3(i);
    float v[8];
    [unroll] for (uint k = 0; k < 8; ++k) {
        const int3 o = int3(k & 1u, (k >> 1) & 1u, (k >> 2) & 1u);
        v[k] = float(pcgHash(uint(c.x + o.x) * 73856093u ^ uint(c.y + o.y) * 19349663u ^ uint(c.z + o.z) * 83492791u) & 1023u) / 1023.0;
    }
    const float n = lerp(lerp(lerp(v[0], v[1], u.x), lerp(v[2], v[3], u.x), u.y), lerp(lerp(v[4], v[5], u.x), lerp(v[6], v[7], u.x), u.y), u.z);
    return 0.05 + 0.9 * n;
}

// Where fire burnt the grass away (frame constants scorch): 1 inside a circle, fading to 0 over a ragged edge.
// rel: a point on the ground relative to the camera.
float scorchAt(float3 rel) {
    if (g_frame.scorch[0].w <= 0.0) return 0.0;
    // A ragged edge: value noise on the world position, about 0.7 m across.
    const float2 q = (g_frame.cameraWrap.xz + rel.xz) * 1.4;
    const float2 i = floor(q), f = q - i, u = f * f * (3.0 - 2.0 * f);
    const int2 c = int2(i);
    const float n = lerp(lerp(float(pcgHash(uint(c.x) * 73856093u ^ uint(c.y) * 19349663u) & 1023u),
                              float(pcgHash(uint(c.x + 1) * 73856093u ^ uint(c.y) * 19349663u) & 1023u), u.x),
                         lerp(float(pcgHash(uint(c.x) * 73856093u ^ uint(c.y + 1) * 19349663u) & 1023u),
                              float(pcgHash(uint(c.x + 1) * 73856093u ^ uint(c.y + 1) * 19349663u) & 1023u), u.x), u.y) / 1023.0;
    float burnt = 0.0;
    [unroll] for (uint k = 0; k < 8; ++k) {
        const float4 s = g_frame.scorch[k];
        if (s.w <= 0.0) break;
        const float d = length(rel.xz - s.xz) + (n - 0.5) * 0.6 * s.w;
        burnt = max(burnt, saturate((s.w - d) / 0.5));
    }
    return burnt;
}

#endif
