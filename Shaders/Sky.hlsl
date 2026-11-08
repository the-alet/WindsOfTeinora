#include "Clouds.hlsli"
#include "Planet.hlsli"

// The star map has been drawn to Texture0 in this frame.

struct Varyings
{
	float4 Clip : SV_Position;
	float2 Screen : TEXCOORD0;
	float2 Uv : TEXCOORD1;
};

static const float SunRadius = 0.0173;       // SunSize in Engine/Render/SceneTypes.h
static const float SecondSunRadius = 0.0126; // SecondSunSize
static const float CoronaLight = 0.3;        // at the rim, as a share of the sun's own light

/**
 * A sun's corona: its thin outer air, lit by it and glowing all round, far fainter than its disc. By day
 * the sky drowns it; when the planet covers the disc and the sky goes dark, it shows. It is brightest at
 * the rim and falls off steeply, drawn out into broad, smooth streamers along the sun's equator, which lies
 * along the sky's great circle, and short at its poles. Right at the rim is a thin red layer of hotter
 * gas, with prominences standing out of it here and there. The planet hides what lies behind it, as it
 * is drawn after this.
 */
float3 GetCorona(float3 Direction, float3 Sun, float Radius, float3 Light, float Seed)
{
	const float Strength = max(max(Light.r, Light.g), Light.b);
	const float Cosine = dot(Direction, Sun);
	if (Strength <= 0.0 || Cosine < cos(Radius * 20.0))
	{
		return float3(0.0, 0.0, 0.0);
	}
	const float Out = acos(saturate(Cosine)) / Radius; // in the sun's radii
	if (Out < 1.0)
	{
		return float3(0.0, 0.0, 0.0); // the disc itself
	}

	// Where round the sun, from its equator.
	const float3 Across = normalize(cross(StarPole.xyz, Sun));
	const float3 Up = cross(Sun, Across);
	const float3 Off = Direction - Sun * Cosine;
	const float Angle = atan2(dot(Off, Up), dot(Off, Across));
	const float Equator = cos(Angle) * cos(Angle);

	const float Falloff = lerp(3.0, 1.8, Equator); // the streamers along the equator reach far, the glow at the poles less
	const float Streamers = 0.55 + 0.9 * Equator * (0.6 + 0.4 * sin(Angle * 3.0 + Seed));
	const float3 Pearl = lerp(float3(1.0, 1.0, 1.0), Light / Strength, 0.3);
	float3 Glow = Pearl * (Strength * CoronaLight * pow(Out, -Falloff) * Streamers);

	// The red layer at the rim, and prominences out of it.
	const float Rim = 1.0 - smoothstep(1.0, 1.035, Out);
	const float Prominence = saturate(sin(Angle * 5.0 + Seed * 3.1) * sin(Angle * 11.0 - Seed * 1.7) * 2.5 - 0.9) * (1.0 - smoothstep(1.0, 1.12, Out));
	Glow += float3(1.0, 0.16, 0.24) * (Strength * CoronaLight * 2.5 * max(Rim, Prominence));
	return Glow;
}

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

	// Around each disc, the light it sends that the air scatters a little aside: the aureole. It outshines the
	// corona by day and goes out with the disc, which is why the corona is only seen when the disc is covered.
	Color += SunColor.rgb * pow(saturate(SunDot), 3000.0) * 2.0 + Sun2Color.rgb * pow(saturate(Sun2Dot), 3000.0) * 2.0;
	Color += GetCorona(Direction, SunDirection.xyz, SunRadius, Coronas[0].rgb, 0.0);
	Color += GetCorona(Direction, Sun2Direction.xyz, SecondSunRadius, Coronas[1].rgb, 2.7);

	if (GetNight() > 0.0)
	{
		Color += Texture0.Load(int3(Input.Clip.xy, 0)).rgb;
	}

	Color = AddPlanet(Color, Direction);

	// A different start in every pixel turns the banding of the steps into fine grain.
	const float Dither = frac(52.9829189 * frac(dot(Input.Clip.xy, float2(0.06711056, 0.00583715))));
	return float4(AddClouds(Color, Direction, 40, Dither), 1.0);
}
