// The clouds: a shell of billowing noise high above a round world, marched through ray by ray and
// lit by the suns, so that they have depth, the sky shows through wherever they are thin, and
// they curve away to sink below the horizon. The shell stays centred under the camera; only its
// clouds are fixed to the ground, as if the world were rolling along beneath them.
#ifndef TEINORA_CLOUDS_HLSLI
#define TEINORA_CLOUDS_HLSLI

#include "Common.hlsli"

// Red holds the broad shapes, green the finer ones that eat into their edges. Both tile.
[[vk::binding(1, 1)]] Texture3D<float4> CloudNoise : register(t1);

static const float CloudBottom = 1700.0;
static const float CloudTop = 3100.0;
static const float CloudReach = 26000.0;     // metres of the layer that a ray goes through at most
static const float CloudExtinction = 0.010;  // per metre, at full density: lower is more see-through
static const float HazeFrom = 6000.0;        // metres of the ray before far clouds begin to sink into the haze
static const float WorldRadius = 40000.0;    // small enough for the sky to be seen to curve

// Integer mixing: the usual sin() hash runs out of float precision for the
// large coordinates near the horizon and tears anything built on it into straight edges.
float Hash(int2 Cell)
{
	uint Value = asuint(Cell.x) * 1597334677u ^ asuint(Cell.y) * 3812015801u;
	Value ^= Value >> 15;
	Value *= 2246822519u;
	Value ^= Value >> 13;
	return float(Value >> 8) / 16777216.0;
}

float Noise(float2 Value)
{
	const float2 Floor = floor(Value);
	const int2 Cell = int2(Floor);
	const float2 Part = Value - Floor;
	const float2 Fade = Part * Part * (3.0 - 2.0 * Part);
	return lerp(
		lerp(Hash(Cell), Hash(Cell + int2(1, 0)), Fade.x),
		lerp(Hash(Cell + int2(0, 1)), Hash(Cell + int2(1, 1)), Fade.x),
		Fade.y);
}

/** Height above the round ground of a point given relative to the camera. */
float GetAltitude(float3 Relative)
{
	return length(float3(Relative.xy, Relative.z + CameraPosition.z + WorldRadius)) - WorldRadius;
}

/** How far along a ray from a point relative to the camera the sphere at an altitude lies: nearer and farther crossing, both below zero when it misses. */
float2 CrossSphere(float3 Origin, float3 Direction, float Altitude)
{
	const float3 FromCentre = float3(Origin.xy, Origin.z + CameraPosition.z + WorldRadius);
	const float Inside = length(FromCentre);
	const float Radius = WorldRadius + Altitude;
	const float Along = dot(FromCentre, Direction);
	const float Square = Along * Along + (Radius - Inside) * (Radius + Inside);
	if (Square < 0.0)
	{
		return float2(-1.0, -1.0);
	}
	const float Root = sqrt(Square);
	return float2(-Along - Root, -Along + Root);
}

static const int CloudMapSide = 48;     // CloudMapSide in Engine/Render/Renderer.h
static const float CloudMapTop = 1.3;   // and CloudMapTop

float ReadCloudMap(int2 Cell)
{
	const int Index = clamp(Cell.x, 0, CloudMapSide - 1) + clamp(Cell.y, 0, CloudMapSide - 1) * CloudMapSide;
	const uint Packed = CloudMap[Index >> 4][(Index >> 2) & 3];
	return float((Packed >> ((Index & 3) * 8)) & 255u) / 255.0 * CloudMapTop;
}

/**
 * How much cloud the water magic up there makes over a point relative to the camera, from 0 to about 1.3: what
 * the storms' sheet says, and past its edges, where nothing is worked out, what the setting for the cover says.
 */
