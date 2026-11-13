// Volumetric clouds (step 3): the noise volumes, the cloud shadow map, the half-resolution raymarch, temporal
// accumulation and the composite into the lit scene. C++ side: engine/render/cloud_renderer.cpp.
// Density = the simulation's map (coverage, base, thickness) x 3D noise (shape, detail), carried by the wind.
#include "atmosphere.hlsli"
#include "clouds.hlsli"
#include "fire.hlsli"

// tex/size/offset per kernel:
//   ShapeNoise/DetailNoise  tex0.x out uav, size.x edge
//   Shadow     tex0: out uav, shape, detail; size.xy: map size; offset: noise offset
//   March      tex0: colour uav, depth uav, shape, detail; tex1.x scene depth; size: half w h, full w h; offset
//   Resolve    tex0: out uav, march colour, march depth, history; tex1.x history valid; size: half w h, full w h
//   Composite  tex0: scene uav, scene depth, clouds, march depth; size: full w h, half w h
// offset: xz the camera in cloud coordinates modulo the shape tile (m), w: shape tile size (m).
struct CloudPush {
    uint4 tex0, tex1, size;
    float4 offset;
};
[[vk::push_constant]] ConstantBuffer<CloudPush> g_push : register(b1, space0);

static const float kSigma = 0.03;  // extinction per metre at density 1
static const float kPi = 3.14159265;

float remap(float x, float a, float b, float c, float d) { return c + (x - a) / (b - a) * (d - c); }

// ---- tileable noise for the volumes (p in tile units 0..1)
float3 hash33(int3 c, uint seed) {
    const uint h = pcgHash(uint(c.x) + pcgHash(uint(c.y) + pcgHash(uint(c.z) + seed)));
    const uint h2 = pcgHash(h ^ 0x9e3779b9u), h3 = pcgHash(h2 ^ 0x85ebca6bu);
    return float3(h & 0xffffffu, h2 & 0xffffffu, h3 & 0xffffffu) / 16777216.0;
}

int3 wrapCell(int3 c, int n) { return ((c % n) + n) % n; }

// Inverted cellular noise: 1 at the feature points, 0 a cell away.
float worley(float3 p, int cells, uint seed) {
    const float3 q = p * cells;
    const int3 ci = int3(floor(q));
    const float3 f = q - float3(ci);
    float best = 1e9;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                const int3 o = int3(x, y, z);
                const float3 d = float3(o) + hash33(wrapCell(ci + o, cells), seed) - f;
                best = min(best, dot(d, d));
            }
    return 1.0 - saturate(sqrt(best));
}

float perlinCorner(int3 c, int period, float3 f, uint seed) {
    const float3 g = normalize(hash33(wrapCell(c, period), seed) * 2.0 - 1.0 + 1e-4);
    return dot(g, f);
}

float perlin(float3 p, int period, uint seed) {
    const float3 q = p * period;
    const int3 i = int3(floor(q));
    const float3 f = q - float3(i);
    const float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float c[8];
    [unroll] for (int k = 0; k < 8; ++k) {
        const int3 o = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        c[k] = perlinCorner(i + o, period, f - float3(o), seed);
    }
    const float x0 = lerp(lerp(c[0], c[1], u.x), lerp(c[2], c[3], u.x), u.y);
    const float x1 = lerp(lerp(c[4], c[5], u.x), lerp(c[6], c[7], u.x), u.y);
    return lerp(x0, x1, u.z);
}

