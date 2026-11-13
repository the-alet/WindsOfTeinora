// The magic view: the colours of the aspects; the free magic in the air (the field's volumes, marched up to whatever is
// nearer: the scene or the view's reach) as a faint haze with wisps carried by its flow, the air's brightest (its
// currents are the wind), and motes glinting near the eye; the magic each thing holds streaming through it (pulses
// along where its mesh says its streams run: up a tree from the root out to the leaves, up grass blades, in slow veins
// through the ground), the lighter the aspect the faster, with a glowing outline. Magic glows by its own light: the
// same by day and by night, and it lights nothing.
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

// Where a point relative to the camera falls in the uploaded windows: the level (0 fine, 1 coarse) and its volume
// coordinate; false past both.
bool magicCoords(float3 rel, out uint level, out float3 uvw) {
    level = 0;
    uvw = 0;
    if (g_frame.magicInfo.w < 0.5) return false;
    const float dims = float(K_MAGIC_COLUMNS), layers = float(K_MAGIC_VIEW_LAYERS);
    [unroll] for (uint l = 0; l < 2; ++l) {
        const float4 w = g_frame.magicWindow[l];
        const float3 cell = float3(w.w, l == 0 ? g_frame.magicInfo.x : g_frame.magicInfo.y, w.w);
        const float3 t = (rel - w.xyz) / cell;  // x, height, z in cells
        const float3 c = float3(t.x / dims, t.z / dims, t.y / layers);
        if (all(c > 0.0) && all(c < 1.0)) {
            level = l;
            uvw = c;
            return true;
        }
    }
    return false;
}

// The field at a point relative to the camera: rgb the aspects' colours weighted by density, a the total (kg/m3).
// The fine window where it reaches, else the coarse one, else the free magic everywhere (half air, half neutral).
float4 magicAt(float3 rel) {
    uint l;
    float3 uvw;
    if (magicCoords(rel, l, uvw)) return g_tex3D[l == 0 ? g_frame.magicVolume.x : g_frame.magicVolume.y].SampleLevel(s_linearClamp, uvw, 0);
    const float free = K_MAGIC_AMBIENT_FREE;
    return float4((K_MAGIC_COLOR_AIR + K_MAGIC_COLOR_NEUTRAL) * 0.5 * free, free);
}

// How the field flows there (m/s) and how much of it is air magic (kg/m3); past the windows, the wind and the free air.
float4 magicFlowAt(float3 rel) {
    uint l;
    float3 uvw;
    if (magicCoords(rel, l, uvw)) return g_tex3D[l == 0 ? g_frame.magicVolume.z : g_frame.magicVolume.w].SampleLevel(s_linearClamp, uvw, 0);
    return float4(g_frame.wind.xyz, K_MAGIC_AMBIENT_FREE * 0.5);
}

// A point's world position for patterns (x and z wrap every 4096 m, far past what the view reaches).
float3 magicWorld(float3 rel) {
    return float3(g_frame.cameraWrap.y + rel.x, g_frame.cameraPosHigh.y + g_frame.cameraPosLow.y + rel.y, g_frame.cameraWrap.w + rel.z);
}

// Wisps of free magic carried by its flow (0..1): a pattern drawn out along the flow and moved with it, restarted every
// few seconds in two layers half a period apart (each fades out before it restarts), so it never smears.
float magicWisps(float3 rel, float3 u) {
    const float3 q = magicWorld(rel) * 0.55;
    const float speed = length(u);
    const float3 along = speed > 0.05 ? u / speed : float3(0, 1, 0);
    const float3 s = q - along * (dot(q, along) * 0.65);  // drawn out about three times along the flow
    const float3 drift = u * (0.55 * 0.35);                 // the flow in the pattern's own measure
    const float period = 4.0, t = g_frame.fireInfo.x / period;
    const float a = frac(t), b = frac(t + 0.5);
    const float3 sa = s - drift * (a * period), sb = s - drift * (b * period) + 31.7;
    const float na = latticeNoise3(sa) * 0.7 + latticeNoise3(sa * 2.3 + 11.0) * 0.3;
    const float nb = latticeNoise3(sb) * 0.7 + latticeNoise3(sb * 2.3 + 11.0) * 0.3;
    return smoothstep(0.66, 0.86, lerp(na, nb, abs(1.0 - 2.0 * a)));
}

