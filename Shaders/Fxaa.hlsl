#include "Common.hlsli"

struct FinalConstants
{
	float2 SourceTexel;
	float FxaaEnabled;
	float Aspect;
	float2 FocusCenter;  // the point of gaze, in UV
	float FocusRadius;   // sharp area, as a share of the screen height
	float FocusStrength; // 0 turns the eye focus off
};

[[vk::push_constant]] ConstantBuffer<FinalConstants> Final : register(b2);

struct Varyings
{
	float4 Clip : SV_Position;
	float2 Screen : TEXCOORD0;
	float2 Uv : TEXCOORD1;
};

float Luma(float3 Color)
{
	return dot(Color, float3(0.299, 0.587, 0.114));
}

float3 Sample(float2 Uv)
{
	return Texture0.SampleLevel(LinearClamp, Uv, 0.0).rgb;
}

// The blurred view has a quarter of the resolution. Four spread samples hide its
// texels, which would otherwise show as steps around dark, high-contrast edges.
float3 SampleBlurred(float2 Uv)
{
	const float2 Spread = Final.SourceTexel * 3.0;
	const float3 Sum = Texture1.SampleLevel(LinearClamp, Uv + float2(Spread.x, Spread.y), 0.0).rgb
		+ Texture1.SampleLevel(LinearClamp, Uv + float2(-Spread.x, Spread.y), 0.0).rgb
		+ Texture1.SampleLevel(LinearClamp, Uv + float2(Spread.x, -Spread.y), 0.0).rgb
		+ Texture1.SampleLevel(LinearClamp, Uv + float2(-Spread.x, -Spread.y), 0.0).rgb;
	return Sum * 0.25;
}

float3 SmoothEdges(float2 Uv)
{
	const float3 Center = Sample(Uv);

	const float LumaM = Luma(Center);
	const float LumaNW = Luma(Sample(Uv + float2(-1.0, -1.0) * Final.SourceTexel));
	const float LumaNE = Luma(Sample(Uv + float2(1.0, -1.0) * Final.SourceTexel));
	const float LumaSW = Luma(Sample(Uv + float2(-1.0, 1.0) * Final.SourceTexel));
	const float LumaSE = Luma(Sample(Uv + float2(1.0, 1.0) * Final.SourceTexel));

	const float LumaMin = min(LumaM, min(min(LumaNW, LumaNE), min(LumaSW, LumaSE)));
	const float LumaMax = max(LumaM, max(max(LumaNW, LumaNE), max(LumaSW, LumaSE)));
	if (LumaMax - LumaMin < max(0.03, LumaMax * 0.1))
	{
		return Center;
	}

	float2 Direction = float2(-(LumaNW + LumaNE - LumaSW - LumaSE), LumaNW + LumaSW - LumaNE - LumaSE);
	const float Reduce = max((LumaNW + LumaNE + LumaSW + LumaSE) * 0.03125, 0.0078125);
	const float Scale = 1.0 / (min(abs(Direction.x), abs(Direction.y)) + Reduce);
	Direction = clamp(Direction * Scale, -8.0, 8.0) * Final.SourceTexel;

	const float3 Near = 0.5 * (Sample(Uv + Direction * (1.0 / 3.0 - 0.5)) + Sample(Uv + Direction * (2.0 / 3.0 - 0.5)));
	const float3 Wide = Near * 0.5 + 0.25 * (Sample(Uv - Direction * 0.5) + Sample(Uv + Direction * 0.5));

	const float LumaWide = Luma(Wide);
	return LumaWide < LumaMin || LumaWide > LumaMax ? Near : Wide;
}

// Writes the frame to the backbuffer: upscaling, smoothing edges when enabled,
// and keeping only the area around the point of gaze sharp.
float4 PSMain(Varyings Input) : SV_Target
{
	const float2 Uv = Input.Uv;

	float3 Color = Sample(Uv);
	if (Final.FxaaEnabled > 0.0)
	{
		Color = SmoothEdges(Uv);
	}

	if (Final.FocusStrength > 0.0)
	{
		const float Distance = length((Uv - Final.FocusCenter) * float2(Final.Aspect, 1.0));
		const float Blur = smoothstep(Final.FocusRadius, Final.FocusRadius * 2.6, Distance) * Final.FocusStrength;
		Color = lerp(Color, SampleBlurred(Uv), Blur);

		// The very edge of the blur also dims a little, by 5% at most.
		Color *= 1.0 - 0.05 * smoothstep(Final.FocusRadius * 2.2, Final.FocusRadius * 3.4, Distance);
	}

	return float4(Color, 1.0);
}