[numthreads(8, 8, 1)] void ShapeNoiseMain(uint3 id : SV_DispatchThreadID) {
    const uint n = g_push.size.x;
    if (any(id >= n)) return;
    const float3 p = (float3(id) + 0.5) / float(n);
    const float pn = (perlin(p, 4, 11u) + 0.5 * perlin(p, 8, 12u) + 0.25 * perlin(p, 16, 13u)) / 1.75;
    const float perl = saturate(pn * 0.9 + 0.5);
    const float w4 = worley(p, 4, 21u), w8 = worley(p, 8, 22u), w16 = worley(p, 16, 23u), w32 = worley(p, 32, 24u),
                w64 = worley(p, 64, 25u);
    const float3 k = float3(0.625, 0.25, 0.125);
    const float f0 = dot(float3(w4, w8, w16), k), f1 = dot(float3(w8, w16, w32), k), f2 = dot(float3(w16, w32, w64), k);
    const float pw = saturate(remap(perl, f0 - 1.0, 1.0, 0.0, 1.0));
    g_rwTex3D[g_push.tex0.x][id] = float4(pw, f0, f1, f2);
}

[numthreads(8, 8, 1)] void DetailNoiseMain(uint3 id : SV_DispatchThreadID) {
    const uint n = g_push.size.x;
    if (any(id >= n)) return;
    const float3 p = (float3(id) + 0.5) / float(n);
    const float w2 = worley(p, 2, 31u), w4 = worley(p, 4, 32u), w8 = worley(p, 8, 33u), w16 = worley(p, 16, 34u);
    const float3 k = float3(0.625, 0.25, 0.125);
    g_rwTex3D[g_push.tex0.x][id] = float4(dot(float3(w2, w4, w8), k), dot(float3(w4, w8, w16), k), w16, 1);
}

// ---- density
// rel: camera-relative point, m: the map there (coverage, base, thickness). Detail erodes the edges (primary rays only).
float cloudDensity(float3 rel, float3 m, bool detail, uint shapeSrv, uint detailSrv) {
    const float alt = rel.y + cloudCameraAltitude();
    const float h = (alt - m.y) / max(m.z, 1.0);
    if (h <= 0.0 || h >= 1.0 || m.x < 0.005) return 0.0;
    const float grad = smoothstep(0.0, 0.12, h) * smoothstep(1.0, 0.55 - 0.25 * m.x, h);
    const float tile = g_push.offset.w;
    const float3 p = float3(rel.x + g_push.offset.x, alt, rel.z + g_push.offset.z);
    const float4 s = g_tex3D[shapeSrv].SampleLevel(s_linearWrap, p / tile, 0);
    const float low = dot(s.gba, float3(0.625, 0.25, 0.125));
    const float n = saturate(remap(s.r * 0.65 + low * 0.35, 0.3, 0.85, 0.0, 1.0));
    const float base = n * grad;
    // Fair days break even the map's densest cells into heaps; overcast days close them.
    const float cov = saturate(m.x * lerp(0.78, 1.05, g_frame.clouds.z));
    float d = saturate(remap(base, 1.0 - cov, 1.0, 0.0, 1.0)) * cov;
    if (detail && d > 0.0) {
        const float3 dn = g_tex3D[detailSrv].SampleLevel(s_linearWrap, p * (6.0 / tile), 0).rgb;
        const float e = dot(dn, float3(0.625, 0.25, 0.125));
        const float mod = lerp(e, 1.0 - e, saturate(h * 4.0));
        d = saturate(remap(d, mod * 0.42, 1.0, 0.0, 1.0));
    }
    return d;
}

float densityAt(float3 rel, bool detail) {
    return cloudDensity(rel, cloudMapAt(rel.xz), detail, g_push.tex0.z, g_push.tex0.w);
}

