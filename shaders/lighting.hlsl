// Lights the G-buffer: the sun (with a ray-traced shadow when the scene has a TLAS), light from the sky, haze.
#include "common.hlsli"
#include "gbuffer.hlsli"
#include "sky.hlsli"
#include "rt.hlsli"
#include "skybodies.hlsli"
#include "clouds.hlsli"
#include "magic.hlsli"
#include "fire.hlsli"
#include "water_optics.hlsli"

struct LightPush {
    uint albedo, normal, material, depth;
    uint outUav, width, height, pad;
};
[[vk::push_constant]] ConstantBuffer<LightPush> g_push : register(b1, space0);

// Whether the light from direction L reaches rel (no farther than maxT): ray traced when possible.
float rayVisibility(float3 rel, float3 n, float3 L, float maxT) {
    if (!rtEnabled()) return 1.0;
    const uint tlas = g_frame.scene.x;
    RayDesc ray;
    ray.Origin = rel + n * (0.02 + 0.0004 * length(rel));
    ray.Direction = L;
    ray.TMin = 0.01;
    ray.TMax = maxT;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(g_tlas[tlas], RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            leafAlphaHit(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

float sunVisibility(float3 rel, float3 n, float3 L) {
    if (L.y < -0.05) return 0.0;
    return rayVisibility(rel, n, L, 6000.0);
}

// Light under skin spreads past the terminator, red farthest: a cheap stand-in for scattering (subsurface 0: none).
float3 diffuseWrap(float NoL, float subsurface) {
    const float3 w = subsurface * float3(0.42, 0.18, 0.10);
    return saturate((NoL + w) / (1.0 + w));
}

float D_GGX(float NoH, float a) {
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * d * d);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_push.width || id.y >= g_push.height) return;
    const float depth = g_tex2D[g_push.depth].Load(int3(id.xy, 0)).r;
    const float2 uv = (float2(id.xy) + 0.5) / float2(g_push.width, g_push.height);
    const float3 dir = viewRay(uv);
    float3 color;
    if (depth <= 0.0) {
        // The sky itself, then what hangs in it (stars come next, clouds over all of it).
        float through;
        const float3 bodies = skyBodies(dir, through);
        color = skyRadiance(dir) + bodies;  // the air in front of everything out there scatters its own light
        if (magicView()) color = magicDim(color) + magicFreeGlow(dir, 1e9, float2(id.xy)) + magicMotes(dir, 1e9);
        else color += magicDewTint(dir, 1e9);
    } else {
        const float viewZ = g_frame.cameraUp.w / depth;
        const float3 rel = dir * (viewZ / dot(dir, g_frame.cameraForward.xyz));
        const float4 a = g_tex2D[g_push.albedo].Load(int3(id.xy, 0));
        const float4 nr = g_tex2D[g_push.normal].Load(int3(id.xy, 0));
        const float4 m = g_tex2D[g_push.material].Load(int3(id.xy, 0));
        const float3 n = normalize(nr.xyz);
        const float rough = max(nr.w, 0.08), translucency = m.r, specular = m.g, subsurface = m.b;
        float3 albedo = a.rgb, ownGlow = 0;
        float ao = a.a;
        if (magicView()) {  // the albedo target holds the surface's own glow, and its plain brightness in a
            ownGlow = a.rgb * K_MAGIC_GLOW_SCALE;
            albedo = a.aaa;
            ao = 1.0;
        }
        const float3 V = -dir;

        // Two suns: each through the air, past the planet (eclipses), the clouds and whatever casts a shadow.
        float3 direct = 0, back = 0, glint = 0;
        [unroll] for (uint i = 0; i < 2; ++i) {
            const float3 L = g_frame.suns[i].xyz;
            if (L.y < -0.1) continue;
            float3 sun = g_frame.sunLight[i].rgb * (1.0 - g_frame.suns[i].w) * sunTransmittance(L);
            if (max(sun.r, max(sun.g, sun.b)) < 1e-5) continue;
            sun *= cloudShadow(i, rel);
            sun *= sunVisibility(rel, n, L);
            const float NoL = dot(n, L);
            direct += sun * diffuseWrap(NoL, subsurface);
            back += sun * saturate(-NoL);  // leaves and blades let light through
            const float3 H = normalize(L + V);
            const float NoH = saturate(dot(n, H)), VoH = saturate(dot(V, H));
            const float F = 0.04 + 0.96 * pow(1.0 - VoH, 5.0);
            glint += sun * saturate(NoL) * D_GGX(NoH, rough * rough) * F * 0.25 / max(saturate(dot(n, V)) * saturate(NoL), 0.1);
        }
        // The campfire's flickering light, shadowed.
        const float3 fl = fireLight(rel, n);
        if (fl.r > 1e-5) {
            const float3 toFire = g_frame.fire.xyz + float3(0, 0.6 * fireStrength(), 0) - rel;
            const float dFire = length(toFire);
            direct += fl * rayVisibility(rel, n, toFire / dFire, max(dFire - 0.35, 0.05));
        }
        direct += otherFlamesLight(rel, n);
        direct += float3(0.7, 0.74, 1.0) * 0.5 * g_frame.weather.z * saturate(n.y * 0.5 + 0.5);  // a lightning flash, from above
        const float3 diffuse = albedo * (direct + skyIrradiance(n) * ao + back * translucency * 0.6);
        color = diffuse + glint * specular;
        const float dist = length(rel);
        if (cameraUnderwater() && rel.y < waterSurfaceAbove()) {
            // Seen from under a pond's surface: lit by the daylight the water above lets through, seen through the
            // water between (the straight line from the eye stays under the surface).
            const float depthHere = waterSurfaceAbove() - rel.y;
            color = throughWater(color * waterDepthLight(depthHere) * waterCaustics(rel, depthHere), dist, waterSurfaceAbove() - rel.y * 0.5);
        } else {
            color = aerialPerspective(color, rel);
        }
        if (magicView()) {
            const float fade = 1.0 - smoothstep(g_frame.magicInfo.z * 0.8, g_frame.magicInfo.z, dist);
            color = magicDim(color) + (ownGlow + magicOutlineGlow(m.a, n, V)) * fade + magicFreeGlow(dir, dist, float2(id.xy)) +
                    magicMotes(dir, dist);
        } else {
            color += magicDewTint(dir, dist);
        }
        const float4 f = marchFire(0, dir, dist, magicView() ? K_MAGIC_VIEW_DIM : 1.0);  // in front of the surface
        color = color * f.a + f.rgb;
    }
    g_rwTex2D[g_push.outUav][id.xy] = float4(color, 1);
}
