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

#endif
