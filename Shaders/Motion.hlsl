#include "Common.hlsli"

struct Varyings
{
	float4 Clip : SV_Position;
	float2 Screen : TEXCOORD0;
	float2 Uv : TEXCOORD1;
};

// Motion from the camera alone: each pixel's depth is reprojected into the previous
// frame through PassViewProjection, which holds the clip-to-previous-clip matrix.
float4 PSMain(Varyings Input) : SV_Target
{
	const float Depth = Texture0.SampleLevel(PointClamp, Input.Uv, 0.0).r;
	const float4 Previous = mul(PassViewProjection, float4(Input.Screen, Depth, 1.0));
	const float2 Ndc = Previous.xy / Previous.w;
	const float2 PreviousUv = float2(Ndc.x * 0.5 + 0.5, 0.5 - Ndc.y * 0.5);
	return float4(PreviousUv - Input.Uv, 0.0, 0.0);
}
