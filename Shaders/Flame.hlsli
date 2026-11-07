// The look of a flame on its sheet, for Flames.hlsl and for the water that mirrors it. The same is
// in FLAME_BODY in Unreal/Scripts/make_content.py.
#ifndef TEINORA_FLAME_HLSLI
#define TEINORA_FLAME_HLSLI

static const float FlameLight = 6.0;

// In whole numbers: a hash of sines rounds differently on each side of a cell and can show the seam.
float GetFlameSpeck(float2 Cell)
{
	const int2 Whole = int2(Cell);
	uint Mixed = uint(Whole.x) * 1597334677u + uint(Whole.y) * 3812015801u;
	Mixed = (Mixed ^ (Mixed >> 16)) * 2654435769u;
	return float(Mixed >> 8) / 16777216.0;
}

float GetFlameNoise(float2 Place)
{
	const float2 Cell = floor(Place);
	const float2 Part = Place - Cell;
	const float2 Blend = Part * Part * (3.0 - 2.0 * Part);
	const float Low = lerp(GetFlameSpeck(Cell), GetFlameSpeck(Cell + float2(1.0, 0.0)), Blend.x);
	const float High = lerp(GetFlameSpeck(Cell + float2(0.0, 1.0)), GetFlameSpeck(Cell + float2(1.0, 1.0)), Blend.x);
	return lerp(Low, High, Blend.y);
}

/** How hot a flame is at a place on its sheet: x from -1 to 1 across it, y from 0 at its foot to 1 at the top. */
float GetFlameHeat(float2 Place, float Seed, float Time)
{
	// It starts over every few minutes, which a flickering flame hides, and keeps the numbers small.
	const float Rise = frac(Time * 0.004) * 400.0;
	const float Up = Place.y;

	const float2 Climbing = float2(Place.x * 1.5 + Seed, Up * 1.9 - Rise);
	const float Lean = GetFlameNoise(Climbing) - 0.5;
	const float Tongues = GetFlameNoise(Climbing * 2.7 + 11.0) * 0.6 + GetFlameNoise(Climbing * 5.3 + 5.0) * 0.4;

	// Wide at the foot and pointed at the top, leaning more the higher it gets.
	const float Width = 0.8 * sqrt(max(1.0 - Up, 0.0)) * smoothstep(-0.05, 0.1, Up);
	const float Inside = saturate(1.0 - abs(Place.x + Lean * Up * 1.3) / max(Width, 0.001));
	return saturate((Inside * (1.3 - Up * 0.7) - (1.0 - Tongues) * (0.25 + 0.8 * Up)) * 1.5);
}

/** The light a flame gives off where it is this hot. */
float3 GetFlameColor(float Heat)
{
	float3 Color = lerp(float3(1.0, 0.13, 0.015), float3(1.0, 0.46, 0.07), smoothstep(0.08, 0.45, Heat));
	Color = lerp(Color, float3(1.0, 0.84, 0.45), smoothstep(0.5, 0.95, Heat));
	return Color * Heat * Heat * FlameLight;
}

#endif
