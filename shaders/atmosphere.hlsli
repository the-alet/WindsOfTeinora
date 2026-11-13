// The air: the interface every pass uses for sky light and haze (step 3).
//   float3 skyRadiance(float3 dir)        light of the sky itself in direction dir (no sun discs, planet, stars or clouds)
//   float3 sunTransmittance(float3 dir)   how much of a sun's light from dir reaches the camera through the air (0..1 rgb)
//   float3 skyIrradiance(float3 n)        light from the whole sky onto a surface facing n (ambient, includes night light)
//   float3 aerialPerspective(float3 color, float3 rel)  haze between the camera and a camera-relative point
// Units: the same as FrameConstants::sunLight (a white surface lit by `sunLight` shows albedo * sunLight, so the sky's
// radiance is pi x physical). Hillaire 2020 tables, built by engine/render/atmosphere.* with atmosphere_luts.hlsl:
// g_frame.atmosphere x transmittance (256x64), y multiple scattering (32x32), z sky view (192x108),
// w aerial perspective (32^3 camera froxels to ATMO_AP_RANGE km); atmosphere2.x sky irradiance (SH9 + overcast grey).
// Lights: both suns (minus the part the planet hides) and the planet's diffuse light. Airglow only in the high sky,
// so night air in front of far hills stays dark.
#ifndef TAELIA_ATMOSPHERE_HLSLI
#define TAELIA_ATMOSPHERE_HLSLI

#include "common.hlsli"

static const float ATMO_PI = 3.14159265;
static const float ATMO_R = K_WORLD_RADIUS_KM;   // sea level, km
static const float ATMO_H = 80.0;                // thickness of the air, km
static const float ATMO_HORIZON = sqrt(ATMO_H * (2.0 * ATMO_R + ATMO_H));  // ground-to-top tangent distance, km
static const float3 ATMO_RAYLEIGH = float3(5.802e-3, 13.558e-3, 33.1e-3);  // per km at sea level
static const float ATMO_RAYLEIGH_H = 8.0;
static const float ATMO_MIE_SCAT = 7.0e-3;
static const float ATMO_MIE_EXT = 7.8e-3;
static const float ATMO_MIE_H = 1.2;
static const float ATMO_MIE_G = 0.78;
static const float3 ATMO_OZONE = float3(0.650e-3, 1.881e-3, 0.085e-3);
static const float ATMO_GROUND_ALBEDO = 0.3;
static const float ATMO_AP_RANGE = 40.0;         // km, the aerial perspective volume's last slice
static const float ATMO_AIRGLOW_H = 90.0;        // km, the glowing layer
static const float ATMO_UMBRA_KM = 120.0;        // radius of the planet's shadow on the world in an eclipse
static const float ATMO_AIRGLOW_GAIN = 15.0;      // scale on nightLight.w: the night sky is never black (the eye adapts)
static const uint2 ATMO_T_SIZE = uint2(256, 64);
static const uint2 ATMO_MS_SIZE = uint2(32, 32);
static const uint2 ATMO_SV_SIZE = uint2(192, 108);
static const uint ATMO_AP_SIZE = 32;

// ---- geometry (heights h in km above sea level; forms that keep float precision on a 40,000 km sphere)
float atmoCameraHeight() { return clamp(g_frame.worldNorth.w * 0.001, 0.002, ATMO_H - 0.1); }
float atmoRho(float h) { return sqrt(max(h * (2.0 * ATMO_R + h), 0.0)); }  // sqrt(r^2 - R^2)
float atmoHorizonMu(float h) { return -atmoRho(h) / (ATMO_R + h); }
float atmoDistanceToTop(float h, float mu) {
    const float r = ATMO_R + h;
    const float disc = r * r * mu * mu + (ATMO_H - h) * (2.0 * ATMO_R + ATMO_H + h);
    return max(-r * mu + sqrt(max(disc, 0.0)), 0.0);
}
float atmoDistanceToGround(float h, float mu) {  // -1 if the ray misses the ground
    const float r = ATMO_R + h;
    const float disc = r * r * mu * mu - h * (2.0 * ATMO_R + h);
    if (mu >= 0.0 || disc < 0.0) return -1.0;
    return max(-r * mu - sqrt(disc), 0.0);
}
// Height after going t km from height h along a ray with cosine mu to the local up.
float atmoHeightAt(float h, float mu, float t) {
    const float x = h * (2.0 * ATMO_R + h) + t * (t + 2.0 * (ATMO_R + h) * mu);
    return x / (sqrt(max(ATMO_R * ATMO_R + x, 0.0)) + ATMO_R);
}
// Light from below the horizon is cut by the planet (soft over about a sun's width).
float atmoGroundShadow(float h, float mu) {
    const float muH = atmoHorizonMu(h);
    return smoothstep(muH - 0.006, muH + 0.006, mu);
}

