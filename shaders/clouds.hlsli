// Clouds as seen by other passes (step 3), from the simulation's one cloud map (sim/clouds.h).
//   float  cloudShadow(uint sun, float3 rel)  how much of sun `sun`'s light the clouds let through to a camera-relative point (0..1)
//   float  cloudOverhead(float3 rel)          how much light the clouds let through straight down onto a point (0..1)
//   float3 cloudMapAt(float2 relXZ)           the map at a camera-relative xz: coverage 0..1, base (m above sea level), thickness (m)
// Frame constants (written by engine/render/cloud_renderer.cpp):
//   clouds   xy: wind displacement (m), z: the day's cover, w: map texel size (m)
//   cloudMap xy: world xz of the map's corner relative to the camera, z: map size (texels, 0 until built),
//            w: thickness for the day's cover (m), the far field past the map's edge
//   atmosphere2 y: shadow map (r: toward sun 0, g: toward sun 1, b: straight up), z: map (rgb as cloudMapAt), w: clouds
// The shadow map: square, kCloudShadowTexel-metre texels on the world grid around the camera, light traced from sea
// level toward each sun through the cloud layer; a point at altitude h looks up where its sun ray meets sea level.
#ifndef TAELIA_CLOUDS_HLSLI
#define TAELIA_CLOUDS_HLSLI
#include "common.hlsli"

static const float kCloudShadowTexel = 32.0;  // divides 256 so the grid follows g_frame.cameraWrap exactly
static const float kCloudBottom = 1000.0, kCloudTop = 4700.0;  // m above sea level: the map's bases and tops lie within

float cloudCameraAltitude() { return g_frame.worldNorth.w; }

float3 cloudMapAt(float2 relXZ) {
    const float3 far = float3(g_frame.clouds.z, 1500.0, g_frame.cloudMap.w);
    if (g_frame.atmosphere2.z == 0 || g_frame.cloudMap.z <= 0) return far;
    const float2 uv = (relXZ - g_frame.cloudMap.xy) / (g_frame.clouds.w * g_frame.cloudMap.z);
    const float3 m = g_tex2D[g_frame.atmosphere2.z].SampleLevel(s_linearClamp, uv, 0).rgb;
    const float2 e = abs(uv - 0.5);
    return lerp(m, far, smoothstep(0.44, 0.5, max(e.x, e.y)));
}

// Camera-relative xz of the shadow map's corner (texel edges on multiples of kCloudShadowTexel in world space).
float2 cloudShadowCorner(float size) {
    const float2 cam = float2(g_frame.cameraWrap.x, g_frame.cameraWrap.z);
    return -fmod(cam, kCloudShadowTexel) - size * 0.5 * kCloudShadowTexel;
}

// Light let through past the map's reach: from the coverage alone.
float cloudShadowFar(float2 relXZ) {
    return lerp(1.0, 0.08, smoothstep(0.1, 0.7, cloudMapAt(relXZ).x));
}

float cloudShadowLookup(float3 dir, float3 rel, uint channel) {
    const float alt = rel.y + cloudCameraAltitude();
    const float2 p = rel.xz - dir.xz / max(dir.y, 0.05) * alt;  // where the ray toward the light leaves sea level
    if (g_frame.atmosphere2.y == 0) return cloudShadowFar(p + dir.xz / max(dir.y, 0.05) * 1800.0);
    float w, h;
    g_tex2D[g_frame.atmosphere2.y].GetDimensions(w, h);
    const float2 uv = (p - cloudShadowCorner(w)) / (w * kCloudShadowTexel);
    const float near = g_tex2D[g_frame.atmosphere2.y].SampleLevel(s_linearClamp, uv, 0)[channel];
    const float2 e = abs(uv - 0.5);
    const float edge = smoothstep(0.42, 0.5, max(e.x, e.y));
    if (edge <= 0) return near;
    return lerp(near, cloudShadowFar(p + dir.xz / max(dir.y, 0.05) * 1800.0), edge);
}

float cloudShadow(uint sun, float3 rel) {
    return cloudShadowLookup(g_frame.suns[sun].xyz, rel, sun);
}

float cloudOverhead(float3 rel) {
    return cloudShadowLookup(float3(0, 1, 0), rel, 2);
}

#endif
