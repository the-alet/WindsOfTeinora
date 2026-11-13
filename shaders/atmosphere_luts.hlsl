// Atmosphere tables (Hillaire 2020): transmittance and multiple scattering once, sky view, aerial perspective
// and sky irradiance each frame. Radiance is stored x pi (see atmosphere.hlsli: units of FrameConstants::sunLight).
#include "atmosphere.hlsli"

struct AtmoPush {
    uint outIndex;  // UAV of the table this kernel writes
    uint pad0, pad1, pad2;
};
[[vk::push_constant]] ConstantBuffer<AtmoPush> g_push : register(b1, space0);

struct AtmoMedium {
    float3 scatR;  // Rayleigh scattering
    float scatM;   // Mie scattering
    float3 ext;    // total extinction
};

AtmoMedium atmoMedium(float h) {
    h = max(h, 0.0);
    const float dR = exp(-h / ATMO_RAYLEIGH_H), dM = exp(-h / ATMO_MIE_H);
    const float dO = max(0.0, 1.0 - abs(h - 25.0) / 15.0);
    AtmoMedium m;
    m.scatR = ATMO_RAYLEIGH * dR;
    m.scatM = ATMO_MIE_SCAT * dM;
    m.ext = m.scatR + ATMO_MIE_EXT * dM + ATMO_OZONE * dO;
    return m;
}

float rayleighPhase(float c) { return 3.0 / (16.0 * ATMO_PI) * (1.0 + c * c); }
float miePhase(float c) {
    const float g = ATMO_MIE_G, g2 = g * g;
    const float k = 3.0 / (8.0 * ATMO_PI) * (1.0 - g2) / (2.0 + g2);
    return k * (1.0 + c * c) / pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5);
}

// The three lights: the suns and the planet's diffuse light, for air t km from us along viewDir. In an eclipse
// the planet's shadow is a cylinder along the sun, ATMO_UMBRA_KM in radius around us: air farther out still has
// its sun, so in totality the horizon glows, and a little of that light, scattered on, reaches the air above us
// (`multiColor`, for multiple scattering).
void atmoLight(uint i, float3 viewDir, float t, out float3 dir, out float3 color, out float3 multiColor) {
    if (i < 2) {
        dir = g_frame.suns[i].xyz;
        const float cosT = dot(viewDir, dir);
        const float off = t * sqrt(saturate(1.0 - cosT * cosT));  // from the shadow's axis
        const float shade = g_frame.suns[i].w * (1.0 - smoothstep(ATMO_UMBRA_KM, ATMO_UMBRA_KM * 2.5, off));
        color = g_frame.sunLight[i].rgb * (1.0 - shade);
        multiColor = g_frame.sunLight[i].rgb * (1.0 - shade * 0.995);
    } else {
        dir = g_frame.planet.xyz;
        color = g_frame.nightLight.rgb;
        multiColor = color;
    }
}

// In-scattered light per km at height h (local up `up`), t km from us, toward -viewDir: single + multiple scattering.
float3 atmoInscatter(float h, float3 up, float3 viewDir, float t, AtmoMedium m) {
    float3 s = 0;
    [unroll] for (uint i = 0; i < 3; ++i) {
        float3 L, c, cm;
        atmoLight(i, viewDir, t, L, c, cm);
        const float muL = dot(up, L);
        const float3 tr = atmoTransmittance(h, muL) * atmoGroundShadow(h, muL);
        const float cosT = dot(viewDir, L);
        const float3 single = (m.scatR * rayleighPhase(cosT) + m.scatM * miePhase(cosT)) * tr;
        const float3 multi = atmoMultiScattering(h, muL) * (m.scatR + m.scatM);
        s += c * single + cm * multi;
    }
    return s;
}

// Marches from the camera along dir between t0 and t1 (km), adding to L and multiplying T.
void atmoMarch(float h0, float3 dir, float t0, float t1, uint steps, inout float3 L, inout float3 T) {
    const float dt = (t1 - t0) / float(steps);
    for (uint i = 0; i < steps; ++i) {
        const float t = t0 + (float(i) + 0.5) * dt;
        const float h = atmoHeightAt(h0, dir.y, t);
        const float3 up = normalize(float3(dir.x * t, ATMO_R + h0 + dir.y * t, dir.z * t));
        const AtmoMedium m = atmoMedium(h);
        const float3 s = atmoInscatter(h, up, dir, t, m);
        const float3 stepT = exp(-m.ext * dt);
        L += T * (s - s * stepT) / max(m.ext, 1e-7);
        T *= stepT;
    }
}

