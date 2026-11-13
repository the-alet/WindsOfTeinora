// Weather over the lit scene (step 5g). Rain: thin streaks at four depths, each in front of whatever is farther,
// falling and slanted by the wind, catching the sky's light; under rain everything is a little greyer. Never a flat
// texture: the streaks hang in space around the camera at their depths, so nearer ones sweep past farther ones.
// Lightning: the last strike's bolt for a moment. In the magic view the rain is blue.
#include "common.hlsli"
#include "atmosphere.hlsli"

struct WeatherPush {
    uint hdrUav, depthSrv, width, height;
};
[[vk::push_constant]] ConstantBuffer<WeatherPush> g_push : register(b1, space0);

float cellHash(int2 c, uint layer) {
    uint h = uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ layer * 83492791u;
    h = (h ^ (h >> 13)) * 0x5bd1e995u;
    return float((h ^ (h >> 15)) & 0xffffu) / 65535.0;
}

// The bolt: a jagged line from the cloud base down to where it hit; how close this view ray passes (radians).
float boltGlow(float3 dir, float sceneDist) {
    const float3 ground = g_frame.bolt[0].xyz, top = g_frame.bolt[1].xyz;
    const float age = g_frame.bolt[1].w;
    if (age > 0.3 || age < 0.0) return 0;
    const uint seed = asuint(g_frame.bolt[0].w);
    float best = 1e9;
    float3 a = top;
    const float3 down = ground - top;
    const float3 side = normalize(cross(down, float3(0, 1, 0.01)));
    for (uint i = 1; i <= 12; ++i) {
        const float f = float(i) / 12.0;
        const float j = (cellHash(int2(int(seed), int(i)), 7u) - 0.5) * (i == 12 ? 0.0 : 1.0);
        const float3 b = top + down * f + side * j * length(down) * 0.06;
        // Closest approach of the ray to the segment a-b, as an angle.
        const float3 ab = b - a;
        const float t = saturate(dot(dir * dot(a + ab * 0.5, dir) - a, ab) / max(dot(ab, ab), 1e-3));
        const float3 p = a + ab * t;
        const float along = dot(p, dir);
        if (along > 0.0 && along < sceneDist) best = min(best, length(p - dir * along) / along);
        a = b;
    }
    const float width = 0.0009;
    return exp(-(best * best) / (width * width)) * (1.0 - age / 0.3) * 60.0;
}

[numthreads(8, 8, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_push.width || id.y >= g_push.height) return;
    const float rain = g_frame.weather.x;
    const bool bolt = g_frame.bolt[1].w >= 0.0 && g_frame.bolt[1].w < 0.3;
    if (rain <= 0.001 && !bolt) return;
    float4 c = g_rwTex2D[g_push.hdrUav][id.xy];
    const float depth = g_tex2D[g_push.depthSrv].Load(int3(id.xy, 0)).r;
    const float2 uv = (float2(id.xy) + 0.5) / float2(g_push.width, g_push.height);
    const float3 dir = viewRay(uv);
    const float sceneDist = depth > 0.0 ? g_frame.cameraUp.w / depth / max(dot(dir, g_frame.cameraForward.xyz), 1e-3) : 1e9;
    const bool magic = g_frame.flags.x != 0;
    if (rain > 0.001) {
        const float lum = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
        c.rgb = lerp(c.rgb, lum.xxx, 0.18 * rain) * (1.0 - 0.06 * rain);  // a little greyer under rain
        const float t = g_frame.fireInfo.x;
        const float2 wind = g_frame.fireInfo.yz;
        const float fall = 7.5;
        const float3 tint = magic ? K_MAGIC_COLOR_WATER * 0.25 : skyIrradiance(float3(0, 1, 0)) * 0.08 + 0.004;
        const float depths[4] = {2.0, 4.5, 9.0, 20.0};
        float streaks = 0;
        [unroll] for (uint k = 0; k < 4; ++k) {
            const float d = depths[k];
            if (sceneDist < d) continue;
            // On a cylinder of radius d round the camera: arc length across, height up (falling, leaning downwind).
            const float2 h = normalize(dir.xz + 1e-5);
            const float across = atan2(h.y, h.x) * d;
            const float up = dir.y / max(length(dir.xz), 1e-3) * d;
            const float lean = dot(wind, float2(-h.y, h.x)) / fall;
            const float2 q = float2(across + lean * up * 0.6, up + t * fall);
            const float cw = 0.05 + 0.025 * d, ch = 1.1;
            const int2 cell = int2(floor(q / float2(cw, ch)));
            if (cellHash(cell, k) > 0.25 + 0.55 * rain) continue;
            const float2 f = q / float2(cw, ch) - float2(cell);
            const float x = abs(f.x - (0.2 + 0.6 * cellHash(cell + 17, k))) * cw;
            const float y = frac(f.y + cellHash(cell + 3, k));
            const float width = 0.0012 * d;
            streaks += saturate(1.0 - x / width) * smoothstep(0.0, 0.2, y) * smoothstep(0.75, 0.35, y) / (1.0 + 0.15 * d);
        }
        c.rgb += tint * streaks * rain * 6.0;
    }
    if (bolt) c.rgb += float3(0.85, 0.82, 1.0) * boltGlow(dir, sceneDist);
    g_rwTex2D[g_push.hdrUav][id.xy] = c;
}
