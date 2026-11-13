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

float4 PSMain(FullscreenOut i) : SV_Target0 {
    Texture2D<float4> hdr = g_tex2D[g_push.hdrSrv];
    float2 uv = i.uv;
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

    float2 q = abs(uv * 2.0 - 1.0);
    float e = pow(pow(q.x, 6.0) + pow(q.y, 6.0), 1.0 / 6.0);
    c *= 1.0 - g_push.vignette * smoothstep(0.72, 1.0, e);

    const float4 eye = asfloat(g_buffers[g_push.adaptSrv].Load4(0));
    c = tonemapNeutral(max(c * eye.x * eye.yzw, 0));
    c = srgbEncode(c);
    c += (ign(i.pos.xy + float(g_frame.flags.y % 64u) * 5.588238) - 0.5) / 255.0;
    return float4(c, 1);
}