[numthreads(8, 8, 1)] void TransmittanceMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= ATMO_T_SIZE)) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(ATMO_T_SIZE);
    const float xMu = atmoFromTexel(uv.x, ATMO_T_SIZE.x), xR = atmoFromTexel(uv.y, ATMO_T_SIZE.y);
    const float rho = ATMO_HORIZON * xR;
    const float h = rho * rho / (sqrt(ATMO_R * ATMO_R + rho * rho) + ATMO_R);
    const float r = ATMO_R + h;
    const float dMin = ATMO_H - h, dMax = rho + ATMO_HORIZON;
    const float d = dMin + xMu * (dMax - dMin);
    const float mu = d <= 1e-4 ? 1.0 : clamp(((ATMO_H - h) * (2.0 * ATMO_R + ATMO_H + h) - d * d) / (2.0 * r * d), -1.0, 1.0);
    const uint steps = 64;
    const float dt = d / float(steps);
    float3 od = 0;
    for (uint i = 0; i < steps; ++i) od += atmoMedium(atmoHeightAt(h, mu, (float(i) + 0.5) * dt)).ext * dt;
    g_rwTex2D[g_push.outIndex][id.xy] = float4(exp(-od), 1);
}

// One group per texel, one thread per direction (8x8 over the sphere).
groupshared float3 gs_a[64];
groupshared float3 gs_b[64];

[numthreads(64, 1, 1)] void MultiScatteringMain(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    const float2 uv = (float2(gid.xy) + 0.5) / float2(ATMO_MS_SIZE);
    const float muS = atmoFromTexel(uv.x, ATMO_MS_SIZE.x) * 2.0 - 1.0;
    const float h = max(atmoFromTexel(uv.y, ATMO_MS_SIZE.y) * ATMO_H, 0.002);
    const float3 Ls = float3(sqrt(saturate(1.0 - muS * muS)), muS, 0.0);
    const float a = (float(tid % 8) + 0.5) / 8.0, b = (float(tid / 8) + 0.5) / 8.0;
    const float theta = 2.0 * ATMO_PI * a, cphi = 1.0 - 2.0 * b, sphi = sqrt(saturate(1.0 - cphi * cphi));
    const float3 dir = float3(cos(theta) * sphi, cphi, sin(theta) * sphi);
    const float tG = atmoDistanceToGround(h, dir.y);
    const float tMax = tG >= 0.0 ? tG : atmoDistanceToTop(h, dir.y);
    const uint steps = 24;
    const float dt = tMax / float(steps);
    float3 L = 0, F = 0, T = 1;
    for (uint i = 0; i < steps; ++i) {
        const float t = (float(i) + 0.5) * dt;
        const float hs = atmoHeightAt(h, dir.y, t);
        const float3 up = normalize(float3(dir.x * t, ATMO_R + h + dir.y * t, dir.z * t));
        const AtmoMedium m = atmoMedium(hs);
        const float muL = dot(up, Ls);
        const float3 scat = m.scatR + m.scatM;
        const float3 s = scat * atmoTransmittance(hs, muL) * atmoGroundShadow(hs, muL) / (4.0 * ATMO_PI);
        const float3 stepT = exp(-m.ext * dt);
        const float3 ext = max(m.ext, 1e-7);
        L += T * (s - s * stepT) / ext;
        F += T * (scat - scat * stepT) / ext;
        T *= stepT;
    }
    if (tG >= 0.0) {  // the lit ground bounces some back
        const float3 up = normalize(float3(dir.x * tG, ATMO_R + h + dir.y * tG, dir.z * tG));
        const float muL = dot(up, Ls);
        L += T * atmoTransmittance(0.0, muL) * saturate(muL) * ATMO_GROUND_ALBEDO / ATMO_PI;
    }
    gs_a[tid] = L;
    gs_b[tid] = F;
    for (uint s2 = 32; s2 > 0; s2 >>= 1) {
        GroupMemoryBarrierWithGroupSync();
        if (tid < s2) { gs_a[tid] += gs_a[tid + s2]; gs_b[tid] += gs_b[tid + s2]; }
    }
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0) {
        const float3 l2 = gs_a[0] / 64.0, fms = gs_b[0] / 64.0;
        g_rwTex2D[g_push.outIndex][gid.xy] = float4(l2 / max(1.0 - fms, 1e-3), 1);
    }
}

[numthreads(8, 8, 1)] void SkyViewMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= ATMO_SV_SIZE)) return;
    const float h = atmoCameraHeight();
    const float2 uv = float2((float(id.x) + 0.5) / float(ATMO_SV_SIZE.x), float(id.y) / float(ATMO_SV_SIZE.y - 1));
    const float3 dir = atmoSkyViewDir(h, uv);
    const float tG = atmoDistanceToGround(h, dir.y);
    const float tMax = tG >= 0.0 ? tG : atmoDistanceToTop(h, dir.y);
    // Steps crowd near the camera, where the air is densest.
    float3 L = 0, T = 1;
    const uint segments = 8;
    float t0 = 0.0;
    for (uint i = 0; i < segments; ++i) {
        const float f = float(i + 1) / float(segments);
        const float t1 = tMax * f * f;
        atmoMarch(h, dir, t0, t1, 4, L, T);
        t0 = t1;
    }
    g_rwTex2D[g_push.outIndex][id.xy] = float4(L * ATMO_PI, 1);
}

