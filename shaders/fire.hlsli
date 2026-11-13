// The campfire (step 5d) and other fires, and steam over water: a flame of integer noise rising through a teardrop over the bed, glowing by its
// temperature (white-yellow at the root, orange and red at the tips), and smoke above it that drifts with the wind and
// is lit by the sky, the suns and the flame. Marched by the lighting pass in front of whatever is behind it and by the
// water's reflection rays; it lights its surroundings with a flickering, shadowed light. Dims in the magic view.
#ifndef TAELIA_FIRE_HLSLI
#define TAELIA_FIRE_HLSLI

#include "common.hlsli"
#include "atmosphere.hlsli"

float fireStrength() { return g_frame.fire.w; }
float fireTime() { return g_frame.fireInfo.x; }

float fireHash(int3 c) {
    uint h = uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u;
    h = (h ^ (h >> 13)) * 0x5bd1e995u;
    return float(h & 0xffffu) / 65535.0;
}

float fireNoise(float3 p) {
    const float3 i = floor(p), f = p - i, u = f * f * (3.0 - 2.0 * f);
    const int3 c = int3(i);
    const float a = lerp(lerp(fireHash(c), fireHash(c + int3(1, 0, 0)), u.x), lerp(fireHash(c + int3(0, 1, 0)), fireHash(c + int3(1, 1, 0)), u.x), u.y);
    const float b = lerp(lerp(fireHash(c + int3(0, 0, 1)), fireHash(c + int3(1, 0, 1)), u.x), lerp(fireHash(c + int3(0, 1, 1)), fireHash(c + int3(1, 1, 1)), u.x), u.y);
    return lerp(a, b, u.z);
}

float fireFbm(float3 p) { return fireNoise(p) * 0.55 + fireNoise(p * 2.07) * 0.3 + fireNoise(p * 4.13) * 0.15; }

// Flame light (rgb, emitted per metre) and smoke density at p (relative to the bed, y up), for a fire of strength s.
void fireAt(float3 p, float s, out float3 emit, out float smoke) {
    const float t = fireTime();
    emit = 0;
    smoke = 0;
    // Up to the campfire's strength the flame shrinks as it dies down; a stronger fire (a burning tree) is the same
    // flame scaled up, with as many tongues and no brighter per metre.
    const float k = sqrt(max(s, 1.0));
    const float height = 1.35 * min(s, 1.0) * pow(k, 1.5), radius = 0.42 * sqrt(min(s, 1.0)) * pow(k, 1.2);
    const float y = p.y / height;
    if (y > 0.0 && y < 1.15) {
        // A teardrop, licked upward by noise that rises through it.
        const float3 q = p / k;
        const float w = radius * pow(saturate(1.0 - y), 0.65) * saturate(y * 6.0 + 0.25);
        const float n = fireFbm(float3(q.x * 3.1, q.y * 2.2 - t * 3.4, q.z * 3.1));
        const float d = length(p.xz + (n - 0.5) * 0.35 * k * y) / max(w, 1e-3);
        const float body = saturate(1.0 - d) * smoothstep(0.32 + 0.08 * (k - 1.0), 0.7, n + 0.55 * (1.0 - y));
        const float heat = saturate(1.15 - y * 1.1 - d * 0.35);  // hottest low and in the middle
        const float3 color = lerp(float3(1.0, 0.22, 0.03), float3(1.0, 0.8, 0.45), heat * heat);
        emit = color * body * (2.0 + 22.0 * heat * heat) * min(s, 1.0) / k;
    }
    // Smoke: a widening column above the flame, carried off by the wind as it rises.
    const float above = p.y - height * 0.8;
    if (above > 0.0 && above < 9.0) {
        const float2 drift = g_frame.fireInfo.yz * (above / 2.2);
        const float2 q = p.xz - drift;
        const float rw = 0.25 + 0.22 * above;
        const float n = fireFbm(float3(q.x * 0.9, above * 0.7 - t * 0.9, q.y * 0.9));
        smoke = saturate(1.0 - length(q) / rw) * smoothstep(0.4, 0.8, n) * 0.3 * s * saturate(1.0 - above / 9.0);
    }
}

