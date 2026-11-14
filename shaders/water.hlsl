// Ponds: liquid at each pond's own level over its irregular shore shape (out to the footprint, where the bank keeps
// the land above the water). Calm: no waves, only faint wind ripples. Light through the water is absorbed by depth.
#include "common.hlsli"
#include "sky.hlsli"
#include "rt.hlsli"
#include "clouds.hlsli"
#include "ball.hlsli"
#include "magic.hlsli"
#include "fire.hlsli"
#include "water_optics.hlsli"

struct Pond {
    float4 centerLevel;  // xz: centre relative to the camera, y: level relative to the camera, w: mean radius
    float4 shape;        // three phases (0..1) of the shore harmonics, w: footprint (normalized distance)
};

struct WaterPush {
    uint pondsSrv;
    uint pondsOffset;
    uint sceneColor;  // lit scene before the water (refraction)
    uint depth;
    float2 invSize;   // 1 / render size
    float2 pad;
};
[[vk::push_constant]] ConstantBuffer<WaterPush> g_push : register(b1, space0);

static const uint SEGMENTS = 96;
static const uint RINGS = 10;

// Must match Pond::shoreRadius in sim/src/terrain.cpp.
float shoreRadius(Pond p, float angle) {
    const float u1 = p.shape.x, u2 = p.shape.y, u3 = p.shape.z;
    return p.centerLevel.w * (1.0 + 0.22 * sin(angle + u1 * 6.2832) + 0.14 * sin(2.0 * angle + u2 * 6.2832) +
                              0.08 * sin(3.0 * angle + u3 * 6.2832) + 0.04 * sin(5.0 * angle + u1 * 9.1));
}

struct VSOut {
    float4 pos : SV_Position;
    float3 rel : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    Pond p = g_buffers[g_push.pondsSrv].Load<Pond>(g_push.pondsOffset + iid * 32);
    // Triangle list over a polar grid: each quad = 6 vertices.
    const uint quad = vid / 6, corner = vid % 6;
    const uint seg = quad % SEGMENTS, ring = quad / SEGMENTS;
    const uint2 offs[6] = {uint2(0, 0), uint2(1, 0), uint2(1, 1), uint2(0, 0), uint2(1, 1), uint2(0, 1)};
    const uint s = seg + offs[corner].x, r = ring + offs[corner].y;
    const float angle = float(s % SEGMENTS) * (6.2831853 / SEGMENTS);
    const float rn = float(r) / RINGS * p.shape.w;
    const float radius = shoreRadius(p, angle) * rn;
    VSOut o;
    o.rel = float3(p.centerLevel.x + cos(angle) * radius, p.centerLevel.y, p.centerLevel.z + sin(angle) * radius);
    o.pos = mul(g_frame.viewProj, float4(o.rel, 1));
    return o;
}

float rippleNoise(float2 p, int period) {
    // Value noise on a lattice that wraps every `period` cells (the cells divide 256 m, so the camera wrap stays seamless).
    float2 i = floor(p), f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    int2 c = ((int2(i) % period) + period) % period;
    int2 d = (c + 1) % period;
    float a = float(pcgHash(uint(c.x) + pcgHash(uint(c.y))) & 0xffffu) / 65535.0;
    float b = float(pcgHash(uint(d.x) + pcgHash(uint(c.y))) & 0xffffu) / 65535.0;
    float e = float(pcgHash(uint(c.x) + pcgHash(uint(d.y))) & 0xffffu) / 65535.0;
    float g = float(pcgHash(uint(d.x) + pcgHash(uint(d.y))) & 0xffffu) / 65535.0;
    return lerp(lerp(a, b, f.x), lerp(e, g, f.x), f.y);
}

float rippleHeight(float2 xz) {
    // Faint, irregular cat's-paws drifting downwind; xz is world position modulo 256 m.
    const float2 drift = g_frame.wind.xz * g_frame.time.z * 0.15;
    return rippleNoise((xz - drift) * 2.0, 512) + 0.5 * rippleNoise((xz - drift * 1.3) * 4.0, 1024);
}

