#include "Common.hlsli"

struct TonemapConstants
{
	float4 Magic; // x how far the magic view has faded in, y 1 when dense magic bends light, zw a texel of its map
};

[[vk::push_constant]] ConstantBuffer<TonemapConstants> Push : register(b2);

// Texture1 holds the haze of the magic view, this one where magic bends light.
[[vk::binding(2, 1)]] Texture2D<float4> BendMap : register(t2);

struct Varyings
{
	float4 Clip : SV_Position;
	float2 Screen : TEXCOORD0;
	float2 Uv : TEXCOORD1;
};

static const float Bending = 0.5;   // how far light bends, in screen widths per unit of path difference
static const float MaxBend = 0.007; // and where that stops, so that swirls shimmer rather than tear

// Fitted ACES curve (Narkowicz).
float3 Aces(float3 Color)
{
	return saturate((Color * (2.51 * Color + 0.03)) / (Color * (2.43 * Color + 0.59) + 0.14));
}

float4 PSMain(Varyings Input) : SV_Target
{
	float2 Uv = Input.Uv;
	float Shimmer = 0.0;

	if (Push.Magic.y > 0.0)
	{
		// Light bends along the slope of the optical path, which is what makes air shimmer.
		const float2 Texel = Push.Magic.zw;
		const float Left = BendMap.SampleLevel(LinearClamp, Uv - float2(Texel.x, 0.0), 0.0).r;
		const float Right = BendMap.SampleLevel(LinearClamp, Uv + float2(Texel.x, 0.0), 0.0).r;
		const float Up = BendMap.SampleLevel(LinearClamp, Uv - float2(0.0, Texel.y), 0.0).r;
		const float Down = BendMap.SampleLevel(LinearClamp, Uv + float2(0.0, Texel.y), 0.0).r;

		const float2 Offset = float2(Right - Left, Down - Up) * Bending;
		const float Size = length(Offset);
		Uv = saturate(Uv + (Size > MaxBend ? Offset * (MaxBend / Size) : Offset));

		// Where the swirls gather light they look a little brighter, and darker where they spread it.
		const float2 Here = BendMap.SampleLevel(LinearClamp, Input.Uv, 0.0).rg;
		Shimmer = clamp(Here.r * 1.5, -0.14, 0.14) + 0.06 * saturate(Here.g);
	}

	float3 Color = Texture0.SampleLevel(LinearClamp, Uv, 0.0).rgb * Options.x * (1.0 + Shimmer);

	const float View = Push.Magic.x;
	if (View > 0.0)
	{
		// The world goes dim and pale, and the haze shows in front of it.
		const float4 Haze = Texture1.SampleLevel(LinearClamp, Input.Uv, 0.0);
		const float Grey = dot(Color, float3(0.2126, 0.7152, 0.0722));
		const float3 Dimmed = lerp(Color, Grey.xxx, 0.45) * 0.22;
		Color = lerp(Color, Dimmed * (1.0 - Haze.a) + Haze.rgb * 0.9, View);
	}

	return float4(pow(Aces(Color), 1.0 / 2.2), 1.0);
}