// ---- the cloud shadow map: light let through from sea level toward each sun (and straight up)
[numthreads(8, 8, 1)] void ShadowMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_push.size.xy)) return;
    const float2 xz = cloudShadowCorner(float(g_push.size.x)) + (float2(id.xy) + 0.5) * kCloudShadowTexel;
    const float camAlt = cloudCameraAltitude();
    const uint shapeSrv = g_push.tex0.y, detailSrv = g_push.tex0.z;
    float3 lit = 1;
    [unroll] for (uint ch = 0; ch < 3; ++ch) {
        const float3 L = ch < 2 ? g_frame.suns[ch].xyz : float3(0, 1, 0);
        if (L.y <= -0.05) continue;
        const float2 slope = L.xz / max(L.y, 0.05);
        const float pathPerMetre = length(float3(slope.x, 1, slope.y));
        const float da = (kCloudTop - kCloudBottom) / 16.0;
        float od = 0;
        for (uint k = 0; k < 16; ++k) {
            const float a = kCloudBottom + (k + 0.5) * da;
            const float3 rel = float3(xz.x + slope.x * a, a - camAlt, xz.y + slope.y * a);
            od += cloudDensity(rel, cloudMapAt(rel.xz), false, shapeSrv, detailSrv);
        }
        lit[ch] = exp(-od * da * pathPerMetre * kSigma);
    }
    g_rwTex2D[g_push.tex0.x][id.xy] = float4(lit, 1);
}