// What the calm surface mirrors: the sky, or (ray traced) the land and trees around the pond as dim silhouettes.
float3 reflectedScene(float3 origin, float3 R, out float hitT) {
    hitT = 1e5;
    const float3 sky = skyColor(R);
    if (!rtEnabled()) return sky;
    RayDesc ray;
    ray.Origin = origin + float3(0, 0.03, 0);
    ray.Direction = R;
    ray.TMin = 0.05;
    ray.TMax = 4000.0;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(g_tlas[g_frame.scene.x], RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            leafAlphaHit(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return sky;
    hitT = q.CommittedRayT();
    if (q.CommittedInstanceID() == BALL_INSTANCE) {
        // A ball: its panels by the hit point on the unit sphere, lit by the suns and the sky.
        const float3 local = normalize(q.CommittedObjectRayOrigin() + q.CommittedObjectRayDirection() * q.CommittedRayT());
        const float3 bn = normalize(mul(q.CommittedObjectToWorld3x4(), float4(local, 0)));
        float3 light = skyAmbient(bn);
        [unroll] for (uint b = 0; b < 2; ++b)
            light += g_frame.sunLight[b].rgb * (1.0 - g_frame.suns[b].w) * sunTransmittance(g_frame.suns[b].xyz) * saturate(dot(bn, g_frame.suns[b].xyz));
        return applyHaze(ballAlbedo(local, 0.01).rgb * light, R * q.CommittedRayT());
    }
    const bool land = q.CommittedInstanceID() == TERRAIN_INSTANCE;
    float3 base = land ? float3(0.065, 0.085, 0.035) : float3(0.030, 0.055, 0.020);
    if (q.CommittedInstanceID() == HUMAN_INSTANCE) base = float3(0.09, 0.11, 0.12);  // the human: its shirt, mostly
    float3 sunsUp = 0;
    [unroll] for (uint s = 0; s < 2; ++s)
        sunsUp += g_frame.sunLight[s].rgb * (1.0 - g_frame.suns[s].w) * sunTransmittance(g_frame.suns[s].xyz) * saturate(g_frame.suns[s].y);
    const float3 lit = base * (skyAmbient(float3(0, 1, 0)) + sunsUp * 0.3);
    return applyHaze(lit, R * q.CommittedRayT());
}

// What the surface mirrors, with the campfire in front of it (fire is reflected in water).
float3 reflectedColor(float3 origin, float3 R) {
    float hitT;
    const float3 c = reflectedScene(origin, R, hitT);
    const float4 f = marchFire(origin + float3(0, 0.03, 0), R, hitT, 1.0);
    return c * f.a + f.rgb;
}

// The scene's colour in a direction from the eye (the frame before the water), or the sky where that leaves the screen.
// `onScreen`: how much of it came from the screen (fading out toward the edges, so nothing ends in a seam).
float3 sceneToward(float3 from, float3 dir, float reach, out float onScreen) {
    const float4 c = mul(g_frame.viewProj, float4(from + dir * reach, 1));
    onScreen = 0;
    if (c.w <= 1e-4) return 0;
    const float2 ndc = c.xy / c.w;
    onScreen = saturate((0.995 - max(abs(ndc.x), abs(ndc.y))) / 0.2);
    if (onScreen <= 0.0) return 0;
    return g_tex2D[g_push.sceneColor].SampleLevel(s_linearClamp, ndc * float2(0.5, -0.5) + 0.5, 0).rgb;
}

// The surface from under the water: inside Snell's window the world above, squeezed into a cone round the vertical;
// past the critical angle the surface mirrors the water below. All of it through the water between it and the eye.
float4 seenFromBelow(VSOut i, float3 n, float3 V, float dist) {
    const float3 down = -n;                   // the side facing the eye
    const float3 d = -V;                      // the view ray, going up
    const float3 T = refract(d, down, 1.333); // into the air; zero past the critical angle
    float F = 1.0;
    float3 above = 0;
    if (dot(T, T) > 0.5) {
        F = 0.02 + 0.98 * pow(1.0 - saturate(dot(T, n)), 5.0);
        float onScreen;
        above = sceneToward(i.rel, T, 400.0, onScreen);
        above = lerp(skyColor(T), above, onScreen);
    }
    // The mirrored water: the bed where it shows, else the water's own colour.
    float bedOnScreen;
    float3 below = sceneToward(i.rel, reflect(d, down), 25.0, bedOnScreen);
    const float depth = waterSurfaceAbove();
    below = lerp(waterGlow(depth), below, bedOnScreen);
    float3 color = throughWater(lerp(above, below, F), dist, depth * 0.5);
    if (magicView()) {
        color = magicDim(color) + waterMagicGlow(i.rel, down, V, dist) + magicFreeGlow(-V, dist, i.pos.xy) + magicMotes(-V, dist);
    }
    return float4(color, 1);
}

float4 PSMain(VSOut i) : SV_Target0 {
    const float2 screenUv = i.pos.xy * g_push.invSize;
    const float3 V = -normalize(i.rel);
    const float dist = length(i.rel);

    // Ripples: barely there in calm air, a touch more with wind, gone with distance.
    const float2 xz = g_frame.cameraWrap.xz + i.rel.xz;
    const float e = 0.05;
    const float strength = 0.012 + 0.02 * saturate(length(g_frame.wind.xz) / 8.0);
    const float fade = saturate(1.0 - dist / 90.0);
    const float hx = rippleHeight(xz + float2(e, 0)) - rippleHeight(xz - float2(e, 0));
    const float hz = rippleHeight(xz + float2(0, e)) - rippleHeight(xz - float2(0, e));
    const float3 n = normalize(float3(-hx * strength * fade / (2 * e), 1.0, -hz * strength * fade / (2 * e)));

    // How much water the light crosses before reaching the bed behind this pixel.
    const float sceneDepth = g_tex2D[g_push.depth].SampleLevel(s_pointClamp, screenUv, 0).r;
    const float3 dir = -V;
    const float sceneDist = sceneDepth > 0 ? (g_frame.cameraUp.w / sceneDepth) / max(dot(dir, g_frame.cameraForward.xyz), 1e-3) : 1e4;
    const float thickness = max(sceneDist - dist, 0.0);

    if (i.rel.y > 0.0) return seenFromBelow(i, n, V, dist);

    const float2 refrUv = screenUv + n.xz * 0.04 * saturate(thickness);
    const float3 behind = g_tex2D[g_push.sceneColor].SampleLevel(s_linearClamp, refrUv, 0).rgb;
    // The bed lies this deep: the daylight reaching it crossed that much water first.
    const float3 bed = dir * min(sceneDist, dist + 60.0);
    const float bedDepth = max(i.rel.y - bed.y, 0.0);
    const float3 refracted = throughWater(behind * waterDepthLight(bedDepth) * waterCaustics(bed, bedDepth), thickness, bedDepth * 0.5);

    if (magicView()) {  // no reflections; the water glows with the magic it holds, the free magic in front of it
        const float dist = length(i.rel);
        const float3 glow = necroticGlow(waterMagicGlow(i.rel, n, V, dist), necroticAmount(i.rel)) + magicMotes(-V, dist);
        const float4 f = marchFire(0, -V, dist, K_MAGIC_VIEW_DIM);
        return float4((magicDim(refracted) + glow + magicFreeGlow(-V, dist, i.pos.xy)) * f.a + f.rgb, 1);
    }
    const float3 R = reflect(-V, n);
    float3 reflected = reflectedColor(i.rel, R);
    const float F = 0.02 + 0.98 * pow(1.0 - saturate(dot(n, V)), 5.0);

    // The sun's glint on calm water.
    // The suns' glints on calm water (each through the air, past the planet and the clouds).
    float3 glints = 0;
    [unroll] for (uint s = 0; s < 2; ++s) {
        const float3 L = g_frame.suns[s].xyz, H = normalize(L + V);
        const float a = 0.02, a2 = a * a, NoH = saturate(dot(n, H));
        const float d = NoH * NoH * (a2 - 1.0) + 1.0;
        const float glint = a2 / (3.14159 * d * d) * F * 0.25 * saturate(dot(n, L));
        glints += g_frame.sunLight[s].rgb * (1.0 - g_frame.suns[s].w) * sunTransmittance(L) * cloudShadow(s, i.rel) * min(glint, 60.0);
    }
    float3 color = necroticTint(lerp(refracted, reflected, F), necroticAmount(i.rel)) + glints;  // what it holds of necrotic magic
    color = applyHaze(color, i.rel);
    const float4 f = marchFire(0, -V, length(i.rel), 1.0);  // the campfire between us and the water
    return float4(color * f.a + f.rgb, 1);
}