float atmoToTexel(float x, uint size) { return 0.5 / float(size) + x * (1.0 - 1.0 / float(size)); }
float atmoFromTexel(float u, uint size) { return (u - 0.5 / float(size)) / (1.0 - 1.0 / float(size)); }

float2 atmoTransmittanceUv(float h, float mu) {
    const float rho = atmoRho(h);
    const float dMin = ATMO_H - h, dMax = rho + ATMO_HORIZON;
    const float d = atmoDistanceToTop(h, mu);
    return float2(atmoToTexel(saturate((d - dMin) / max(dMax - dMin, 1e-6)), ATMO_T_SIZE.x),
                  atmoToTexel(saturate(rho / ATMO_HORIZON), ATMO_T_SIZE.y));
}
float3 atmoTransmittance(float h, float mu) {
    return g_tex2D[g_frame.atmosphere.x].SampleLevel(s_linearClamp, atmoTransmittanceUv(h, mu), 0).rgb;
}
float3 atmoMultiScattering(float h, float muL) {
    const float2 uv = float2(atmoToTexel(saturate(muL * 0.5 + 0.5), ATMO_MS_SIZE.x), atmoToTexel(saturate(h / ATMO_H), ATMO_MS_SIZE.y));
    return g_tex2D[g_frame.atmosphere.y].SampleLevel(s_linearClamp, uv, 0).rgb;
}

// Sky view LUT: u = azimuth (atan2(z, x) / 2pi, wraps), v = 0 zenith, 0.5 horizon, 1 nadir (dense near the horizon).
float2 atmoSkyViewUv(float h, float3 dir) {
    const float beta = acos(clamp(atmoRho(h) / (ATMO_R + h), 0.0, 1.0));
    const float zh = ATMO_PI - beta;  // zenith angle of the horizon
    const float vz = acos(clamp(dir.y, -1.0, 1.0));
    const float v = vz < zh ? 0.5 * (1.0 - sqrt(saturate(1.0 - vz / zh))) : 0.5 + 0.5 * sqrt(saturate((vz - zh) / max(beta, 1e-6)));
    const float u = atan2(dir.z, dir.x) / (2.0 * ATMO_PI);
    return float2(u, (v * float(ATMO_SV_SIZE.y - 1) + 0.5) / float(ATMO_SV_SIZE.y));
}
float3 atmoSkyViewDir(float h, float2 uv) {  // uv.y in 0..1 over the rows (row centres at the ends)
    const float beta = acos(clamp(atmoRho(h) / (ATMO_R + h), 0.0, 1.0));
    const float zh = ATMO_PI - beta;
    float vz;
    if (uv.y < 0.5) { const float c = 1.0 - 2.0 * uv.y; vz = zh * (1.0 - c * c); }
    else { const float c = 2.0 * uv.y - 1.0; vz = zh + beta * c * c; }
    const float phi = uv.x * 2.0 * ATMO_PI;
    return float3(cos(phi) * sin(vz), cos(vz), sin(phi) * sin(vz));
}

// Airglow: a thin glowing layer high up, brighter toward the horizon (seen edge-on), dimmed by the air below it.
float3 atmoAirglow(float h, float3 dir) {
    if (atmoDistanceToGround(h, dir.y) >= 0.0) return 0;
    const float r = ATMO_R + h, rg = ATMO_R + ATMO_AIRGLOW_H;
    const float s = (r / rg) * sqrt(saturate(1.0 - dir.y * dir.y));
    const float slant = min(rsqrt(max(1.0 - s * s, 1e-4)), 8.0);
    return g_frame.nightLight.w * ATMO_AIRGLOW_GAIN * slant * float3(0.38, 0.58, 1.0) * atmoTransmittance(h, dir.y);
}

