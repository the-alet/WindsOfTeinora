// Direct sun and moon light, shadows from cascades or traced rays, and a hemispheric ambient term.
#ifndef TEINORA_LIGHTING_HLSLI
#define TEINORA_LIGHTING_HLSLI

#include "Clouds.hlsli"
#include "Common.hlsli"

#ifdef RAY_TRACED
#include "Rays.hlsli"
#endif

float SampleShadow(float3 Relative, float3 Normal, float ViewDepth)
{
	const float Texel = CascadeEnds.w;
	if (Texel <= 0.0 || ViewDepth >= CascadeEnds.z)
	{
		return 1.0;
	}

	const int Cascade = ViewDepth < CascadeEnds.x ? 0 : (ViewDepth < CascadeEnds.y ? 1 : 2);
	const float NormalOffsets[3] = { 0.05, 0.16, 0.6 };
	const float4 Position = mul(ShadowMatrices[Cascade], float4(Relative + Normal * NormalOffsets[Cascade], 1.0));

	float Light = 0.0;
	for (int Y = -1; Y <= 1; Y += 2)
	{
		for (int X = -1; X <= 1; X += 2)
		{
			// The atlas is three tiles wide, so a texel is a third as wide in U as in V.
			Light += Texture0.SampleCmpLevelZero(ShadowCompare, Position.xy + float2(X / 3.0, Y) * Texel * 0.75, Position.z);
		}
	}
	Light *= 0.25;

	// The last cascade fades out instead of ending in a visible line.
	const float Fade = saturate((ViewDepth - CascadeEnds.z * 0.85) / (CascadeEnds.z * 0.15));
	return lerp(Light, 1.0, Fade);
}

/**
 * How much of one sun reaches a point. Lift is how far the point may sit from the shape rays see,
 * which the wind does not bend. Without rays, only the sun the cascades follow casts a shadow.
 */
float GetSunShadow(float3 Relative, float3 Normal, float ViewDepth, float Lift, float3 Sun, float Which)
{
#ifdef RAY_TRACED
#ifdef FOLIAGE
	// A card must not shade itself, whichever way it happens to lie against the light.
	return TraceLight(Relative + Sun * (0.5 + Lift + ViewDepth * 0.0006), Sun, RayReach);
#else
	return TraceLight(Relative + Normal * (0.03 + Lift + ViewDepth * 0.0006), Sun, RayReach);
#endif
#else
	return Which == Detail.y ? SampleShadow(Relative, Normal, ViewDepth) : 1.0;
#endif
}

// Of a sun's light that cloud keeps from coming straight down, the share that still reaches the ground,
// scattered: an overcast day is a quarter or so as bright as a clear one, and has no shadows.
static const float CloudPassing = 0.25;

/** A colour with some of its hue taken out, as light is that has been scattered about in cloud. */
float3 Greyed(float3 Color, float Amount)
{
	return lerp(Color, dot(Color, float3(0.2126, 0.7152, 0.0722)).xxx, Amount);
}

/** How much of the sky a point can see, which is what darkens the ground under trees and beside rocks. */
float GetOpenness(float3 Relative, float3 Normal, float ViewDepth, float Lift)
{
#ifdef FOLIAGE
	return 0.8; // the rounded normals of a crown already say how open a leaf is
#elif defined(RAY_TRACED)
	const float Fade = 1.0 - smoothstep(200.0, 320.0, ViewDepth);
	if (Fade <= 0.0)
	{
		return 1.0;
	}

	static const float Reach = 9.0;
	const float3 Origin = Relative + Normal * (0.05 + Lift);
	const float3 Side = normalize(cross(Normal, abs(Normal.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0)));
	const float3 Other = cross(Normal, Side);

	float Open = TraceLight(Origin, Normal, Reach);
	Open += TraceLight(Origin, normalize(Normal + Side), Reach);
	Open += TraceLight(Origin, normalize(Normal - Side * 0.5 + Other * 0.87), Reach);
	Open += TraceLight(Origin, normalize(Normal - Side * 0.5 - Other * 0.87), Reach);
	return lerp(1.0, 0.3 + 0.7 * Open * 0.25, Fade);
#else
	return 1.0;
#endif
}

