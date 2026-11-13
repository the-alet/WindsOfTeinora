// What hangs in the sky, seen in direction dir (local frame): the two suns, the corona in an eclipse, the
// ringed gas giant (matte, smoky, blue, lit by both suns, its phase), and the band of the invented galaxy.
// Stars are drawn separately (stars.hlsl); clouds are composited over all of this afterwards.
#ifndef TAELIA_SKYBODIES_HLSLI
#define TAELIA_SKYBODIES_HLSLI

#include "atmosphere.hlsli"
#include "clouds.hlsli"

float valueNoise3(float3 p) {
    float3 i = floor(p), f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    uint3 c = uint3(int3(i) + 4096);
    float n000 = float(pcgHash(c.x + pcgHash(c.y + pcgHash(c.z))) & 0xffffu);
    float n100 = float(pcgHash(c.x + 1 + pcgHash(c.y + pcgHash(c.z))) & 0xffffu);
    float n010 = float(pcgHash(c.x + pcgHash(c.y + 1 + pcgHash(c.z))) & 0xffffu);
    float n110 = float(pcgHash(c.x + 1 + pcgHash(c.y + 1 + pcgHash(c.z))) & 0xffffu);
    float n001 = float(pcgHash(c.x + pcgHash(c.y + pcgHash(c.z + 1))) & 0xffffu);
    float n101 = float(pcgHash(c.x + 1 + pcgHash(c.y + pcgHash(c.z + 1))) & 0xffffu);
    float n011 = float(pcgHash(c.x + pcgHash(c.y + 1 + pcgHash(c.z + 1))) & 0xffffu);
    float n111 = float(pcgHash(c.x + 1 + pcgHash(c.y + 1 + pcgHash(c.z + 1))) & 0xffffu);
    float a = lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y);
    float b = lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y);
    return lerp(a, b, f.z) / 65535.0;
}

float fbmSky(float3 p) {
    return valueNoise3(p) * 0.55 + valueNoise3(p * 2.03) * 0.28 + valueNoise3(p * 4.1) * 0.17;
}

// The planet as a sphere one unit away along its direction, radius sin(apparent radius).
struct PlanetFrame {
    float3 c;      // centre
    float R;       // radius
    float3 axis;   // spin axis (normal of the ring plane)
    float3 e1, e2; // its equator
};

PlanetFrame planetFrame() {
    PlanetFrame f;
    f.c = g_frame.planet.xyz;
    f.R = sin(g_frame.planet.w);
    const float3 V = -f.c;  // from the planet toward us
    const float3 wn = g_frame.worldNorth.xyz;
    float3 north = wn - dot(wn, V) * V;
    north = dot(north, north) > 1e-6 ? normalize(north) : float3(0, 1, 0);
    const float a = g_frame.planetPose.z, B = g_frame.planetPose.y;
    const float3 onSky = north * cos(a) + cross(V, north) * sin(a);
    f.axis = normalize(onSky * cos(B) + V * sin(B));  // tilted toward or away from us: the rings open
    f.e1 = normalize(cross(f.axis, abs(f.axis.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0)));
    f.e2 = cross(f.axis, f.e1);
    return f;
}

// Ring density at a distance from the centre (planet radii): bands with gaps.
float ringDensity(float r) {
    if (r < 1.32 || r > 2.38) return 0.0;
    float d = smoothstep(1.32, 1.40, r) * (1.0 - smoothstep(2.25, 2.38, r));
    d *= 0.55 + 0.45 * sin(r * 37.0) * sin(r * 11.3 + 1.7);
    d *= 1.0 - 0.85 * (smoothstep(1.71, 1.73, r) - smoothstep(1.79, 1.81, r));  // a dark gap
    d *= 1.0 - 0.6 * (smoothstep(2.02, 2.03, r) - smoothstep(2.06, 2.07, r));
    return saturate(d);
}

// Ray (from us) against the sphere: distance to the near side, or -1.
float hitSphere(float3 dir, float3 c, float R) {
    const float b = dot(dir, c), q = dot(c, c) - R * R, h = b * b - q;
    return h < 0 ? -1.0 : b - sqrt(h);
}

float3 planetSurface(PlanetFrame f, float3 n) {
    const float lat = asin(clamp(dot(n, f.axis), -1, 1));
    const float lon = atan2(dot(n, f.e2), dot(n, f.e1)) + g_frame.planetPose.x;
    // Soft blue bands, smoky turbulence drifting along them, hazy poles.
    const float3 p = float3(cos(lon), sin(lon), lat * 2.5) * 3.0;
    const float turb = fbmSky(p + float3(0, 0, lat * 8.0));
    const float band = 0.5 + 0.5 * sin(lat * 11.0 + 1.6 * sin(lat * 4.0) + (turb - 0.5) * 2.2);
    float3 col = lerp(float3(0.09, 0.17, 0.33), float3(0.40, 0.53, 0.66), band);
    col = lerp(col, float3(0.30, 0.36, 0.44), smoothstep(0.55, 0.85, turb) * 0.5);  // smoke
    col = lerp(col, float3(0.20, 0.26, 0.36), smoothstep(0.9, 1.3, abs(lat)));        // poles
    return col;
}

