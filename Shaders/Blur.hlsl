#include "Common.hlsli"

struct BlurConstants
{
	float2 Step; // one source texel along the blur direction
	float2 Padding;
};

[[vk::push_constant]] ConstantBuffer<BlurConstants> Blur : register(b2);

struct Varyings
{
	float4 Clip : SV_Position;
	float2 Screen : TEXCOORD0;
	float2 Uv : TEXCOORD1;
};

float3 Sample(float2 Uv)
{
	return Texture0.SampleLevel(LinearClamp, Uv, 0.0).rgb;
}

// A nine-tap Gaussian in five samples: bilinear filtering merges each outer pair.
float4 PSMain(Varyings Input) : SV_Target
{
	const float2 Near = Blur.Step * 1.3846154;
	const float2 Far = Blur.Step * 3.2307692;

	float3 Color = Sample(Input.Uv) * 0.2270270;
	Color += (Sample(Input.Uv + Near) + Sample(Input.Uv - Near)) * 0.3162162;
	Color += (Sample(Input.Uv + Far) + Sample(Input.Uv - Far)) * 0.0702703;
	return float4(Color, 1.0);
}