/** What fires and lava near the camera throw on a point. Too few and too low to be worth shadows. */
float3 GetFirelight(float3 Normal, float3 Relative)
{
	float3 Light = float3(0.0, 0.0, 0.0);
	for (int Index = 0; Index < 8 && HotLights[Index].w > 0.0; ++Index)
	{
		const float Reach = HotLights[Index].w;
		const float3 Towards = HotPoints[Index].xyz - Relative;
		const float Square = dot(Towards, Towards);
		if (Square < Reach * Reach)
		{
			const float Fade = 1.0 - Square / (Reach * Reach);
			const float Facing = saturate(dot(Normal, Towards) / sqrt(max(Square, 1e-4)) * 0.75 + 0.25);
			const float Time = CameraPosition.w;
			const float Flicker = HotPoints[Index].w > 0.0 ? 0.86 + 0.14 * sin(Time * (9.0 + Index)) * sin(Time * (5.3 + Index * 1.7) + 1.0) : 1.0;
			const float Near = Reach * Reach * 0.015; // what keeps the light from blowing up right beside it
			Light += HotLights[Index].rgb * (Fade * Fade * Facing * Flicker / (1.0 + Square / Near));
		}
	}
	return Light;
}

float3 ShadeSurface(float3 Albedo, float3 Normal, float3 Relative, float ViewDepth, float Gloss, float Lift, float3 Glow = float3(0.0, 0.0, 0.0))
{
	const float3 View = -normalize(Relative);

	// The planet's light arrives scattered by the air: a little of it straight from there, most from the whole sky.
	float3 Light = PlanetColor.rgb * (saturate(dot(Normal, PlanetDirection.xyz)) * 0.25 + (Normal.z * 0.5 + 0.5) * 0.35);
	float3 Highlight = float3(0.0, 0.0, 0.0);

	// What cloud lets straight through of each sun, and what it scatters on down instead, greyer and from all over the sky.
	const bool bFirstUp = dot(SunColor.rgb, SunColor.rgb) > 1e-4;
	const bool bSecondUp = dot(Sun2Color.rgb, Sun2Color.rgb) > 1e-4;
	const float Through = bFirstUp ? GetCloudShade(Relative, SunDirection.xyz) : 1.0;
	const float Through2 = bSecondUp ? GetCloudShade(Relative, Sun2Direction.xyz) : 1.0;
	const float3 Diffused = Greyed(SunColor.rgb * (max(SunDirection.z, 0.0) * (1.0 - Through)) + Sun2Color.rgb * (max(Sun2Direction.z, 0.0) * (1.0 - Through2)), 0.6) * CloudPassing;

	// A soft highlight is what makes surface detail catch the light.
	const float SunLight = saturate(dot(Normal, SunDirection.xyz));
	if (SunLight > 0.0 && bFirstUp)
	{
		const float3 Arriving = SunColor.rgb * SunLight * Through * GetSunShadow(Relative, Normal, ViewDepth, Lift, SunDirection.xyz, 0.0);
		Light += Arriving;
		Highlight += Arriving * pow(saturate(dot(Normal, normalize(SunDirection.xyz + View))), 24.0) * Gloss;
	}

	const float Sun2Light = saturate(dot(Normal, Sun2Direction.xyz));
	if (Sun2Light > 0.0 && bSecondUp)
	{
		const float3 Arriving = Sun2Color.rgb * Sun2Light * Through2 * GetSunShadow(Relative, Normal, ViewDepth, Lift, Sun2Direction.xyz, 1.0);
		Light += Arriving;
		Highlight += Arriving * pow(saturate(dot(Normal, normalize(Sun2Direction.xyz + View))), 24.0) * Gloss;
	}

	// Sky from above, warm bounce from the lit ground below. Under an overcast sky the light from above is the
	// cloud's, grey, and the ground gives back less for having less direct light on it.
	const float Overcast = Shade.x;
	const float3 Sky = Greyed(SkyZenith.rgb * 0.6 + SkyHorizon.rgb * 0.22, 0.7 * Overcast) * (1.0 - 0.35 * Overcast) + Diffused;
	const float3 Bounce = SkyHorizon.rgb * 0.12 * (1.0 - 0.5 * Overcast) + (SunColor.rgb * Through + Sun2Color.rgb * Through2) * 0.05 + Diffused * 0.3;
	Light += lerp(Bounce, Sky, Normal.z * 0.5 + 0.5) * GetOpenness(Relative, Normal, ViewDepth, Lift);
	Light += GetFirelight(Normal, Relative);

	return ApplyFog(Albedo * Light + Highlight + Glow, Relative);
}

#endif