// Camera froxels: slice i ends at ATMO_AP_RANGE * ((i + 1) / N)^2 km.
[numthreads(8, 8, 1)] void AerialPerspectiveMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= ATMO_AP_SIZE)) return;
    const float h = atmoCameraHeight();
    const float3 dir = viewRay((float2(id.xy) + 0.5) / float(ATMO_AP_SIZE));
    float3 L = 0, T = 1;
    float t0 = 0.0;
    for (uint i = 0; i < ATMO_AP_SIZE; ++i) {
        const float f = float(i + 1) / float(ATMO_AP_SIZE);
        const float t1 = ATMO_AP_RANGE * f * f;
        atmoMarch(h, dir, t0, t1, 2, L, T);
        t0 = t1;
        g_rwTex3D[g_push.outIndex][uint3(id.xy, i)] = float4(L * ATMO_PI, dot(T, 1.0 / 3.0));
    }
}

// Sky irradiance: SH9 of the sky (sky view LUT above the horizon, the lit ground below), cosine-convolved, / pi;
// then a grey for the overcast sky. One group, 512 directions (32 azimuths x 16 bands uniform in mu).
groupshared float3 gs_r[256];

float3 atmoReduce(uint tid, float3 v) {
    GroupMemoryBarrierWithGroupSync();
    gs_r[tid] = v;
    for (uint s = 128; s > 0; s >>= 1) {
        GroupMemoryBarrierWithGroupSync();
        if (tid < s) gs_r[tid] += gs_r[tid + s];
    }
    GroupMemoryBarrierWithGroupSync();
    return gs_r[0];
}

float3 atmoIrrDir(uint k) {
    const float mu = 1.0 - (float(k / 32) + 0.5) / 8.0;
    const float phi = (float(k % 32) + 0.5) / 32.0 * 2.0 * ATMO_PI;
    const float s = sqrt(saturate(1.0 - mu * mu));
    return float3(cos(phi) * s, mu, sin(phi) * s);
}

[numthreads(256, 1, 1)] void IrradianceMain(uint tid : SV_GroupIndex) {
    const float h = atmoCameraHeight();
    const float dw = 4.0 * ATMO_PI / 512.0;
    float3 radiance[2];
    float3 down = 0;
    [unroll] for (uint j = 0; j < 2; ++j) {
        const float3 d = atmoIrrDir(tid * 2 + j);
        radiance[j] = d.y > 0.0 ? skyRadiance(d) : 0.0;
        down += radiance[j] * saturate(d.y) * dw / ATMO_PI;
    }
    const float3 skyDown = atmoReduce(tid, down);
    // Direct light on open ground (through the air), for the ground bounce and the overcast grey.
    float3 direct = 0;
    [unroll] for (uint i = 0; i < 3; ++i) {
        float3 L, c, cm;
        atmoLight(i, float3(0, 1, 0), 0.0, L, c, cm);  // here, under the shadow
        direct += c * atmoTransmittance(h, L.y) * atmoGroundShadow(h, L.y) * saturate(L.y);
    }
    const float3 groundL = ATMO_GROUND_ALBEDO * (direct + skyDown);
    float3 shOut[9];
    [unroll] for (uint k = 0; k < 9; ++k) {
        float3 acc = 0;
        [unroll] for (uint j2 = 0; j2 < 2; ++j2) {
            const float3 d = atmoIrrDir(tid * 2 + j2);
            float sh[9];
            atmoSh9(d, sh);
            acc += (d.y > 0.0 ? radiance[j2] : groundL) * sh[k] * dw;
        }
        shOut[k] = atmoReduce(tid, acc);
    }
    if (tid == 0) {
        const float band[3] = {ATMO_PI, 2.0 * ATMO_PI / 3.0, ATMO_PI / 4.0};
        [unroll] for (uint k2 = 0; k2 < 9; ++k2) {
            const uint l = k2 == 0 ? 0 : (k2 < 4 ? 1 : 2);
            g_rwBuffers[g_push.outIndex].Store4(k2 * 16, asuint(float4(shOut[k2] * band[l] / ATMO_PI, 0)));
        }
        const float grey = dot(skyDown + direct, float3(0.2126, 0.7152, 0.0722)) * 0.35;
        g_rwBuffers[g_push.outIndex].Store4(9 * 16, asuint(float4(grey, grey, grey, 0)));
    }
}
