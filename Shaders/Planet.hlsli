// The ringed planet, which shines only by the light of the suns, giving back 10 to 15% of it from
// a matte, smoky surface. It is worked out directly for each ray: a sphere and a flat ring around
// it, seen from so far away that they are all at one distance.
#ifndef TEINORA_PLANET_HLSLI
#define TEINORA_PLANET_HLSLI

#include "Common.hlsli"

// These three are also in Engine/Render/SceneTypes.h, which works out its eclipses.
static const float PlanetSize = 0.04;   // radians across its radius, PlanetSize in Engine/Render/SceneTypes.h
static const float RingInner = 1.38;    // in radii of the planet
static const float RingOuter = 2.5;

// The suns as they are out in space, whatever the hour on the ground.
static const float3 SunInSpace = float3(1.75, 1.35, 0.85);
static const float3 Sun2InSpace = float3(0.72, 1.78, 1.65);

float GetRingDensity(float Radius)
{
	const float Edges = smoothstep(RingInner, RingInner + 0.05, Radius) * (1.0 - smoothstep(RingOuter - 0.09, RingOuter, Radius));
	const float Bands = 0.62 + 0.24 * sin(Radius * 41.0) + 0.14 * sin(Radius * 97.0 + 1.3);
	const float Gap = 1.0 - 0.9 * (1.0 - smoothstep(0.0, 0.045, abs(Radius - 2.0))); // a dark division, as Saturn has
	return saturate(Bands) * Edges * Gap;
}

/** 0 where a point of the ring lies in the planet's shadow from a sun, otherwise 1. */
float GetRingLight(float3 Point, float3 Sun)
{
	const float Along = dot(Point, Sun);
	return Along > 0.0 ? 1.0 : smoothstep(0.96, 1.04, length(Point - Sun * Along));
}

/** How much of a sun's light the rings let through to a point of the planet's surface. */
float GetRingShade(float3 Surface, float3 Sun, float3 Axis)
{
	const float Across = dot(Sun, Axis);
	const float Rise = -dot(Surface, Axis) / (abs(Across) > 1e-3 ? Across : 1e-3);
	return Rise > 0.0 ? 1.0 - 0.6 * GetRingDensity(length(Surface + Sun * Rise)) : 1.0;
}

float3 AddPlanet(float3 Color, float3 Direction)
{
	const float3 Toward = PlanetDirection.xyz;
	if (dot(Direction, Toward) < 0.85 || Toward.z < -0.25)
	{
		return Color;
	}

	const float3 Axis = PlanetAxis.xyz;
	const float3 Across = normalize(cross(Axis, Toward));
	const float3 Up = cross(Toward, Across);
	const float2 Spot = float2(dot(Direction, Across), dot(Direction, Up)) / PlanetSize;
	if (dot(Spot, Spot) > RingOuter * RingOuter * 4.0)
	{
		return Color;
	}

	// The sky goes on being lit in front of the planet, which only hides what lies behind it.
	const float3 Air = SkyColor(Direction);
	float3 Result = Color;

	// Where this ray crosses the plane of the rings, and whether that is in front of the planet.
	const float Tilt = dot(Axis, Toward);
	const float RingDepth = -Spot.y * dot(Axis, Up) / (abs(Tilt) > 1e-3 ? Tilt : 1e-3);
	const float3 RingPoint = Across * Spot.x + Up * Spot.y + Toward * RingDepth;
	const float RingRadius = length(RingPoint);
	const float Ring = GetRingDensity(RingRadius) * 0.6;
	const float3 RingLook = float3(0.085, 0.12, 0.185) * (SunInSpace * GetRingLight(RingPoint, SunDirection.xyz) + Sun2InSpace * GetRingLight(RingPoint, Sun2Direction.xyz)) * 0.5 + Air;

	if (RingDepth > 0.0)
	{
		Result = lerp(Result, RingLook, Ring);
	}

	const float Reach = dot(Spot, Spot);
	const float Disc = 1.0 - smoothstep(0.86, 1.05, Reach); // a smoky edge, with no hard line to it
	if (Disc > 0.0)
	{
		const float3 Surface = Across * Spot.x + Up * Spot.y - Toward * sqrt(max(1.0 - Reach, 0.0));

		// Soft bands along the latitude, leaning a little and sliding as it turns. Longitudes are
		// counted from the side facing the ground, which the same face keeps doing all through a day.
		const float Latitude = dot(Surface, Axis);
		const float Longitude = atan2(dot(Surface, Across), dot(Surface, cross(Axis, Across))) + PlanetAxis.w;
		const float Girth = sqrt(max(1.0 - Latitude * Latitude, 0.0));
		const float Flow = (sin(Longitude * 3.0 + Latitude * 7.0) * 0.06 + sin(Longitude * 5.0 - Latitude * 4.0) * 0.03) * Girth;
		const float Bands = 0.5 + 0.35 * sin((Latitude + Flow) * 9.0) + 0.15 * sin((Latitude - Flow) * 21.0 + 0.8);

		// Its sides differ, so that a turn of it shows from any angle: one half a shade the darker,
		// with a pale storm on the lighter half and dark basins on the other, in both hemispheres.
		const float3 Fixed = float3(Girth * cos(Longitude), Girth * sin(Longitude), Latitude);
		const float Side = 0.5 + 0.5 * dot(Fixed, float3(0.8, 0.6, 0.0));
		const float Storm = smoothstep(0.84, 0.95, dot(Fixed, float3(0.60, 0.35, 0.72)) + Flow);
		const float Basin = smoothstep(0.66, 0.9, dot(Fixed, float3(-0.72, -0.30, 0.62)) + Flow);
		const float Lower = smoothstep(0.7, 0.92, dot(Fixed, float3(-0.2, -0.82, -0.53)) + Flow);

		// Luminance from 0.10 to 0.15 of what falls on it, and no highlight at all.
		const float Tone = lerp(lerp(saturate(Bands) * 0.55 + Side * 0.45, 1.0, Storm), 0.0, max(Basin, Lower));
		const float3 Albedo = lerp(float3(0.075, 0.102, 0.155), float3(0.112, 0.154, 0.228), Tone);
		const float3 Lit = SunInSpace * saturate(dot(Surface, SunDirection.xyz)) * GetRingShade(Surface, SunDirection.xyz, Axis)
			+ Sun2InSpace * saturate(dot(Surface, Sun2Direction.xyz)) * GetRingShade(Surface, Sun2Direction.xyz, Axis);
		Result = lerp(Result, Albedo * Lit + Air, Disc);
	}

	if (RingDepth <= 0.0)
	{
		Result = lerp(Result, RingLook, Ring);
	}
	return Result;
}

#endif