// Through one fire's box (bed xyz relative to the camera, w its strength) along a ray from `origin` up to maxT: rgb the
// light it adds, a how much of what is behind still shows.
float4 marchOneFire(float4 fire, float3 origin, float3 dir, float maxT, float dim) {
    const float s = fire.w;
    if (s <= 0.01) return float4(0, 0, 0, 1);
    const float3 bed = fire.xyz;
    const float3 lo = bed + float3(-3.5, -0.1, -3.5) + min(float3(g_frame.fireInfo.y, 0, g_frame.fireInfo.z) * 4.0, 0.0);
    const float3 hi = bed + float3(3.5, 10.0, 3.5) + max(float3(g_frame.fireInfo.y, 0, g_frame.fireInfo.z) * 4.0, 0.0);
    const float3 inv = 1.0 / select(abs(dir) > 1e-6, dir, 1e-6);
    const float3 ta = (lo - origin) * inv, tb = (hi - origin) * inv;
    const float t0 = max(max(max(min(ta.x, tb.x), min(ta.y, tb.y)), min(ta.z, tb.z)), 0.0);
    const float t1 = min(min(min(max(ta.x, tb.x), max(ta.y, tb.y)), max(ta.z, tb.z)), maxT);
    if (t1 <= t0) return float4(0, 0, 0, 1);
    // The smoke's light: the sky from above, the suns, and the flame's glow from below.
    float3 sunLight = 0;
    [unroll] for (uint i = 0; i < 2; ++i)
        sunLight += g_frame.sunLight[i].rgb * (1.0 - g_frame.suns[i].w) * sunTransmittance(g_frame.suns[i].xyz) * saturate(g_frame.suns[i].y + 0.1);
    const float3 smokeAmbient = (skyIrradiance(float3(0, 1, 0)) + sunLight * 0.25) * 0.3;  // grey smoke by day
    const uint steps = 40;
    const float dt = (t1 - t0) / float(steps);
    float3 sum = 0;
    float T = 1.0;
    for (uint k = 0; k < steps && T > 0.01; ++k) {
        const float3 p = origin + dir * (t0 + (float(k) + 0.5) * dt) - bed;
        float3 e;
        float sm;
        fireAt(p, s, e, sm);
        // Smoke is grey, warmed by the flame only low down.
        const float3 smokeLit = smokeAmbient + float3(1.0, 0.45, 0.15) * 0.12 * s * exp(-max(p.y - 1.0, 0.0) / 1.2);
        sum += T * (e + sm * smokeLit) * dim * dt;
        T *= exp(-sm * 1.6 * dt);
    }
    return float4(sum, T);
}

// Steam where fire past the dew point meets water: a white column rising from the surface (p relative to its base, y up),
// widening and carried off by the wind; densest low down. s: its strength (about 1 for one hot spot).
float steamAt(float3 p, float s) {
    if (p.y < 0.0 || p.y > 8.0) return 0.0;
    const float t = fireTime();
    const float2 q = p.xz - g_frame.fireInfo.yz * (p.y / 1.6);
    const float rw = (0.45 + 0.17 * p.y) * sqrt(s);
    const float n = fireFbm(float3(q.x * 1.3, p.y * 0.8 - t * 1.3, q.y * 1.3));
    const float fade = pow(saturate(1.0 - p.y / 7.5), 1.5);
    return smoothstep(1.0, 0.35, length(q) / rw) * smoothstep(0.35, 0.75, n) * 0.6 * s * fade * smoothstep(0.0, 0.25, p.y);
}

