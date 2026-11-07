// Flames over what burns, one sheet to each, standing up and turned to face the eye. The fire itself
// is in the pixels, see Flame.hlsli: noise that climbs, leans the flame over and eats at its edges.
// It adds to what is behind it, as light does.
#include "Common.hlsli"
#include "Flame.hlsli"

struct Varyings
{
	float4 Clip : SV_Position;
	float2 Place : TEXCOORD0; // x from -1 to 1 across the sheet, y from 0 at its foot to 1 at the top
	float Seed : TEXCOORD1;   // so that no two fires move alike
};

Varyings VSMain(uint Vertex : SV_VertexID, uint Instance : SV_InstanceID)
{
	static const float2 Corners[6] = {
		float2(-1.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
		float2(-1.0, 0.0), float2(1.0, 1.0), float2(-1.0, 1.0) };
	const float2 Corner = Corners[Vertex];
	const float3 Point = HotPoints[Instance].xyz;
	const float Height = HotPoints[Instance].w;

	Varyings Output;
	Output.Place = Corner;
	Output.Seed = Instance * 37.0;
	Output.Clip = float4(2.0, 2.0, 0.0, 1.0); // off the screen, for what only glows

	if (Height > 0.0)
	{
		const float2 Facing = normalize(Point.xy);
		const float3 Side = float3(-Facing.y, Facing.x, 0.0);
		const float3 Foot = Point - float3(0.0, 0.0, Height * 0.33);
		Output.Clip = mul(ViewProjection, float4(Foot + Side * (Corner.x * Height * 0.6) + float3(0.0, 0.0, Corner.y * Height), 1.0));
	}
	return Output;
}

float4 PSMain(Varyings Input) : SV_Target
{
	return float4(GetFlameColor(GetFlameHeat(Input.Place, Input.Seed, CameraPosition.w)), 0.0);
}
