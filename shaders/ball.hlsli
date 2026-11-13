// The thrown ball's look, shared by its G-buffer pass (ball.hlsl) and the water's reflections (water.hlsl):
// two panels, pale cream and deep rust, meeting along a three-lobed curved seam, so its spin shows. Matte.
#ifndef TAELIA_BALL_HLSLI
#define TAELIA_BALL_HLSLI

static const float BALL_ROUGHNESS = 0.6;
static const float BALL_SPECULAR = 0.5;

// Turns v by the unit quaternion q (x, y, z, w).
float3 rotateByQuat(float4 q, float3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

// Signed distance-like field over the unit sphere: > 0 on the cream panel, < 0 on the rust one; 0 on the seam.
// The seam winds around the equator in three lobes; the two panels are the same size.
float ballSeam(float3 n) {
    const float r = sqrt(saturate(1.0 - n.y * n.y));
    return n.y - 0.55 * r * sin(3.0 * atan2(n.z, n.x));
}

// Albedo (linear) at the local direction n; aa: the seam field's change across a pixel. w.: 0..1 on the seam groove.
float4 ballAlbedo(float3 n, float aa) {
    const float w = ballSeam(n);
    const float3 cream = float3(0.58, 0.50, 0.36), rust = float3(0.27, 0.062, 0.028), seam = float3(0.09, 0.05, 0.035);
    aa = max(aa, 1e-3);
    float3 c = lerp(rust, cream, smoothstep(-aa, aa, w));
    const float groove = 1.0 - smoothstep(0.02, 0.02 + aa * 1.5, abs(w));
    c = lerp(c, seam, groove);
    return float4(c, groove);
}

#endif