// The free magic along a view ray up to `dist` (no farther than the view reaches): a faint haze of all of it (denser
// magic shows more) and wisps where its flow carries it, the air's own the brightest. Steps grow with distance: the
// fine detail is near, and the wisps fade out farther off where the steps get long.
float3 magicFreeGlow(float3 dir, float dist, float2 pixel) {
    const float reach = min(dist, g_frame.magicInfo.z);
    const uint steps = 40;
    const float jitter = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    float3 sum = 0;
    for (uint i = 0; i < steps; ++i) {
        const float s0 = (float(i) + jitter) / float(steps), s1 = (float(i) + 1.0 + jitter) / float(steps);
        const float t0 = reach * s0 * s0, t1 = reach * min(s1 * s1, 1.0);
        const float tm = 0.5 * (t0 + t1);
        const float3 p = dir * tm;
        const float4 m = magicAt(p);
        const float4 flow = magicFlowAt(p);
        const float wisp = magicWisps(p, flow.xyz) * saturate(1.4 - tm / 40.0);
        const float3 haze = m.rgb * (1.0 + 0.25 * m.a) * K_MAGIC_FREE_GLOW * 0.4;
        const float3 wisps = (m.rgb + K_MAGIC_COLOR_AIR * flow.w * K_MAGIC_AIR_WISPS) * wisp * K_MAGIC_WISP_GLOW;
        sum += (haze + wisps) * max(t1 - t0, 0.0);
    }
    return sum;
}

// Motes: the gas's own grains glinting near the eye, on a few shells round it; each wanders on its own small loop.
// Coloured by the magic where it is, brighter where there is more of it.
float3 magicMotes(float3 dir, float dist) {
    const float shells[4] = {1.6, 2.7, 4.3, 6.8};
    const float cellSize = 0.16;  // m on the shell
    const float t = g_frame.fireInfo.x;
    const float az = atan2(dir.z, dir.x), el = asin(clamp(dir.y, -1.0, 1.0));
    float3 sum = 0;
    [unroll] for (uint k = 0; k < 4; ++k) {
        const float d = shells[k];
        if (d > dist) break;
        const float2 q = float2(az, el) * d / cellSize;
        const int2 c = int2(floor(q));
        const uint h = pcgHash(uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ (k + 1u) * 83492791u);
        if ((h & 255u) > 46u) continue;  // most cells hold no mote at this moment
        const float ph = float((h >> 8) & 1023u) / 1023.0 * 6.2831853;
        const float2 at = 0.5 + 0.3 * float2(cos(t * 0.9 + ph), sin(t * 1.3 + ph * 1.7));
        const float r = length(q - float2(c) - at) * cellSize;  // m from the mote along the shell
        const float size = d * 0.0016;                          // a pixel or two
        const float glint = exp(-r * r / (size * size)) * (0.65 + 0.35 * sin(t * 6.0 + ph * 3.0));
        const float4 mag = magicAt(dir * d);
        sum += mag.rgb / max(mag.a, 1e-3) * min(mag.a, 6.0) * glint / (1.0 + d * 0.25);
    }
    return sum * K_MAGIC_MOTES;
}

// How fast magic streams through a thing of this aspect (aspect + 1): the lighter its motes, the faster.
float streamSpeed(uint aspect1) {
    switch (aspect1) {
        case 1: return K_MAGIC_STREAM_EARTH;
        case 2: return K_MAGIC_STREAM_WATER;
        case 3: return K_MAGIC_STREAM_NATURE;
        case 5: return K_MAGIC_STREAM_AIR;
        case 7: return K_MAGIC_STREAM_LIGHT;
        default: return K_MAGIC_STREAM_FIRE;
    }
}

