// Final pass: blur outside the eye's focus (none inside a generous area, strong at the screen edge),
// 5% darkening at the very edge, tone curve, sRGB encode and dither into the output.
#include "common.hlsli"

struct PostPush {
    uint hdrSrv;
    uint mipCount;
    uint adaptSrv;  // the eye's state (adapt.hlsl): exposure, white balance
    float blurPower;
    float vignette;
    float hdrHeight;   // pixels of the HDR image (it may be rendered at a lower scale)
    float2 pad;
};
[[vk::push_constant]] ConstantBuffer<PostPush> g_push : register(b1, space0);

FullscreenOut VSMain(uint id : SV_VertexID) { return fullscreenTriangle(id); }

// Blur radius for a pixel, as a fraction of the screen height.
float blurRadius(float2 uv) {
    float aspect = g_frame.cameraRight.w;
    // Work in half-screen-height units with x scaled by the aspect, so the focus area is round.
    float2 P = float2(uv.x * aspect, uv.y) * 2.0;
    float2 G = float2(g_frame.gaze.x * aspect, g_frame.gaze.y) * 2.0;
    float2 d = P - G;
    float dist = length(d);
    float2 v = dist > 1e-5 ? d / dist : float2(1, 0);
    float2 hi = float2(aspect * 2.0, 2.0);
    // How far the screen edge is from the gaze along this direction: the blur peaks there.
    float tx = v.x > 1e-5 ? (hi.x - G.x) / v.x : (v.x < -1e-5 ? -G.x / v.x : 1e9);
    float ty = v.y > 1e-5 ? (hi.y - G.y) / v.y : (v.y < -1e-5 ? -G.y / v.y : 1e9);
    float edgeDist = max(min(tx, ty), 1e-3);
    float R = min(g_frame.gaze.z, edgeDist * 0.8);
    float t = saturate((dist - R) / max(edgeDist - R, 1e-3));
    return g_frame.gaze.w * pow(t, g_push.blurPower);
}

// Magic past the dew point bends light in plain sight: a shimmer where the view passes the campfire's dense core.
float2 dewShimmer(float2 uv) {
    const float s = g_frame.fire.w;
    const float over = K_MAGIC_FIRE_CORE * s / K_MAGIC_DEW_POINT;
    if (over <= 1.0) return 0;
    const float reach = K_MAGIC_FIRE_CORE_SIGMA * sqrt(2.0 * log(over)) * 1.6;
    const float3 dir = viewRay(uv);
    const float3 c = g_frame.fire.xyz + float3(0, 0.4 + 0.6 * s, 0);
    const float t = dot(c, dir);
    if (t <= 0.0) return 0;
    const float k = saturate(1.0 - length(c - dir * t) / reach);
    if (k <= 0.0) return 0;
    const float time = g_frame.fireInfo.x;
    return float2(sin(uv.y * 170.0 + time * 9.0), cos(uv.x * 150.0 + time * 7.0)) * k * 0.01 / max(t, 1.0);
}

// ... and wherever else magic is past the dew point (its box): the more of it the view crosses, the more it bends.
float2 denseShimmer(float2 uv) {
    if (g_frame.denseLo.w < 0.5) return 0;
    const float3 dir = viewRay(uv);
    const float3 inv = 1.0 / select(abs(dir) > 1e-6, dir, 1e-6);
    const float3 ta = g_frame.denseLo.xyz * inv, tb = g_frame.denseHi.xyz * inv;
    const float t0 = max(max(max(min(ta.x, tb.x), min(ta.y, tb.y)), min(ta.z, tb.z)), 0.0);
    const float t1 = min(min(min(max(ta.x, tb.x), max(ta.y, tb.y)), max(ta.z, tb.z)), 1e5);
    if (t1 <= t0) return 0;
    const float k = saturate((t1 - t0) / 6.0), time = g_frame.fireInfo.x;
    return float2(sin(uv.y * 120.0 + time * 4.0), cos(uv.x * 110.0 + time * 3.0)) * k * 0.004 / max(t0, 1.0);
}

float4 PSMain(FullscreenOut i) : SV_Target0 {
    Texture2D<float4> hdr = g_tex2D[g_push.hdrSrv];
    float2 uv = i.uv + (g_frame.flags.x == 0 ? dewShimmer(i.uv) + denseShimmer(i.uv) : 0.0);
    float aspect = g_frame.cameraRight.w;
    float radius = blurRadius(uv);
    float radiusPx = radius * g_push.hdrHeight;
    float3 c;
    if (radiusPx < 0.35) {
        c = hdr.SampleLevel(s_linearClamp, uv, 0).rgb;
    } else {
        // Vogel disc over the mip pyramid: the mip pre-blurs, the taps shape the disc, the rotation hides steps.
        float lod = clamp(log2(max(radiusPx / 2.5, 1.0)), 0.0, float(g_push.mipCount - 1));
        float rot = ign(i.pos.xy) * 6.2831853;
        float3 acc = 0;
        const int N = 12;
        [unroll] for (int k = 0; k < N; ++k) {
            float r = sqrt((k + 0.5) / N) * radius;
            float a = k * 2.39996323 + rot;
            float2 o = float2(cos(a) * r / aspect, sin(a) * r);
            acc += hdr.SampleLevel(s_linearClamp, uv + o, lod).rgb;
        }
        c = acc / N;
    }

    if (g_frame.flags.x != 0) {
        // The magic view: a soft halo round bright magic (the pyramid's blurred levels, past a threshold).
        const float far = float(g_push.mipCount - 1);
        const float3 halo = hdr.SampleLevel(s_linearClamp, uv, min(3.0, far)).rgb + hdr.SampleLevel(s_linearClamp, uv, min(5.0, far)).rgb;
        c += max(halo * 0.5 - 0.04, 0.0) * K_MAGIC_BLOOM;
    }

    float2 q = abs(uv * 2.0 - 1.0);
    float e = pow(pow(q.x, 6.0) + pow(q.y, 6.0), 1.0 / 6.0);
    c *= 1.0 - g_push.vignette * smoothstep(0.72, 1.0, e);

    float4 eye = asfloat(g_buffers[g_push.adaptSrv].Load4(0));
    if (g_frame.flags.x != 0) eye = float4(K_MAGIC_VIEW_EXPOSURE, 1, 1, 1);  // magic glows the same by day and night
    c = tonemapNeutral(max(c * eye.x * eye.yzw, 0));
    c = srgbEncode(c);
    c += (ign(i.pos.xy + float(g_frame.flags.y % 64u) * 5.588238) - 0.5) / 255.0;
    return float4(c, 1);
}
