// Light in pond water, shared by the water surface (seen from above or from below) and the lighting pass (things seen
// from under the surface): the water takes red first and glows a little with the daylight it scatters, dimmer deeper.
#ifndef TAELIA_WATER_OPTICS_HLSLI
#define TAELIA_WATER_OPTICS_HLSLI

#include "common.hlsli"
#include "atmosphere.hlsli"

bool cameraUnderwater() { return g_frame.underwater.x > 0.0; }
float waterSurfaceAbove() { return g_frame.underwater.y; }  // how far the pond's surface is above the eye (m)

// The daylight falling on the water: the sky, and a quarter of the suns (the rest goes on down as their beams).
float3 waterDaylight() {
    float3 suns = 0;
    [unroll] for (uint s = 0; s < 2; ++s)
        suns += g_frame.sunLight[s].rgb * (1.0 - g_frame.suns[s].w) * sunTransmittance(g_frame.suns[s].xyz) * saturate(g_frame.suns[s].y);
    return skyIrradiance(float3(0, 1, 0)) + suns * 0.25;
}

// The water's own glow toward the eye at a depth under the surface.
float3 waterGlow(float depthBelow) {
    return K_WATER_COLOR * waterDaylight() * exp(-K_WATER_EXTINCTION * max(depthBelow, 0.0));
}

// What is seen through `path` metres of water at about this depth under the surface.
float3 throughWater(float3 color, float path, float depthBelow) {
    const float3 T = exp(-K_WATER_EXTINCTION * max(path, 0.0));
    return color * T + waterGlow(depthBelow) * (1.0 - T);
}

// Daylight left at a depth under the surface (things down there are lit by what the water above lets through).
float3 waterDepthLight(float depthBelow) { return exp(-K_WATER_EXTINCTION * max(depthBelow, 0.0)); }

// A net of bright lines: the borders between cells (`period` of them per 256 m, so the camera wrap stays seamless), each
// cell's point drifting round its own small circle so the net slowly writhes.
float causticNet(float2 p, int period, float t, uint salt) {
    const float2 i = floor(p), f = p - i;
    float d1 = 9.0, d2 = 9.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2 c = ((int2(i) + int2(x, y)) % period + period) % period;
            const uint h = pcgHash(uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ salt);
            const float2 base = float2(float(h & 1023u), float((h >> 10) & 1023u)) / 1023.0 * 0.6 + 0.2;
            const float phase = float((h >> 20) & 1023u) / 1023.0 * 6.2831853;
            const float2 q = float2(x, y) + base + 0.22 * float2(cos(t + phase), sin(t * 1.3 + phase)) - f;
            const float d = dot(q, q);
            d2 = d < d1 ? d1 : min(d2, d);
            d1 = min(d1, d);
        }
    return pow(saturate(1.0 - (sqrt(d2) - sqrt(d1)) * 3.5), 3.0);
}

// Sunlight on the bed, gathered by the rippling surface into a slowly writhing net of bright lines: a multiplier on how
// the bed is lit (about 1 on average), fading out with depth. rel: the point relative to the camera.
float waterCaustics(float3 rel, float depthBelow) {
    const float2 xz = g_frame.cameraWrap.xz + rel.xz;
    const float t = g_frame.time.z;
    const float lines = causticNet(xz * 1.125, 288, t * 0.7, 0x9e37u) * 0.7 + causticNet(xz * 2.25 + 0.37, 576, t * 1.1, 0x51edu) * 0.3;
    float sun = 0;
    [unroll] for (uint s = 0; s < 2; ++s) sun += (1.0 - g_frame.suns[s].w) * saturate(g_frame.suns[s].y * 3.0);
    const float strength = saturate(sun) * exp(-max(depthBelow, 0.0) / 3.0);
    return 1.0 + strength * (lines * 2.2 - 0.25);
}

#endif
