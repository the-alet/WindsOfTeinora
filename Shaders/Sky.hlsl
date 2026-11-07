#include "Clouds.hlsli"
#include "Planet.hlsli"

// The star map has been drawn to Texture0 in this frame.

struct Varyings
{
	float4 Clip : SV_Position;
	float2 Screen : TEXCOORD0;
	float2 Uv : TEXCOORD1;
};

float4 PSMain(Varyings Input) : SV_Target
{
	const float4 Far = mul(InverseViewProjection, float4(Input.Screen, 0.5, 1.0));
	const float3 Direction = normalize(Far.xyz / Far.w);

	float3 Color = SkyColor(Direction);

	const float SunDot = dot(Direction, SunDirection.xyz);
	Color += SunColor.rgb * smoothstep(0.99985, 0.99995, SunDot) * 12.0;

	// The blue-green sun in its own colour. As bright as the first it would burn out to white, as any sun does
	// to an eye or a camera, and the two would look alike: a few times the sky around it keeps its hue.
	const float Sun2Dot = dot(Direction, Sun2Direction.xyz);
	const float3 Hue2 = pow(Sun2Color.rgb / max(max(Sun2Color.r, Sun2Color.g), max(Sun2Color.b, 1e-4)), 2.5); // deepened, or it reads as white
	Color += Hue2 * (dot(Color, float3(0.3, 0.6, 0.1)) * 4.0 + 0.4) * smoothstep(0.99992, 0.99997, Sun2Dot);

	if (GetNight() > 0.0)
	{
		Color += Texture0.Load(int3(Input.Clip.xy, 0)).rgb;
	}

	Color = AddPlanet(Color, Direction);

	// A different start in every pixel turns the banding of the steps into fine grain.
	const float Dither = frac(52.9829189 * frac(dot(Input.Clip.xy, float2(0.06711056, 0.00583715))));
	return float4(AddClouds(Color, Direction, 40, Dither), 1.0);
}