float GetGathered(float2 Relative)
{
	const float Usual = (Conditions.z + 0.5) / 4.2; // what gives the cover the setting asks for, see GetCover
	if (CloudMapPlace.w <= 0.0)
	{
		return Usual;
	}
	const float2 Place = (Relative - CloudMapPlace.xy) / CloudMapPlace.z - 0.5;
	const float2 Base = floor(Place);
	const float2 Part = Place - Base;
	const int2 Cell = int2(Base);
	const float Here = lerp(lerp(ReadCloudMap(Cell), ReadCloudMap(Cell + int2(1, 0)), Part.x),
		lerp(ReadCloudMap(Cell + int2(0, 1)), ReadCloudMap(Cell + int2(1, 1)), Part.x), Part.y);
	// Past the sheet's edge the sky goes on as it is at the edge for some kilometres, since weather is wider than
	// the sheet, and only then comes to what the setting says.
	const float2 FromEdge = min(Place, CloudMapSide - 1.0 - Place);
	return lerp(Usual, Here, saturate(1.0 + min(FromEdge.x, FromEdge.y) / 30.0));
}

/**
 * How much of the sky has cloud, as the Cloud Cover setting says it, 1 for the usual, for what the water magic up
 * there gathers: none where it makes no cloud, overcast where it rains. ShownCloudCover in Game/TeinoraSim.cpp.
 */
float GetCover(float Gathered)
{
	return -0.5 + 4.2 * clamp(Gathered, 0.0, 1.3);
}

/** How dense the cloud is at a point relative to the camera, from 0 to 1. Tile sizes divide the distance positions wrap at. */
float SampleCloud(float3 Relative, bool bFine)
{
	const float Altitude = GetAltitude(Relative);
	const float Height = saturate((Altitude - CloudBottom) / (CloudTop - CloudBottom));
	const float Profile = smoothstep(0.0, 0.08, Height) * (1.0 - smoothstep(0.25, 1.0, Height)); // flat below, round above
	if (Profile <= 0.0)
	{
		return 0.0;
	}

	// Carried by the wind aloft, and slowly changing their shape as they go: Storms.cpp's MeasureCloud reads the same.
	const float2 Carried = Relative.xy + CameraPosition.xy - CloudDrift.xy;
	const float Broad = CloudNoise.SampleLevel(LinearWrap, float3(Carried / 4096.0, Altitude / 2048.0 + CloudDrift.z), 0.0).r;
	// Clouds are where water magic has gathered up here, and the more of it, the more of the sky they fill and the
	// thicker they are. The noise only gives them their shapes.
	const float Gathered = GetGathered(Relative.xy);
	const float Thickness = lerp(0.45, 2.2, saturate(Gathered));
	float Density = saturate((Broad - 0.74 + GetCover(Gathered) * 0.2 - (1.0 - Profile) * 0.16) * 5.0) * Thickness;
	if (bFine && Density > 0.0)
	{
		const float Fine = CloudNoise.SampleLevel(LinearWrap, float3(Carried / 512.0, Altitude / 512.0 + CloudDrift.z * 4.0), 0.0).g;
		const float Shape = saturate(Density / Thickness);
		Density = max(Density - (1.0 - Fine) * 0.45 * (1.0 - Shape * 0.6) * Thickness, 0.0);
	}
	return Density;
}

/** Metres of full-density cloud between a point relative to the camera and a sun. */
float MeasureCloudToSun(float3 Relative, float3 Sun)
{
	float Depth = 0.0;
	float Along = 0.0;
	float Stride = 60.0;
	for (int Index = 0; Index < 4; ++Index)
	{
		Stride *= 2.0;
		Along += Stride;
		Depth += SampleCloud(Relative + Sun * Along, false) * Stride;
	}
	return Depth;
}

float Scatter(float Cosine, float Forward)
{
	const float Square = Forward * Forward;
	return (1.0 - Square) / pow(1.0 + Square - 2.0 * Forward * Cosine, 1.5) * 0.25;
}

