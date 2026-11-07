// Lightning: ribbons along a bolt's channel, turned to face the eye by the renderer. It adds to what
// is behind it. A white core, in a violet glow, the colour of lightning magic.
#include "Common.hlsli"

static const float BoltLight = 60.0;

struct Varyings
{
	float4 Clip : SV_Position;
	float2 Look : TEXCOORD0; // x from -1 to 1 across the ribbon, y how bright it is here
};

Varyings VSMain(float3 Position : TEXCOORD0, float2 Look : TEXCOORD1)
{
	Varyings Output;
	Output.Clip = mul(ViewProjection, float4(Position, 1.0));
	Output.Look = Look;
	return Output;
}

float4 PSMain(Varyings Input) : SV_Target
{
	const float Square = Input.Look.x * Input.Look.x;
	const float3 Color = float3(1.0, 0.97, 1.0) * exp(-Square * 14.0) + float3(0.42, 0.34, 1.0) * exp(-Square * 3.0) * 0.16;
	return float4(Color * Input.Look.y * BoltLight, 0.0);
}
