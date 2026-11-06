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
	return TraceLight(Relative + Sun * (0.5 + Lift + ViewDepth * 0.0006), Sun, RayReach) * GetCloudShade(Relative, Sun);
#else
	return TraceLight(Relative + Normal * (0.03 + Lift + ViewDepth * 0.0006), Sun, RayReach) * GetCloudShade(Relative, Sun);
#endif
#else
	return (Which == Detail.y ? SampleShadow(Relative, Normal, ViewDepth) : 1.0) * GetCloudShade(Relative, Sun);
#endif
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

float3 ShadeSurface(float3 Albedo, float3 Normal, float3 Relative, float ViewDepth, float Gloss, float Lift)
{
	const float3 View = -normalize(Relative);

	float3 Light = PlanetColor.rgb * saturate(dot(Normal, PlanetDirection.xyz));
	float3 Highlight = float3(0.0, 0.0, 0.0);

	// A soft highlight is what makes surface detail catch the light.
	const float SunLight = saturate(dot(Normal, SunDirection.xyz));
	if (SunLight > 0.0 && dot(SunColor.rgb, SunColor.rgb) > 1e-4)
	{
		const float3 Arriving = SunColor.rgb * SunLight * GetSunShadow(Relative, Normal, ViewDepth, Lift, SunDirection.xyz, 0.0);
		Light += Arriving;
		Highlight += Arriving * pow(saturate(dot(Normal, normalize(SunDirection.xyz + View))), 24.0) * Gloss;
	}

	const float Sun2Light = saturate(dot(Normal, Sun2Direction.xyz));
	if (Sun2Light > 0.0 && dot(Sun2Color.rgb, Sun2Color.rgb) > 1e-4)
	{
		const float3 Arriving = Sun2Color.rgb * Sun2Light * GetSunShadow(Relative, Normal, ViewDepth, Lift, Sun2Direction.xyz, 1.0);
		Light += Arriving;
		Highlight += Arriving * pow(saturate(dot(Normal, normalize(Sun2Direction.xyz + View))), 24.0) * Gloss;
	}

	// Sky from above, warm bounce from the lit ground below.
	const float3 Bounce = SkyHorizon.rgb * 0.12 + (SunColor.rgb + Sun2Color.rgb) * 0.05;
	Light += lerp(Bounce, SkyZenith.rgb * 0.6 + SkyHorizon.rgb * 0.22, Normal.z * 0.5 + 0.5) * GetOpenness(Relative, Normal, ViewDepth, Lift);

	return ApplyFog(Albedo * Light + Highlight, Relative);
}

#endif