// Through one steam column's box (base xyz relative to the camera, w its strength), like marchOneFire.
float4 marchSteam(float4 st, float3 origin, float3 dir, float maxT, float dim) {
    const float s = st.w;
    if (s <= 0.01) return float4(0, 0, 0, 1);
    const float3 drift = float3(g_frame.fireInfo.y, 0, g_frame.fireInfo.z) * 5.0;
    const float3 lo = st.xyz + float3(-4.5, -0.1, -4.5) + min(drift, 0.0);
    const float3 hi = st.xyz + float3(4.5, 7.6, 4.5) + max(drift, 0.0);
    const float3 inv = 1.0 / select(abs(dir) > 1e-6, dir, 1e-6);
    const float3 ta = (lo - origin) * inv, tb = (hi - origin) * inv;
    const float t0 = max(max(max(min(ta.x, tb.x), min(ta.y, tb.y)), min(ta.z, tb.z)), 0.0);
    const float t1 = min(min(min(max(ta.x, tb.x), max(ta.y, tb.y)), max(ta.z, tb.z)), maxT);
    if (t1 <= t0) return float4(0, 0, 0, 1);
    // White: the sky from above and the suns, scattered almost without loss.
    float3 sunLight = 0;
    [unroll] for (uint i = 0; i < 2; ++i)
        sunLight += g_frame.sunLight[i].rgb * (1.0 - g_frame.suns[i].w) * sunTransmittance(g_frame.suns[i].xyz) * saturate(g_frame.suns[i].y + 0.1);
    const float3 lit = (skyIrradiance(float3(0, 1, 0)) + sunLight * 0.35) * 0.8;
    const uint steps = 28;
    const float dt = (t1 - t0) / float(steps);
    float3 sum = 0;
    float T = 1.0;
    for (uint k = 0; k < steps && T > 0.01; ++k) {
        const float d = steamAt(origin + dir * (t0 + (float(k) + 0.5) * dt) - st.xyz, s);
        sum += T * d * lit * dim * dt;
        T *= exp(-d * dt);
    }
    return float4(sum, T);
}

// The campfire, then the flames where fire condensed or trees burn, then steam over water.
float4 marchFire(float3 origin, float3 dir, float maxT, float dim) {
    float4 r = marchOneFire(g_frame.fire, origin, dir, maxT, dim);
    [unroll] for (uint i = 0; i < 3; ++i) {
        if (g_frame.flames[i].w <= 0.01) continue;
        const float4 f = marchOneFire(g_frame.flames[i], origin, dir, maxT, dim);
        r = float4(r.rgb + f.rgb * r.a, r.a * f.a);
    }
    [unroll] for (uint j = 0; j < 2; ++j) {
        if (g_frame.steam[j].w <= 0.01) continue;
        const float4 f = marchSteam(g_frame.steam[j], origin, dir, maxT, dim);
        r = float4(r.rgb + f.rgb * r.a, r.a * f.a);
    }
    return r;
}

// A flame's light on a surface at rel with normal n: orange, flickering (the campfire's is shadowed by the caller).
float3 flameLight(float4 fire, float3 rel, float3 n) {
    const float s = fire.w;
    if (s <= 0.01) return 0;
    const float3 toLight = fire.xyz + float3(0, 0.6 * s, 0) - rel;
    const float d2 = dot(toLight, toLight);
    if (d2 > 900.0) return 0;
    const float d = sqrt(d2);
    const float3 L = toLight / d;
    const float t = fireTime();
    const float flicker = 0.82 + 0.18 * fireNoise(float3(t * 6.0, 0.5, 1.5));
    const float fall = saturate(1.0 - d2 / 900.0);
    return float3(1.0, 0.5, 0.2) * (0.9 * s * flicker * fall * fall / max(d2, 0.25)) * saturate(dot(n, L) + 0.05);
}

float3 fireLight(float3 rel, float3 n) { return flameLight(g_frame.fire, rel, n); }

float3 otherFlamesLight(float3 rel, float3 n) {
    float3 l = 0;
    [unroll] for (uint i = 0; i < 3; ++i) l += flameLight(g_frame.flames[i], rel, n);
    return l;
}

#endif