// ---- lighting inside the cloud
float hg(float c, float g) {
    const float g2 = g * g;
    return (1.0 - g2) / (4.0 * kPi * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

float cloudPhase(float c, float k) { return 0.7 * hg(c, 0.6 * k) + 0.3 * hg(c, -0.25 * k); }

// Optical depth toward the light: six steps doubling in length (about 2.5 km).
float lightDepth(float3 pos, float3 L) {
    float od = 0, t = 0, step = 40.0;
    [unroll] for (int k = 0; k < 6; ++k) {
        od += densityAt(pos + L * (t + step * 0.5), k < 3) * step;  // the same eroded surface near the sample
        t += step;
        step *= 2.0;
    }
    return od * kSigma;
}

// Beer-Lambert with an approximation of multiple scattering: octaves that see less extinction, a broader phase.
float scatterLight(float od, float c) {
    float sum = 0, a = 1, b = 1;
    [unroll] for (int n = 0; n < 4; ++n) {
        sum += a * exp(-od * b) * cloudPhase(c, a);
        a *= 0.7;
        b *= 0.35;
    }
    return sum;
}

// Where the ray is inside the cloud layer (flat layer: the world's curve is a few metres over the march).
bool layerSegment(float3 dir, out float t0, out float t1) {
    const float ca = cloudCameraAltitude();
    t0 = 0;
    t1 = 0;
    if (ca < kCloudBottom) {
        if (dir.y <= 1e-4) return false;
        t0 = (kCloudBottom - ca) / dir.y;
        t1 = (kCloudTop - ca) / dir.y;
    } else if (ca > kCloudTop) {
        if (dir.y >= -1e-4) return false;
        t0 = (kCloudTop - ca) / dir.y;
        t1 = (kCloudBottom - ca) / dir.y;
    } else {
        t1 = dir.y > 1e-4 ? (kCloudTop - ca) / dir.y : dir.y < -1e-4 ? (kCloudBottom - ca) / dir.y : 1e9;
    }
    return true;
}

[numthreads(8, 8, 1)] void MarchMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_push.size.xy)) return;
    const uint2 full = g_push.size.zw;
    const float2 uv = (float2(id.xy) * 2.0 + 1.0) / float2(full);
    const float3 dir = viewRay(uv);
    // March to the farthest of the 2x2 scene pixels; the composite drops clouds behind nearer ones.
    float depth = 1;
    [unroll] for (uint k = 0; k < 4; ++k) {
        const uint2 q = min(id.xy * 2 + uint2(k & 1, k >> 1), full - 1);
        depth = min(depth, g_tex2D[g_push.tex1.x].Load(int3(q, 0)).r);
    }
    const float sceneDist = depth > 0 ? g_frame.cameraUp.w / depth / max(dot(dir, g_frame.cameraForward.xyz), 1e-3) : 1e9;
    float t0, t1;
    const bool hit = layerSegment(dir, t0, t1);
    t1 = min(min(t1, 80000.0), sceneDist);
    float3 S = 0;
    float T = 1, entry = -1, tw = 0;
    if (hit && t1 > t0) {
        const float jitter = frac(ign(float2(id.xy)) + float(g_frame.flags.y % 64u) * 0.618034);
        float3 sunL[2], sunD[2];
        float sunC[2];
        bool sunOn[2];
        [unroll] for (uint i = 0; i < 2; ++i) {
            sunD[i] = g_frame.suns[i].xyz;
            sunL[i] = g_frame.sunLight[i].rgb * (1.0 - g_frame.suns[i].w) * sunTransmittance(sunD[i]);
            sunC[i] = dot(dir, sunD[i]);
            sunOn[i] = max(sunL[i].r, max(sunL[i].g, sunL[i].b)) > 1e-4;
        }
        const float3 eUp = skyIrradiance(float3(0, 1, 0)) / kPi, eDown = skyIrradiance(float3(0, -1, 0)) / kPi;
        // Coarse steps through empty air; on touching cloud, step back and walk it in fine steps.
        float t = t0 + jitter * max(30.0, t0 * 0.004);
        bool fine = false;
        uint empty = 0;
        for (uint s = 0; s < 320 && t < t1; ++s) {
            const float3 pos = dir * t;
            const float3 m = cloudMapAt(pos.xz);
            if (!fine) {
                const float coarse = max(120.0, t * 0.02);
                if (cloudDensity(pos, m, false, g_push.tex0.z, g_push.tex0.w) > 0.0) {
                    fine = true;
                    empty = 0;
                    t = max(t0, t - coarse);
                } else {
                    t += coarse;
                }
                continue;
            }
            const float dt = max(30.0, t * 0.004);
            const float d = cloudDensity(pos, m, true, g_push.tex0.z, g_push.tex0.w);
            if (d <= 0.002) {
                if (++empty > 8) fine = false;
                t += dt;
                continue;
            }
            empty = 0;
            if (entry < 0) entry = t;
            const float sig = d * kSigma;
            const float h = saturate((pos.y + cloudCameraAltitude() - m.y) / max(m.z, 1.0));
            float3 light = eUp * lerp(0.5, 1.0, h) + eDown * 0.4 * (1.0 - h);
            [unroll] for (uint i = 0; i < 2; ++i) {
                if (!sunOn[i]) continue;
                const float od = lightDepth(pos, sunD[i]);
                const float powder = lerp(1.0 - 0.35 * exp(-sig * 150.0), 1.0, saturate(sunC[i] * 0.5 + 0.5));
                light += sunL[i] * scatterLight(od, sunC[i]) * powder;
            }
            // A storm's clouds are heavy and dark; a lightning flash lights them from inside.
            light = light * (1.0 - 0.6 * g_frame.weather.y) + float3(0.75, 0.78, 1.0) * 4.0 * g_frame.weather.z;
            const float ts = exp(-sig * dt);
            S += T * light * (1.0 - ts);
            tw += t * T * (1.0 - ts);
            T *= ts;
            t += dt;
            if (T < 0.01) {
                T = 0;
                break;
            }
        }
        // Haze between the camera and the clouds.
        if (T < 0.999) {
            const float a = 1.0 - T;
            S = aerialPerspective(S / a, dir * (tw / a)) * a;
        }
    }
    g_rwTex2D[g_push.tex0.x][id.xy] = float4(S, T);
    g_rwTex2D[g_push.tex0.y][id.xy] = float4(entry > 0 ? entry : (hit ? max(t0, 1.0) : 1e5), 0, 0, 0);
}