// A thing's own magic: the colour of what it holds, as full as its share, and streams of it running through the thing:
// soft pulses travelling along `flow` (metres along the thing's streams, e.g. from a tree's root out to its leaves),
// hot-cored, gathered into threads where `threads` is high (0..1). The lighting pass adds the outline.
float3 heldMagicGlow(uint aspect1, float fill, float flow, float threads) {
    if (aspect1 == 0 || fill <= 0.0) return 0;
    const float3 col = aspectColor(aspect1);
    const float s = (flow - g_frame.fireInfo.x * streamSpeed(aspect1)) / K_MAGIC_STREAM_SPACING;
    const float cell = floor(s);
    const uint h = pcgHash(uint(int(cell)) * 2654435761u + 0x9e37u);
    const float at = 0.25 + 0.5 * float(h & 255u) / 255.0;            // where in its stretch the pulse sits
    const float strength = 0.4 + 1.2 * float((h >> 8) & 255u) / 255.0;  // pulses differ
    const float x = frac(s) - at;
    const float pulse = exp(-x * x * 60.0) * strength;
    return (col * 0.12 + (col * 2.0 + 0.3) * pulse * threads) * fill * K_MAGIC_SURFACE_GLOW;
}

// The outline of a thing in the colour of what it holds (from its G-buffer material), so its shape reads.
float3 magicOutlineGlow(float materialA, float3 n, float3 V) {
    uint aspect1;
    float fill;
    unpackMagic(materialA, aspect1, fill);
    if (aspect1 == 0) return 0;
    const float rim = 1.0 - saturate(dot(n, V));
    return aspectColor(aspect1) * K_MAGIC_SURFACE_GLOW * fill * rim * rim * rim * 0.9;
}

// In the magic view the albedo target carries a surface's own glow (rgb, at 1/MAGIC_GLOW_SCALE) and the plain albedo's
// brightness (a): the dim grey world keeps its shapes and shading, and the lighting pass adds the glow.
float4 magicViewAlbedo(float3 albedo, float3 glow) {
    return float4(saturate(glow / K_MAGIC_GLOW_SCALE), dot(albedo, float3(0.2126, 0.7152, 0.0722)));
}

// The ground's earth magic runs in slow veins: thin lines where two smooth patterns meet, pulses creeping along them.
// x: where along the veins (m), y: how much of a vein is here (0..1).
float2 groundVeins(float3 rel) {
    const float3 w = magicWorld(rel);
    const float a = latticeNoise3(float3(w.x, 0.0, w.z) * 0.16), b = latticeNoise3(float3(w.x, 7.3, w.z) * 0.16 + 19.1);
    return float2((a + b) * 14.0, pow(saturate(1.0 - abs(a - b) * 18.0), 2.0));
}

// Water's own magic: light-to-deep blue, flowing in ribbons with the drift of its surface; with its outline, fading out
// where the view stops reaching.
float3 waterMagicGlow(float3 rel, float3 n, float3 V, float dist) {
    const float3 w = magicWorld(rel);
    const float2 down = normalize(g_frame.wind.xz + float2(1e-3, 0));
    const float bend = latticeNoise3(float3(w.x, 3.1, w.z) * 0.25) * 3.0;
    const float flow = dot(w.xz, down) + bend;
    const float ribbons = pow(0.5 + 0.5 * cos(dot(w.xz, float2(-down.y, down.x)) * 2.2 + bend * 2.0), 4.0);
    const float fade = 1.0 - smoothstep(g_frame.magicInfo.z * 0.8, g_frame.magicInfo.z, dist);
    return (heldMagicGlow(MAGIC_HELD_WATER, 1.0, flow, ribbons) + magicOutlineGlow(packMagic(MAGIC_HELD_WATER, 1.0, false), n, V)) * fade;
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
