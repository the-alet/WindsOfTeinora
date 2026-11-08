// A flame as a volume: rising, swirling noise inside a cylinder around the fire, as hot as it is thick,
// and nothing but light. Shaders/Flames.hlsl draws it for the eye and Water.hlsl for what the water
// mirrors. The same is in FLAME_BODY in Unreal/Scripts/make_content.py.
#ifndef TEINORA_FLAME_HLSLI
#define TEINORA_FLAME_HLSLI

static const float FlameLight = 6.0;
static const float FlameReach = 1.15;  // how far above its height a tongue may lick

/** The radius of the cylinder around a flame of this breadth, as a share of its height. */
float GetFlameWidth(float Breadth)
{
	return Breadth * 1.4;
}
static const int FlameSteps = 20;

// In whole numbers: a hash of sines rounds differently on each side of a cell and can show the seam.
float GetFlameSpeck(int3 Cell)
{
	uint Mixed = uint(Cell.x) * 1597334677u ^ uint(Cell.y) * 3812015801u ^ uint(Cell.z) * 2798796415u;
	Mixed = (Mixed ^ (Mixed >> 16)) * 2654435769u;
	Mixed ^= Mixed >> 15;
	return float(Mixed >> 8) / 16777216.0;
}

float GetFlameNoise(float3 Place)
{
	const float3 Floor = floor(Place);
	const int3 Cell = int3(Floor);
	const float3 Part = Place - Floor;
	const float3 Blend = Part * Part * (3.0 - 2.0 * Part);
	const float Low = lerp(lerp(GetFlameSpeck(Cell), GetFlameSpeck(Cell + int3(1, 0, 0)), Blend.x),
		lerp(GetFlameSpeck(Cell + int3(0, 1, 0)), GetFlameSpeck(Cell + int3(1, 1, 0)), Blend.x), Blend.y);
	const float High = lerp(lerp(GetFlameSpeck(Cell + int3(0, 0, 1)), GetFlameSpeck(Cell + int3(1, 0, 1)), Blend.x),
		lerp(GetFlameSpeck(Cell + int3(0, 1, 1)), GetFlameSpeck(Cell + int3(1, 1, 1)), Blend.x), Blend.y);
	return lerp(Low, High, Blend.z);
}

/** The colour of burning gas this hot, from 0 to 1: deep red at the edges, orange, then yellow going on white. */
float3 GetFlameColor(float Heat)
{
	float3 Color = lerp(float3(0.9, 0.08, 0.01), float3(1.0, 0.36, 0.05), smoothstep(0.05, 0.35, Heat));
	Color = lerp(Color, float3(1.0, 0.72, 0.28), smoothstep(0.35, 0.7, Heat));
	return lerp(Color, float3(1.0, 0.93, 0.78), smoothstep(0.7, 1.0, Heat));
}

/**
 * The light a flame Height metres tall and Breadth of that across at its foot gives along a ray, and
 * in w how much of what lies behind it it covers. Origin is where the ray starts, relative to the
 * flame's foot, in metres with z up; it goes no further than Reach. Jitter, from 0 to 1, moves the
 * steps within a pixel.
 */
float4 MarchFlame(float3 Origin, float3 Direction, float Reach, float Height, float Breadth, float Seed, float Time, float Jitter)
{
	const float Radius = Height * GetFlameWidth(Breadth);
	const float Top = Height * FlameReach;

	// Where the ray is inside the cylinder, and between the ground and the top.
	float Enter = 0.0;
	float Leave = Reach;
	const float Level = dot(Direction.xy, Direction.xy);
	const float Toward = dot(Origin.xy, Direction.xy);
	const float Outside = dot(Origin.xy, Origin.xy) - Radius * Radius;
	if (Level > 1e-6)
	{
		const float Square = Toward * Toward - Level * Outside;
		if (Square <= 0.0)
		{
			return float4(0.0, 0.0, 0.0, 0.0);
		}
		const float Root = sqrt(Square);
		Enter = max(Enter, (-Toward - Root) / Level);
		Leave = min(Leave, (-Toward + Root) / Level);
	}
	else if (Outside > 0.0)
	{
		return float4(0.0, 0.0, 0.0, 0.0);
	}
	if (abs(Direction.z) > 1e-6)
	{
		const float Floor = -Origin.z / Direction.z;
		const float Ceiling = (Top - Origin.z) / Direction.z;
		Enter = max(Enter, min(Floor, Ceiling));
		Leave = min(Leave, max(Floor, Ceiling));
	}
	else if (Origin.z < 0.0 || Origin.z > Top)
	{
		return float4(0.0, 0.0, 0.0, 0.0);
	}
	if (Leave <= Enter)
	{
		return float4(0.0, 0.0, 0.0, 0.0);
	}

	// Kept small, which a flickering flame hides when it starts over every ten minutes.
	const float Clock = fmod(Time, 600.0);
	const float Stride = (Leave - Enter) / FlameSteps;
	float3 Light = float3(0.0, 0.0, 0.0);
	float Thick = 0.0;
	for (int Step = 0; Step < FlameSteps; ++Step)
	{
		// In the flame's own size: across from -0.5 to 0.5, up from 0 to its height.
		const float3 Here = (Origin + Direction * (Enter + (Step + Jitter) * Stride)) / Height;
		const float Up = Here.z;

		// Gas that rises faster than it swirls, the swirls larger higher up, and the whole flame swaying.
		const float3 Flowing = float3(Here.xy * 3.0, Up * 2.2 - Clock * 2.4) + Seed;
		const float2 Sway = (float2(GetFlameNoise(Flowing * 0.45 + 17.0), GetFlameNoise(Flowing * 0.45 + 31.0)) - 0.5) * Up * 0.55;
		const float Churn = GetFlameNoise(Flowing) * 0.5 + GetFlameNoise(Flowing * 2.3 + 5.0) * 0.3 + GetFlameNoise(Flowing * 5.1 + 9.0) * 0.2;

		// Wide at the foot and narrowing to tongues, which the churning tears at more the higher they go.
		const float Width = Breadth * pow(saturate(1.0 - Up / FlameReach), 0.75) * smoothstep(-0.02, 0.06, Up);
		const float Inside = 1.0 - length(Here.xy - Sway) / max(Width, 1e-3);
		const float Body = saturate(Inside * 1.4 + (Churn - 0.5) * (0.9 + 1.6 * Up));
		const float Heat = saturate(Body * (1.25 - 0.8 * Up) * (0.65 + 0.7 * Churn));

		const float Span = Stride / Height;
		Light += GetFlameColor(Heat) * (Heat * Heat * Span * 4.0);
		Thick += Body * Span;
	}
	return float4(Light * FlameLight, 1.0 - exp(-Thick * 4.0));
}

#endif