// Light reaching a point of the planet system from sun i: blocked by the planet itself or thinned by the rings.
float planetSunVisibility(PlanetFrame f, float3 p, float3 L, bool onRing) {
    if (onRing) {
        if (hitSphere(L, f.c - p, f.R) > 0.0) return 0.0;  // the planet's shadow across the rings
        return 1.0;
    }
    const float t = dot(f.c - p, f.axis) / dot(L, f.axis);
    if (t > 0) {
        const float r = length(p + L * t - f.c) / f.R;
        return 1.0 - 0.8 * ringDensity(r);  // the rings' shadow on the planet
    }
    return 1.0;
}

// Sky bodies in direction dir: returns radiance; `skyThrough` gets how much of the sky behind still shows (0..1).
float3 skyBodies(float3 dir, out float skyThrough) {
    skyThrough = 1.0;
    const float3 airT = sunTransmittance(dir);  // the air between us and anything in this direction
    float3 col = 0;
    const PlanetFrame f = planetFrame();
    const float albedo = g_frame.planetPose.w;

    // The planet and its rings.
    const float tPlanet = hitSphere(dir, f.c, f.R);
    const float tRing = dot(f.c, f.axis) / dot(dir, f.axis);
    const float rRing = tRing > 0 ? length(dir * tRing - f.c) / f.R : 0.0;
    const float ringA = tRing > 0 ? ringDensity(rRing) * 0.85 : 0.0;
    float3 planetCol = 0, ringCol = 0;
    if (tPlanet > 0) {
        const float3 p = dir * tPlanet, n = normalize(p - f.c);
        const float3 surf = planetSurface(f, n);
        [unroll] for (int i = 0; i < 2; ++i) {
            const float3 L = g_frame.planetLit[i].xyz;
            planetCol += surf * g_frame.sunLight[i].rgb * saturate(dot(n, L)) * planetSunVisibility(f, p, L, false);
        }
        planetCol *= albedo / 0.25;  // matte; bands carry the colour (their mean is about 0.25)
    }
    if (ringA > 0) {
        const float3 p = dir * tRing;
        const float towardUs = dot(-f.c, f.axis);
        [unroll] for (int i = 0; i < 2; ++i) {
            const float3 L = g_frame.planetLit[i].xyz;
            const float face = dot(L, f.axis) * towardUs >= 0.0 ? 1.0 : 0.35;  // the unlit face: light seeping through
            ringCol += float3(0.62, 0.70, 0.78) * g_frame.sunLight[i].rgb * 0.18 * face * planetSunVisibility(f, p, L, true);
        }
    }
    if (tPlanet > 0) {
        skyThrough = 0.0;
        col = (ringA > 0 && tRing < tPlanet) ? lerp(planetCol, ringCol, ringA) : planetCol;
        return col * airT;
    }
    if (ringA > 0) {
        skyThrough = 1.0 - ringA;
        col = ringCol * ringA * airT;
    }

    // The suns (behind the planet when it covers them), and the corona when a sun is mostly hidden.
    [unroll] for (int i = 0; i < 2; ++i) {
        const float3 L = g_frame.suns[i].xyz;
        const float r = g_frame.sunLight[i].w;
        const float th = acos(clamp(dot(dir, L), -1, 1));
        // Through the air and the clouds in front of it (a thin cloud must not let the disc show).
        const float3 light = g_frame.sunLight[i].rgb * sunTransmittance(L) * cloudShadow(uint(i), float3(0, 0, 0));
        if (th < r * 1.05) {
            const float x = saturate(th / r), mu = sqrt(saturate(1.0 - x * x));
            const float limb = 1.0 - 0.6 * (1.0 - mu);  // darker toward the edge
            const float edge = 1.0 - smoothstep(r * 0.97, r * 1.05, th);
            col += min(light * limb * edge / (3.14159 * r * r * 0.8), 2000.0) * skyThrough;
        }
        // The corona, only in totality: pearly and smooth (no stripes), falling steeply from the hidden sun's limb
        // into a faint tail. The planet is wider than the suns, so what shows is the part past its limb.
        const float total = smoothstep(0.97, 1.0, g_frame.suns[i].w);
        if (total > 0.0 && th < r * 18.0) {
            const float x = max(th / r, 1.0);  // in sun radii
            const float k = 0.7 * pow(x, -3.0) + 0.3 * pow(x, -2.0);
            const float3 pearl = lerp(light, dot(light, float3(0.3333, 0.3333, 0.3333)), 0.45);
            col += pearl * k * 0.15 * total * (1.0 - smoothstep(7.0, 18.0, x)) * skyThrough;
        }
    }

    // The band of the invented galaxy: faint, dusty, seen only in the dark.
    const float3 inertial = mul(transpose((float3x3)g_frame.starRotation), dir);
    const float lat = asin(clamp(dot(inertial, g_frame.galacticPole.xyz), -1, 1));
    const float band = exp(-(lat * lat) / (0.17 * 0.17));
    const float dust = smoothstep(0.35, 0.75, fbmSky(inertial * 7.0)) * exp(-(lat * lat) / (0.05 * 0.05));
    const float glow = band * (0.55 + 0.45 * fbmSky(inertial * 3.0)) * (1.0 - 0.7 * dust);
    col += float3(1.0, 0.94, 0.84) * glow * 2.5e-4 * g_frame.galacticPole.w * airT * skyThrough;
    return col;
}

#endif
