// Puffs of steam off boiling water and of smoke over fires, one sheet to each, turned to face the eye
// by the renderer. The puff itself is in the pixels: a round shape that noise tears at, lit as clouds
// are, and fading out where it meets what is behind it.
#include "Common.hlsli"

static const float SoftEdge = 3.0; // metres

struct Varyings
{
	float4 Clip : SV_Position;
	float3 Relative : TEXCOORD0;
	float2 Place : TEXCOORD1; // from -1 to 1 across the sheet
	float4 Look : TEXCOORD2;  // x how thick it is, y a number of its own, z how much light it gives back, w how much firelight reaches it
};

static const float3 FlameLight = float3(2.6, 1.1, 0.3); // FlameLightRed and so on in Engine/Render/SceneTypes.h

Varyings VSMain(float3 Position : TEXCOORD0, float2 Place : TEXCOORD1, float4 Look : TEXCOORD2)
{
	Varyings Output;
	Output.Clip = mul(ViewProjection, float4(Position, 1.0));
	Output.Relative = Position;
	Output.Place = Place;
	Output.Look = Look;
	return Output;
}

// In whole numbers: over a shape this smooth, a hash of sines rounds differently on each side of a cell and shows the seam.
float GetSpeck(float2 Cell)
{
	const int2 Whole = int2(Cell);
	uint Mixed = uint(Whole.x) * 1597334677u + uint(Whole.y) * 3812015801u;
	Mixed = (Mixed ^ (Mixed >> 16)) * 2654435769u;
	return float(Mixed >> 8) / 16777216.0;
}

float GetNoise(float2 Place)
{
	const float2 Cell = floor(Place);
	const float2 Part = Place - Cell;
	const float2 Blend = Part * Part * (3.0 - 2.0 * Part);
	const float Low = lerp(GetSpeck(Cell), GetSpeck(Cell + float2(1.0, 0.0)), Blend.x);
	const float High = lerp(GetSpeck(Cell + float2(0.0, 1.0)), GetSpeck(Cell + float2(1.0, 1.0)), Blend.x);
	return lerp(Low, High, Blend.y);
}

float4 PSMain(Varyings Input) : SV_Target
{
	const float Rise = frac(CameraPosition.w * 0.004) * 60.0; // it starts over every few minutes, as flames do
	const float2 Place = Input.Place + Input.Look.y;
	const float2 Moving = float2(Place.x, Place.y - Rise);
	const float2 Torn = float2(GetNoise(Moving * 1.1 + 13.0), GetNoise(Moving * 1.1 + 29.0)) - 0.5;
	const float Billows = GetNoise(Moving * 1.6) * 0.5 + GetNoise(Moving * 3.4 + 7.0) * 0.27 + GetNoise(Moving * 7.1 + 3.0) * 0.15 + GetNoise(Moving * 14.3 + 1.0) * 0.08;

	// The thinner it has grown, the more the air has torn at it, into wisps with holes between them.
	const float Thin = 1.0 - saturate(Input.Look.x);
	const float Round = 1.0 - length(Input.Place + Torn * (0.9 + 0.5 * Thin));
	const float Sheet = saturate((1.0 - length(Input.Place)) * 4.0); // nothing reaches the edges of the sheet
	const float Body = smoothstep(0.0, 0.3, Round * 1.4 - (1.0 - Billows) * (0.75 + 0.45 * Thin)) * Sheet * (0.6 + 0.4 * Billows);

	const float Scene = Options.z / max(Texture0.SampleLevel(PointClamp, Input.Clip.xy * ScreenSize.zw, 0.0).r, 1e-7);
	const float Own = Options.z / max(Input.Clip.z, 1e-7);
	const float Soft = saturate((Scene - Own) / SoftEdge);

	// Brighter on top, where the sky and the suns reach it, and underneath where a fire does.
	const float Top = Input.Place.y * 0.5 + 0.5;
	const float3 Ambient = SkyZenith.rgb * 0.55 + SkyHorizon.rgb * 0.45 + PlanetColor.rgb * 0.25;
	const float3 Sunlight = SunColor.rgb * max(SunDirection.z, 0.0) + Sun2Color.rgb * max(Sun2Direction.z, 0.0);
	const float3 Above = (Ambient * 1.1 + Sunlight * 0.3) * (0.55 + 0.3 * Top + 0.5 * Billows);
	const float3 Below = FlameLight * Input.Look.w * (0.5 - 0.25 * Top);
	const float3 Color = (Above + Below) * Input.Look.z;

	return float4(ApplyFog(Color, Input.Relative), Body * Input.Look.x * Soft);
}