// ---- the interface

float3 sunTransmittance(float3 dir) {
    const float h = atmoCameraHeight();
    return atmoTransmittance(h, dir.y) * atmoGroundShadow(h, dir.y);
}

float3 skyRadiance(float3 dir) {
    const float h = atmoCameraHeight();
    const float3 sky = g_tex2D[g_frame.atmosphere.z].SampleLevel(s_linearWrap, atmoSkyViewUv(h, dir), 0).rgb;
    return sky + atmoAirglow(h, dir);
}

// Real SH (l <= 2) in the local frame.
void atmoSh9(float3 n, out float sh[9]) {
    sh[0] = 0.282095;
    sh[1] = 0.488603 * n.y;
    sh[2] = 0.488603 * n.z;
    sh[3] = 0.488603 * n.x;
    sh[4] = 1.092548 * n.x * n.y;
    sh[5] = 1.092548 * n.y * n.z;
    sh[6] = 0.315392 * (3.0 * n.z * n.z - 1.0);
    sh[7] = 1.092548 * n.x * n.z;
    sh[8] = 0.546274 * (n.x * n.x - n.y * n.y);
}

float3 skyIrradiance(float3 n) {
    // Irradiance buffer: 9 float4 (cosine-convolved SH of the sky, ground bounce below the horizon), then the overcast grey.
    float sh[9];
    atmoSh9(n, sh);
    const uint buf = g_frame.atmosphere2.x;
    float3 e = 0;
    [unroll] for (uint k = 0; k < 9; ++k) e += asfloat(g_buffers[buf].Load3(k * 16)) * sh[k];
    e = max(e, 0.0);
    // Under a closed sky the light comes grey, mostly from above.
    const float cover = smoothstep(0.25, 1.0, g_frame.clouds.z);
    const float grey = asfloat(g_buffers[buf].Load(9 * 16));
    e = lerp(e, grey * (0.7 + 0.3 * n.y), cover);
    // Night: the planet's diffuse light (from its side) and the faint glow of the air.
    const float facing = 0.5 + 0.5 * saturate(dot(n, g_frame.planet.xyz));
    return e + g_frame.nightLight.rgb * facing * (1.0 - 0.6 * cover) + g_frame.nightLight.w * 2.0;
}

float3 aerialPerspective(float3 color, float3 rel) {
    const float dist = length(rel);
    if (dist < 1e-3) return color;
    const float viewZ = max(dot(rel, g_frame.cameraForward.xyz), dist * 1e-3);  // behind the camera (reflections): clamp to the edge
    const float t = g_frame.cameraForward.w, aspect = g_frame.cameraRight.w;
    const float2 ndc = float2(dot(rel, g_frame.cameraRight.xyz) / (t * aspect), dot(rel, g_frame.cameraUp.xyz) / t) / viewZ;
    const float2 uv = saturate(float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5));
    // Slice i holds distance ATMO_AP_RANGE * ((i + 1) / N)^2.
    const float n = float(ATMO_AP_SIZE);
    const float s = sqrt(dist * 0.001 / ATMO_AP_RANGE) * n;
    float4 v = g_tex3D[g_frame.atmosphere.w].SampleLevel(s_linearClamp, float3(uv, (clamp(s, 1.0, n) - 0.5) / n), 0);
    if (s < 1.0) {
        v = lerp(float4(0, 0, 0, 1), v, s);
    } else if (s > n) {
        // Past the volume: the same air keeps going.
        const float k = (dist * 0.001) / ATMO_AP_RANGE;
        const float tFar = pow(max(v.a, 1e-4), k);
        v.rgb *= (1.0 - tFar) / max(1.0 - v.a, 1e-4);
        v.a = tFar;
    }
    return color * v.a + v.rgb;
}

#endif