/** Puts the clouds in front of what a ray sees beyond them. Dither moves the steps within a pixel. */
float3 AddClouds(float3 Color, float3 Direction, int Steps, float Dither)
{
	if (Options.y <= 0.0)
	{
		return Color;
	}

	// The stretch of the ray that lies within the shell.
	const float Eye = CameraPosition.z;
	const float3 Origin = float3(0.0, 0.0, 0.0);
	const float2 Top = CrossSphere(Origin, Direction, CloudTop);
	const float2 Bottom = CrossSphere(Origin, Direction, CloudBottom);
	float Enter = 0.0;
	float Leave = Bottom.x > 0.0 ? Bottom.x : Top.y;
	if (Eye < CloudBottom)
	{
		Enter = Bottom.y; // from inside, a ray always goes out through both spheres
		Leave = Top.y;
	}
	else if (Eye > CloudTop)
	{
		Enter = Top.x;
	}
	if (Leave <= Enter || Enter < 0.0)
	{
		return Color;
	}
	Leave = min(Leave, Enter + CloudReach);

	const float Stride = (Leave - Enter) / Steps;
	const float Cosine = dot(Direction, SunDirection.xyz);
	const float Cosine2 = dot(Direction, Sun2Direction.xyz);
	const float3 Shine = SunColor.rgb * (0.3 + lerp(Scatter(Cosine, -0.15), Scatter(Cosine, 0.72), 0.45)) * 0.8;
	const float3 Shine2 = Sun2Color.rgb * (0.3 + lerp(Scatter(Cosine2, -0.15), Scatter(Cosine2, 0.72), 0.45)) * 0.8;
	const bool bFirstUp = dot(SunColor.rgb, SunColor.rgb) > 1e-4;
	const bool bSecondUp = dot(Sun2Color.rgb, Sun2Color.rgb) > 1e-4;
	const float3 Ambient = SkyZenith.rgb * 0.55 + SkyHorizon.rgb * 0.45 + PlanetColor.rgb * 0.25;

	float Clear = 1.0;
	float3 Light = float3(0.0, 0.0, 0.0);
	for (int Step = 0; Step < Steps && Clear > 0.02; ++Step)
	{
		const float3 Point = Direction * (Enter + (Step + Dither) * Stride);
		const float Density = SampleCloud(Point, true);
		if (Density > 0.002)
		{
			const float Edge = 1.0 - exp(-Density * 5.0); // thin wisps let light through rather than catch it
			const float Rise = saturate((GetAltitude(Point) - CloudBottom) / (CloudTop - CloudBottom));
			float3 Lit = Ambient * (0.55 + 0.45 * Rise);
			if (bFirstUp)
			{
				Lit += Shine * Edge * exp(-MeasureCloudToSun(Point, SunDirection.xyz) * CloudExtinction * 1.1);
			}
			if (bSecondUp)
			{
				Lit += Shine2 * Edge * exp(-MeasureCloudToSun(Point, Sun2Direction.xyz) * CloudExtinction * 1.1);
			}
			if (BoltFlash.w > 0.0)
			{
				Lit += float3(0.8, 0.82, 1.0) * BoltFlash.w * exp(-length(Point - BoltFlash.xyz) * 0.0009);
			}

			const float Taken = 1.0 - exp(-Density * CloudExtinction * Stride);
			Light += Lit * Clear * Taken;
			Clear *= 1.0 - Taken;
		}
	}

	// The march gives up on a cloud once next to nothing gets through it, and then nothing does: not the stars
	// at night, and not a sun's disc, which is bright enough to show by the last fiftieth of its light.
	Clear = saturate((Clear - 0.02) / 0.98);

	// Far away they sink into the haze along the horizon, and past it from below.
	const float Near = exp(-max(Enter - HazeFrom, 0.0) * 0.00011) * saturate(1.0 + Direction.z * 40.0);
	return lerp(Color, Color * Clear + Light, Near);
}

/** How much of a sun's light gets through the clouds to a point near the ground. */
float GetCloudShade(float3 Relative, float3 Sun)
{
	if (Options.y <= 0.0)
	{
		return 1.0;
	}

	// Only what comes straight through: thick cloud lets next to none of that by. What it scatters on its way
	// down still arrives, from the whole sky and with no shadows: see CloudPassing in Lighting.hlsli.
	const float Low = CrossSphere(Relative, Sun, 2000.0).y;
	const float High = CrossSphere(Relative, Sun, 2450.0).y;
	const float Depth = SampleCloud(Relative + Sun * Low, false) + SampleCloud(Relative + Sun * High, false);
	return lerp(0.03, 1.0, exp(-Depth * 350.0 * CloudExtinction));
}

#endif
