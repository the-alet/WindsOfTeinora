#include "Common.hlsli"

struct TonemapConstants
{
	float4 Magic; // x how far the magic view has faded in, y 1 when dense magic bends light, zw a texel of its map
};

[[vk::push_constant]] ConstantBuffer<TonemapConstants> Push : register(b2);

// Texture1 holds the haze of the magic view, this one where magic bends light.
[[vk::binding(2, 1)]] Texture2D<float4> BendMap : register(t2);
[[vk::binding(3, 1)]] Texture2D<float> RainDepth : register(t3); // the scene's depth, for what the rain falls in front of

static const int RainLayers = 5;
static const float RainDistances[RainLayers] = { 1.3, 3.0, 6.5, 14.0, 30.0 }; // metres from the eye
static const float RainFall = 9.0; // m/s

uint RainHash(int3 Cell)
{
	uint Mixed = uint(Cell.x) * 1597334677u ^ uint(Cell.y) * 3812015801u ^ uint(Cell.z) * 2246822519u;
	Mixed ^= Mixed >> 15;
	Mixed *= 2654435769u;
	Mixed ^= Mixed >> 13;
	return Mixed;
}

/**
 * Streaks of rain at one distance in front of the eye, from 0 to 1: drops falling at RainFall, blurred into
 * streaks over the moment a camera or an eye takes in, blown aslant by the wind. The nearer, the bigger and
 * the faster across the view, and the fewer.
 */
float GetRain(float2 Uv, int Layer, float Amount)
{
	const float Distance = RainDistances[Layer];
	const float Aspect = ScreenSize.x * ScreenSize.w;
	const float Across = 1.2 * Distance; // metres the view takes in from side to side there, about
	const float Columns = clamp(Across / 0.09, 24.0, 420.0);
	const float Rows = Columns * 0.35;
	const float Falling = RainFall / Across * Aspect; // screen heights a second

	float2 Place = float2(Uv.x * Aspect, Uv.y);
	Place.x += Place.y * Weather.z;
	const float Column = floor(Place.x * Columns / Aspect);
	const uint Own = RainHash(int3(int(Column), Layer, 7));
	const float Down = Place.y * Rows - CameraPosition.w * Falling * Rows + float(Own & 1023u) / 1023.0 * 17.0;
	const float Row = floor(Down);
	const uint Drop = RainHash(int3(int(Column), int(Row), Layer));
	if (float(Drop & 65535u) / 65535.0 > Amount * 0.55)
	{
		return 0.0;
	}
	const float Where = 0.2 + 0.6 * float((Drop >> 16) & 255u) / 255.0;
	const float Side = abs(frac(Place.x * Columns / Aspect) - Where);
	const float Along = frac(Down);
	const float Length = 0.55;
	return (1.0 - smoothstep(0.03, 0.09, Side)) * (Along < Length ? 1.0 - Along / Length : 0.0);
}

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

	// Rain, in front of whatever is further than it, lit by the sky. Under it everything is a little greyer and darker.
	const float Rain = Weather.x;
	if (Rain > 0.001)
	{
		const float Depth = RainDepth.SampleLevel(PointClamp, Input.Uv, 0.0).r;
		const float Scene = Depth > 0.0 ? Options.z / Depth : 1e9;
		const float3 Lit = (SkyZenith.rgb * 0.6 + SkyHorizon.rgb * 0.4 + PlanetColor.rgb * 0.1) * Options.x * 1.4;
		const float Grey = dot(Color, float3(0.2126, 0.7152, 0.0722));
		Color = lerp(Color, Grey.xxx, 0.25 * Rain) * (1.0 - 0.2 * Rain);
		for (int Layer = 0; Layer < RainLayers; ++Layer)
		{
			if (Scene > RainDistances[Layer])
			{
				const float Streak = GetRain(Input.Uv, Layer, Rain) * (0.45 - 0.06 * Layer);
				Color = lerp(Color, Lit, Streak);
			}
		}
	}
	float3 Shown = Aces(Color);

	const float View = Push.Magic.x;
	if (View > 0.0)
	{
		// The world goes dim and pale, and the haze shows in front of it. This comes after the curve: before
		// it, fire and all it lights are so bright that dimming them only takes them down to white.
		const float4 Haze = Texture1.SampleLevel(LinearClamp, Input.Uv, 0.0);
		const float Grey = dot(Shown, float3(0.2126, 0.7152, 0.0722));
		const float3 Dimmed = lerp(Shown, Grey.xxx, 0.45) * 0.2;
		Shown = lerp(Shown, saturate(Dimmed * (1.0 - Haze.a) + Aces(Haze.rgb * 0.9)), View);
	}

	return float4(pow(Shown, 1.0 / 2.2), 1.0);
}
