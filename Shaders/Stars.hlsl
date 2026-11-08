// Draws the star map into a target of its own, which the sky then shows through itself. Each star
// is a tiny spot on screen: a point of light smeared by the optics, about a pixel across.
#include "Common.hlsli"

// A star of flux 1 holds this much brightness over a steradian. It is the faintest in the map.
static const float StarLight = 3.5e-8;
static const float Sigma = 0.72; // pixels

struct Varyings
{
	float4 Clip : SV_Position;
	float2 Spot : TEXCOORD0;   // pixels from the middle of the star
	float3 Light : TEXCOORD1;
	float Reach : TEXCOORD2;   // pixels to where it has faded out
};

Varyings VSMain(float4 Star : TEXCOORD0, float4 Look : TEXCOORD1, uint Vertex : SV_VertexID)
{
	static const float2 Corners[6] = {
		float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
		float2(-1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0) };
	const float2 Corner = Corners[Vertex];

	// The whole sphere turns with the hours.
	const float3 Pole = StarPole.xyz;
	const float Cosine = cos(StarPole.w);
	const float Sine = sin(StarPole.w);
	const float3 FromMiddle = Star.xyz * Cosine + cross(Pole, Star.xyz) * Sine + Pole * dot(Pole, Star.xyz) * (1.0 - Cosine);
	const float3 Direction = normalize(FromMiddle + SkyShift.xyz); // seen from the camera, not from the sky's middle

	Varyings Output;
	Output.Spot = Corner;
	Output.Light = float3(0.0, 0.0, 0.0);
	Output.Reach = 1.0;
	Output.Clip = float4(2.0, 2.0, 0.0, 1.0); // off the screen, for the stars there is no need of

	const float4 Clip = mul(ViewProjection, float4(Direction, 0.0));
	if (Clip.w <= 0.0 || Direction.z <= 0.0)
	{
		return Output;
	}

	// The same light in a smaller pixel of sky shows brighter, so a star looks alike at any resolution.
	const float PixelAngle = 2.0 / (length(ViewProjection[1].xyz) * ScreenSize.y);
	const float Area = Sigma * PixelAngle * Sigma * PixelAngle;

	// The air flickers them, more so the lower they stand.
	const float Low = 1.0 - saturate(Direction.z);
	const float Phase = Look.a * 6.2831853;
	const float Rate = 5.0 + 6.0 * frac(Look.a * 17.0);
	const float Time = CameraPosition.w;
	const float Flicker = 1.0 + (0.1 + 0.5 * Low * Low * Low) * sin(Time * Rate + Phase) * sin(Time * Rate * 0.37 + Phase * 2.0);

	// And dim them near the horizon, blue going first through the long path of air.
	const float Air = GetAirMass(Direction.z) - 1.0;
	const float Peak = Star.w * StarLight / Area * Flicker * GetNight();

	Output.Light = Look.rgb * Peak * exp(-float3(0.2, 0.3, 0.46) * Air);
	Output.Reach = 2.6 + 1.1 * sqrt(Peak);
	Output.Spot = Corner * Output.Reach;
	Output.Clip = float4(Clip.xy + Output.Spot * 2.0 * ScreenSize.zw * Clip.w, 0.0, Clip.w);
	return Output;
}

float4 PSMain(Varyings Input) : SV_Target
{
	const float Square = dot(Input.Spot, Input.Spot) / (Sigma * Sigma);
	const float Core = exp(-0.5 * Square);
	const float Halo = 0.012 / ((1.0 + Square * 0.25) * (1.0 + Square * 0.25));
	const float Fade = saturate(1.0 - dot(Input.Spot, Input.Spot) / (Input.Reach * Input.Reach));
	return float4(Input.Light * (Core + Halo) * Fade * Fade, 0.0);
}
