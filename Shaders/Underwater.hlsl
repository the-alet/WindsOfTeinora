// What being in water does to the view: the water between the eye and whatever it looks at.
// The same file holds the plain copy of the scene that the water's surface looks through.
#include "Common.hlsli"

struct Varyings
{
	float4 Clip : SV_Position;
	float2 Screen : TEXCOORD0;
	float2 Uv : TEXCOORD1;
};

float4 CopyMain(Varyings Input) : SV_Target
{
	return Texture0.SampleLevel(PointClamp, Input.Uv, 0.0);
}

float4 PSMain(Varyings Input) : SV_Target
{
	// Reversed depth stores near / z, so the view depth comes straight back.
	const float Stored = Texture0.SampleLevel(PointClamp, Input.Uv, 0.0).r;
	const float4 Far = mul(InverseViewProjection, float4(Input.Screen, 0.5, 1.0));
	const float3 Ray = normalize(Far.xyz / Far.w);
	const float4 Forward = mul(InverseViewProjection, float4(0.0, 0.0, 0.5, 1.0));
	const float Along = saturate(dot(Ray, normalize(Forward.xyz / Forward.w)));
	float Distance = Stored > 0.0 ? Options.z / Stored / max(Along, 0.05) : 100000.0;

	// The way up ends at the surface, which has its own depth rather than one in the buffer.
	if (Ray.z > 0.001)
	{
		Distance = min(Distance, Conditions.x / Ray.z);
	}

	// Colour goes first, red soonest, and what remains is the blue-green haze the water scatters.
	const float3 Sunlight = SunColor.rgb * max(SunDirection.z, 0.0) + Sun2Color.rgb * max(Sun2Direction.z, 0.0);
	const float3 Ambient = SkyZenith.rgb + SkyHorizon.rgb * 0.3 + Sunlight * 0.25;
	const float Down = exp(-Conditions.x * 0.09);
	const float3 Haze = float3(0.03, 0.17, 0.19) * Ambient * Down;
	const float3 Passing = exp(-float3(0.32, 0.075, 0.055) * Distance);
	return float4(Haze, 1.0 - dot(Passing, float3(0.2, 0.5, 0.3)));
}
