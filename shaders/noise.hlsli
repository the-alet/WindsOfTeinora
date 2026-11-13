// Tileable noise for generated textures: lattices that wrap every `period` cells.
#ifndef TAELIA_NOISE_HLSLI
#define TAELIA_NOISE_HLSLI
#include "common.hlsli"

float2 gradP(int2 c, int2 period, uint seed) {
    c = ((c % period) + period) % period;
    uint h = pcgHash(uint(c.x) + pcgHash(uint(c.y) ^ seed));
    float a = float(h & 0xffffu) * (6.2831853 / 65536.0);
    return float2(cos(a), sin(a));
}

// Gradient noise on a lattice that wraps every `period` cells (p in cells), about [-1, 1].
float noiseP(float2 p, int2 period, uint seed) {
    float2 i = floor(p), f = p - i;
    int2 c = int2(i);
    float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float n00 = dot(gradP(c, period, seed), f);
    float n10 = dot(gradP(c + int2(1, 0), period, seed), f - float2(1, 0));
    float n01 = dot(gradP(c + int2(0, 1), period, seed), f - float2(0, 1));
    float n11 = dot(gradP(c + int2(1, 1), period, seed), f - float2(1, 1));
    return lerp(lerp(n00, n10, u.x), lerp(n01, n11, u.x), u.y) * 1.414;
}

float fbmP(float2 uv, int2 period, int octaves, float gain, uint seed) {
    float sum = 0, amp = 1, norm = 0;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * noiseP(uv * float2(period), period, seed + uint(o) * 7919u);
        norm += amp;
        amp *= gain;
        period *= 2;
    }
    return sum / norm;
}

// Cellular noise that wraps: x = distance to the nearest feature, y = to the second, z = random value of the cell.
float3 worleyP(float2 uv, int cells, uint seed) {
    float2 p = uv * cells;
    int2 c = int2(floor(p));
    float d1 = 9, d2 = 9, id = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            int2 q = c + int2(x, y);
            int2 w = ((q % cells) + cells) % cells;
            uint h = pcgHash(uint(w.x) + pcgHash(uint(w.y) ^ seed));
            float2 f = float2(q) + float2(h & 0xffffu, h >> 16) / 65536.0;
            float d = length(p - f);
            if (d < d1) { d2 = d1; d1 = d; id = float(pcgHash(h) & 0xffffu) / 65535.0; }
            else if (d < d2) d2 = d;
        }
    return float3(d1, d2, id);
}

#endif
