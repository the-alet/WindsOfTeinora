#include "Common.hlsli"

struct TonemapConstants
{
	float4 Magic; // x how far the magic view has faded in, y 1 when dense magic bends light, zw a texel of its map
};

[[vk::push_constant]] ConstantBuffer<TonemapConstants> Push : register(b2);

// Texture1 holds the haze of the magic view, this one where magic bends light.
[[vk::binding(2, 1)]] Texture2D<float4> BendMap : register(t2);
[[vk::binding(3, 1)]] Texture2D<float> RainDepth : register(t3); // the scene's depth, for what the rain falls in front of

// Rain, as sheets of drops at a few distances in front of the eye, each sheet measured in metres where it is.
static const int RainLayers = 8;
static const float RainNearest = 0.9;     // metres to the nearest sheet
static const float RainSpread = 1.5;      // each further than the one before by this much, out to about 15 m
static const float RainFall = 9.0;        // m/s, how fast a drop falls
static const float RainShutter = 0.025;   // seconds an eye blurs a drop over, which draws it out into a streak
static const float DropWidth = 0.0025;    // metres
static const float DropColumns = 0.07;    // metres between drops across a sheet
static const float DropRows = 1.1;        // and up and down it, which leaves room for a streak in each
static const float ViewTall = 1.2256;     // metres the view spans top to bottom 1 m off: 2 tan(31.5°), Gaze::VerticalFov

uint RainHash(int3 Cell)
{
	uint Mixed = uint(Cell.x) * 1597334677u ^ uint(Cell.y) * 3812015801u ^ uint(Cell.z) * 2246822519u;
	Mixed ^= Mixed >> 15;
	Mixed *= 2654435769u;
	Mixed ^= Mixed >> 13;
	return Mixed;
}

float RainShare(uint Bits)
{
	return float(Bits & 65535u) / 65535.0;
}

/**
 * How much of a pixel one sheet of rain covers, from 0 to 1. Each drop is a thin streak as long as it falls
 * while the eye takes it in, brightest at its lower end where the drop is now. Near the eye the drops are out
 * of focus, wider and fainter; far off they are thinner than a pixel, and fainter for it.
 */
float GetRain(float2 Uv, int Layer, float Amount)
{
	const float Distance = RainNearest * pow(RainSpread, float(Layer));
	const float Tall = ViewTall * Distance;
	const float Pixel = Tall * ScreenSize.w; // metres a pixel of the scene spans there
	const float Aspect = ScreenSize.x * ScreenSize.w;

	// Where this pixel is on the sheet, in metres, slanted by the wind and moving down as the rain falls.
	float2 Place = float2((Uv.x - 0.5) * Aspect, Uv.y - 0.5) * Tall;
	Place.x -= Place.y * Weather.z;
	Place.y -= CameraPosition.w * RainFall;

	const float Column = floor(Place.x / DropColumns);
	const uint Shift = RainHash(int3(int(Column), Layer, 7));
	const float Down = Place.y / DropRows + RainShare(Shift) * 7.0;
	const uint Drop = RainHash(int3(int(Column), int(floor(Down)), Layer));
	if (RainShare(Drop) > Amount * (0.7 - 0.05 * float(Layer)))
	{
		return 0.0;
	}

	// Its streak, somewhere in the cell, a little longer or shorter for a bigger or smaller drop.
	const uint Look = RainHash(int3(int(Drop), Layer, 11));
	const float Length = RainFall * RainShutter * (0.8 + 0.5 * RainShare(Look));
	const float Middle = (0.2 + 0.6 * RainShare(Look >> 16)) * DropColumns;
	const float Top = RainShare(Drop >> 16) * (DropRows - Length);
	const float Along = (frac(Down) * DropRows - Top) / Length; // 0 at the top of the streak, 1 at the drop
	if (Along < 0.0 || Along > 1.0)
	{
		return 0.0;
	}
	const float Blur = DropWidth * 2.5 / Distance;           // out of focus near the eye
	const float Width = max(DropWidth + Blur, Pixel * 1.3);
	const float Across = (frac(Place.x / DropColumns) * DropColumns - Middle) / Width;
	const float Strength = DropWidth * 1.6 / Width;           // the same light spread wider is fainter
	return exp(-Across * Across * 4.0) * smoothstep(0.0, 0.5, Along) * smoothstep(0.0, 0.06, 1.0 - Along) * saturate(Strength);
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

	// Rain, in front of whatever is further than it. Under it everything is a little greyer and darker.
	const float Rain = Weather.x;
	if (Rain > 0.001)
	{
		const float Depth = RainDepth.SampleLevel(PointClamp, Input.Uv, 0.0).r;
		const float Scene = Depth > 0.0 ? Options.z / Depth : 1e9;
		const float Grey = dot(Color, float3(0.2126, 0.7152, 0.0722));
		Color = lerp(Color, Grey.xxx, 0.25 * Rain) * (1.0 - 0.2 * Rain);

		float Cover = 0.0;
		for (int Layer = 0; Layer < RainLayers; ++Layer)
		{
			const float Distance = RainNearest * pow(RainSpread, float(Layer));
			const float InFront = saturate((Scene - Distance) / (0.25 * Distance));
			if (InFront > 0.0)
			{
				Cover = 1.0 - (1.0 - Cover) * (1.0 - GetRain(Input.Uv, Layer, Rain) * InFront);
			}
		}
		if (Cover > 0.002)
		{
			// A drop is a little lens: it shows the light from a wide piece of the sky over it, and a bent glimpse of
			// what is behind, so it is lighter than a dark ground and hardly shows against the sky.
			const float3 Sky = (SkyZenith.rgb * 0.55 + SkyHorizon.rgb * 0.45 + PlanetColor.rgb * 0.15) * Options.x;
			const float3 Bent = Texture0.SampleLevel(LinearClamp, Input.Uv + float2(0.0, -0.012), 0.0).rgb * Options.x;
			Color = lerp(Color, Sky * 1.1 + Bent * 0.35, Cover * 0.55);
		}
	}
	float3 Shown = Aces(Color);

	const float View = Push.Magic.x;
	if (View > 0.0)
	{
		// The world goes grey and dim, and the magic glows in it, through it, by its own light: it lights nothing,
		// so the grey is only a little hidden behind it. This comes after the curve: before it, fire and all it
		// lights are so bright that dimming them only takes them down to white.
		const float4 Haze = Texture1.SampleLevel(LinearClamp, Input.Uv, 0.0);
		const float Grey = dot(Shown, float3(0.2126, 0.7152, 0.0722)) * 0.3;
		Shown = lerp(Shown, saturate(Grey.xxx * (1.0 - 0.5 * Haze.a) + Aces(Haze.rgb * 0.9)), View);
	}

	return float4(pow(Shown, 1.0 / 2.2), 1.0);
}