// ---- temporal accumulation at half resolution
[numthreads(8, 8, 1)] void ResolveMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_push.size.xy)) return;
    const Texture2D<float4> march = g_tex2D[g_push.tex0.y];
    float4 cur = march.Load(int3(id.xy, 0));
    float4 mn = cur, mx = cur, mean = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            const int2 q = clamp(int2(id.xy) + int2(x, y), 0, int2(g_push.size.xy) - 1);
            const float4 v = march.Load(int3(q, 0));
            mn = min(mn, v);
            mx = max(mx, v);
            mean += v / 9.0;
        }
    float4 outC = mean;
    if (g_push.tex1.x != 0 && g_frame.cameraDelta.w > 0) {
        const float dist = g_tex2D[g_push.tex0.z].Load(int3(id.xy, 0)).r;
        const float2 uv = (float2(id.xy) * 2.0 + 1.0) / float2(g_push.size.zw);
        const float3 rel = viewRay(uv) * min(dist, 1e5);
        const float4 clip = mul(g_frame.prevViewProj, float4(rel + g_frame.cameraDelta.xyz, 1));
        if (clip.w > 1e-4) {
            const float2 prevUv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
            if (all(prevUv > 0.0) && all(prevUv < 1.0)) {
                const float4 hist = clamp(g_tex2D[g_push.tex0.w].SampleLevel(s_linearClamp, prevUv, 0), mn, mx);
                outC = lerp(hist, cur, 0.12);
            }
        }
    }
    g_rwTex2D[g_push.tex0.x][id.xy] = outC;
}

// ---- composite into the lit scene: over the sky and anything farther than the clouds
[numthreads(8, 8, 1)] void CompositeMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_push.size.xy)) return;
    const float depth = g_tex2D[g_push.tex0.y].Load(int3(id.xy, 0)).r;
    const float2 uvHalf = (float2(id.xy) + 0.5) * 0.5 / float2(g_push.size.zw);
    if (depth > 0) {
        const float3 dir = viewRay((float2(id.xy) + 0.5) / float2(g_push.size.xy));
        const float sceneDist = g_frame.cameraUp.w / depth / max(dot(dir, g_frame.cameraForward.xyz), 1e-3);
        const float cloudDist = g_tex2D[g_push.tex0.w].SampleLevel(s_pointClamp, uvHalf, 0).r;
        if (sceneDist < cloudDist) return;
    }
    float4 c = g_tex2D[g_push.tex0.z].SampleLevel(s_linearClamp, uvHalf, 0);
    // The campfire in front of the sky goes over the clouds (surfaces got it in the lighting pass).
    float4 fire = float4(0, 0, 0, 1);
    if (depth <= 0) fire = marchFire(0, viewRay((float2(id.xy) + 0.5) / float2(g_push.size.xy)), 1e5, g_frame.flags.x != 0 ? K_MAGIC_VIEW_DIM : 1.0);
    if (c.a > 0.9999) {
        if (fire.a < 1.0 || fire.r > 0.0) {
            const float4 s0 = g_rwTex2D[g_push.tex0.x][id.xy];
            g_rwTex2D[g_push.tex0.x][id.xy] = float4(s0.rgb * fire.a + fire.rgb, s0.a);
        }
        return;
    }
    if (g_frame.flags.x != 0) c.rgb = dot(c.rgb, float3(0.2126, 0.7152, 0.0722)) * K_MAGIC_VIEW_DIM;  // magic view: grey, dim
    const float T = c.a * smoothstep(0.0, 0.12, c.a);  // thick cloud hides the discs and stars completely
    const float4 scene = g_rwTex2D[g_push.tex0.x][id.xy];
    if (g_frame.flags.x != 0) {  // the free magic glows in front of far clouds: they only add a dim grey
        g_rwTex2D[g_push.tex0.x][id.xy] = float4((scene.rgb + c.rgb) * fire.a + fire.rgb, scene.a);
        return;
    }
    // Through cloud, the discs and their glow fade to no more than the cloud's own brightness.
    float3 through = scene.rgb * T;
    const float own = max(max(max(c.r, c.g), c.b) / max(1.0 - c.a, 1e-3), 1e-3);
    const float cap = exp2(lerp(log2(1e4), log2(own), smoothstep(0.0, 0.2, 1.0 - c.a)));
    const float peak = max(max(through.r, through.g), through.b);
    if (peak > cap) through *= cap / peak;
    g_rwTex2D[g_push.tex0.x][id.xy] = float4((through + c.rgb) * fire.a + fire.rgb, scene.a);
}
