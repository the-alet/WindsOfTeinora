// The magic view (step 5c): the colours of the aspects, the free magic in the air (the field's volumes, marched up to
// whatever is nearer: the scene or the view's reach) and the glow of things in the colour of the magic they hold.
// Magic glows by its own light: the same by day and by night, and it lights nothing.
#ifndef TAELIA_MAGIC_HLSLI
#define TAELIA_MAGIC_HLSLI

#include "common.hlsli"
#include "gbuffer.hlsli"

bool magicView() { return g_frame.flags.x != 0; }

float3 aspectColor(uint aspect1) {  // aspect + 1, as packMagic stores it
    switch (aspect1) {
        case 1: return K_MAGIC_COLOR_EARTH;
        case 2: return K_MAGIC_COLOR_WATER;
        case 3: return K_MAGIC_COLOR_NATURE;
        case 4: return K_MAGIC_COLOR_FIRE;
        case 5: return K_MAGIC_COLOR_AIR;
        case 6: return K_MAGIC_COLOR_LIGHTNING;
        case 7: return K_MAGIC_COLOR_LIGHT;
        default: return 0;
    }
}

// The field at a point relative to the camera: rgb the aspects' colours weighted by density, a the total (kg/m3).
// The fine window where it reaches, else the coarse one, else the free magic everywhere (half air, half neutral).
float4 magicAt(float3 rel) {
    const float dims = float(K_MAGIC_COLUMNS), layers = float(K_MAGIC_VIEW_LAYERS);
    if (g_frame.magicInfo.w > 0.5) {
        [unroll] for (uint l = 0; l < 2; ++l) {
            const float4 w = g_frame.magicWindow[l];
            const float3 cell = float3(w.w, l == 0 ? g_frame.magicInfo.x : g_frame.magicInfo.y, w.w);
            const float3 t = (rel - w.xyz) / cell;  // x, height, z in cells
            const float3 uvw = float3(t.x / dims, t.z / dims, t.y / layers);
            if (all(uvw > 0.0) && all(uvw < 1.0))
                return g_tex3D[l == 0 ? g_frame.magicVolume.x : g_frame.magicVolume.y].SampleLevel(s_linearClamp, uvw, 0);
        }
    }
    const float free = K_MAGIC_AMBIENT_FREE;
    return float4((K_MAGIC_COLOR_AIR + K_MAGIC_COLOR_NEUTRAL) * 0.5 * free, free);
}

// The glow of the free magic along a view ray up to `dist` (no farther than the view reaches). Steps grow with
// distance: the fine detail is near.
float3 magicFreeGlow(float3 dir, float dist, float2 pixel) {
    const float reach = min(dist, g_frame.magicInfo.z);
    const uint steps = 40;
    const float jitter = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    float3 sum = 0;
    for (uint i = 0; i < steps; ++i) {
        const float s0 = (float(i) + jitter) / float(steps), s1 = (float(i) + 1.0 + jitter) / float(steps);
        const float t0 = reach * s0 * s0, t1 = reach * min(s1 * s1, 1.0);
        const float4 m = magicAt(dir * (0.5 * (t0 + t1)));
        sum += m.rgb * (1.0 + 0.25 * m.a) * max(t1 - t0, 0.0);  // dense magic glows more than its share
    }
    return sum * K_MAGIC_FREE_GLOW;
}

// A thing's glow: the colour of the magic it holds, as full as its share, brighter toward its outline so its shape
// reads; fading out where the view stops reaching.
float3 magicSurfaceGlow(float materialA, float3 n, float3 V, float dist) {
    uint aspect1;
    float fill;
    unpackMagic(materialA, aspect1, fill);
    if (aspect1 == 0) return 0;
    const float rim = 1.0 - saturate(dot(n, V));
    const float fade = 1.0 - smoothstep(g_frame.magicInfo.z * 0.8, g_frame.magicInfo.z, dist);
    return aspectColor(aspect1) * K_MAGIC_SURFACE_GLOW * fill * (0.6 + 0.6 * rim * rim) * fade;
}

// Magic past the dew point shows in plain sight: a faint tint of its colours where it is that thick (only through
// the box where it is, so it costs nothing elsewhere).
float3 magicDewTint(float3 dir, float maxT) {
    if (g_frame.denseLo.w < 0.5) return 0;
    const float3 lo = g_frame.denseLo.xyz, hi = g_frame.denseHi.xyz;
    const float3 inv = 1.0 / select(abs(dir) > 1e-6, dir, 1e-6);
    const float3 ta = lo * inv, tb = hi * inv;
    const float t0 = max(max(max(min(ta.x, tb.x), min(ta.y, tb.y)), min(ta.z, tb.z)), 0.0);
    const float t1 = min(min(min(max(ta.x, tb.x), max(ta.y, tb.y)), max(ta.z, tb.z)), maxT);
    if (t1 <= t0) return 0;
    float3 sum = 0;
    const float dt = (t1 - t0) / 16.0;
    for (uint i = 0; i < 16; ++i) {
        const float4 m = magicAt(dir * (t0 + (float(i) + 0.5) * dt));
        sum += m.rgb / max(m.a, 1e-3) * max(m.a - K_MAGIC_DEW_POINT, 0.0) * dt;
    }
    return sum * 0.004;
}

// The rest of the world in the magic view: grey and dim.
float3 magicDim(float3 color) { return dot(color, float3(0.2126, 0.7152, 0.0722)) * K_MAGIC_VIEW_DIM; }

#endif
