// What a world hands to whatever draws it, free of any graphics API. The standalone
// renderer and the Unreal project both start from these.
#pragma once

#include "Core/Math.h"

#include <cmath>
#include <cstdint>

namespace Teinora
{
	/** What a chunk's vertex is made of, kept in the fourth component of its normal. */
	enum ETerrainMaterial : std::int8_t
	{
		TerrainPlain = 0,   // the colour alone
		TerrainLeaf = 8,    // a card cut out by the leaf texture: red and green are its coordinates, blue its tone
		TerrainNeedle = 16, // the same with the needle texture
		TerrainBark = 32,   // the colour over the bark texture
		TerrainStone = 64,  // the colour over the rock texture
		TerrainGround = 127 // textured by height and slope; see EncodeGroundLevel for its colour
	};

	struct TerrainVertex
	{
		float Position[3];
		std::int8_t Normal[4];
		std::uint8_t Color[4]; // alpha is wind sway
	};

	// The ground's blue channel says how high it stands over the nearest water, which
	// decides where shores are. Shaders/Surfaces.hlsli reads it back with the same numbers.
	inline constexpr double GroundLevelLowest = -8.0;
	inline constexpr double GroundLevelRange = 24.0;

	inline double EncodeGroundLevel(double HeightOverWater)
	{
		return (HeightOverWater - GroundLevelLowest) / GroundLevelRange;
	}

	/** A chunk's triangles, relative to Origin. Vertices are not shared between triangles. */
	struct ChunkMesh
	{
		Vec3 Origin;
		float Size = 0.0f;
		float MinZ = 0.0f;
		float MaxZ = 0.0f;
		const TerrainVertex* Vertices = nullptr;
		std::uint32_t VertexCount = 0;
		std::uint32_t SolidVertexCount = 0;  // leading vertices that are whole triangles: ground, wood and stone
		std::uint32_t ShadowVertexCount = 0; // the next ones, leaf cards, cast shadows too; grass follows them
	};

	/** Whoever shows the terrain. */
	class IChunkDisplay
	{
	public:
		virtual ~IChunkDisplay() = default;

		/** Shows a chunk, replacing what it showed before. The mesh is only valid during the call. */
		virtual void ShowChunk(int X, int Y, const ChunkMesh& Mesh) = 0;
		virtual void HideChunk(int X, int Y) = 0;
	};

	enum class EPrimitive : std::uint8_t
	{
		Cube,     // side 1
		Sphere,   // diameter 1
		Cylinder, // diameter 1, height 1, along local Z
		Count
	};

	struct ObjectDraw
	{
		EPrimitive Primitive = EPrimitive::Cube;
		Vec3 Position;
		Quat Rotation;
		Float3 Scale{ 1.0f, 1.0f, 1.0f };
		Float3 Color{ 1.0f, 1.0f, 1.0f };
	};

	/** Part of a plant moved out of its place by something, for the mesh to follow. */
	struct PlantMove
	{
		Vec3 Point;          // what has moved
		double Radius = 0.0; // how far around it the move reaches
		Vec3 Offset;
	};

	/** One star of the sky map, as the renderer takes it. */
	struct SkyStar
	{
		Float3 Direction;       // on the sphere of stars, before it has turned
		float Flux = 1.0f;      // 1 is the faintest star of the map
		std::uint8_t Color[4] = { 255, 255, 255, 255 }; // tint in RGB, and in alpha a random number for its flicker
	};

	// Everything in the sky keeps near one great circle, which meets the horizon at SkyHeading and
	// opposite it, and whose top stands SkyLean off the zenith. The stars turn around its pole.
	inline constexpr double SkyHeading = 2.2;
	inline constexpr double SkyLean = 0.37;

	// Sizes as the sky draws them, in radians across the radius. The planet's are also in Shaders/Planet.hlsli.
	inline constexpr double SunSize = 0.0173;
	inline constexpr double SecondSunSize = 0.0126;
	inline constexpr double PlanetSize = 0.06;
	inline constexpr double RingInner = 1.38; // in radii of the planet
	inline constexpr double RingOuter = 2.5;

	// The second sun's orbit and the planet's lean a little off the circle. Each leans one way for
	// half of its slant and the other way for the rest, and the place where it crosses the circle
	// creeps round over many days, so that where bodies meet in the sky differs from day to day.
	inline constexpr double SecondSunTilt = 0.1;
	inline constexpr double SecondSunNodeDays = 23.0;
	inline constexpr double PlanetTilt = 0.14;
	inline constexpr double PlanetOffset = 0.19;
	inline constexpr double PlanetNodeDays = 37.0;
	inline constexpr double PlanetSpinDays = 9.0;
	inline constexpr double PlanetWobble = 0.7;      // radians from the middle of its way round
	inline constexpr double PlanetWobbleDays = 29.0;

	inline Vec3 GetSkyAlong() { return { std::cos(SkyHeading), std::sin(SkyHeading), 0.0 }; }

	inline Vec3 GetSkyTop()
	{
		return { -std::sin(SkyHeading) * std::sin(SkyLean), std::cos(SkyHeading) * std::sin(SkyLean), std::cos(SkyLean) };
	}

	inline Vec3 GetSkyPole() { return Cross(GetSkyAlong(), GetSkyTop()); }

	/**
	 * How far round its circle something has come in the day: 0 as it rises at the hour of Rise,
	 * Pi as it sets at the hour of Set, and the rest of the way under the ground.
	 */
	inline double GetPathAngle(double Hour, double Rise, double Set)
	{
		const double Span = std::fmod(Set - Rise + 24.0, 24.0);
		const double Since = std::fmod(Hour - Rise + 48.0, 24.0);
		return Since < Span ? Pi * Since / Span : Pi + Pi * (Since - Span) / (24.0 - Span);
	}

	/** A point at Angle round the great circle, moved off it by an orbit tilted Tilt radians that crosses at Node, plus Offset. */
	inline Vec3 GetOrbitPoint(double Angle, double Tilt = 0.0, double Node = 0.0, double Offset = 0.0)
	{
		const double Off = std::tan(Tilt) * std::sin(Angle - Node) + Offset;
		return Normalize(GetSkyAlong() * std::cos(Angle) + GetSkyTop() * std::sin(Angle) + GetSkyPole() * Off);
	}

	// The world has two suns. The yellow-orange one rises first and the blue-green one joins it from
	// the opposite side of the sky; the first then sets, the second after it, and night falls until
	// the first rises again. Day is a count that goes up by one at each midnight.

	inline Vec3 GetSunDirection(double Hour, double Day = 0.0)
	{
		(void)Day; // its orbit is the circle itself
		return GetOrbitPoint(GetPathAngle(Hour, 5.0, 18.0));
	}

	/** Against the first sun's way round. They pass each other early in the afternoon, and on some days run together. */
	inline Vec3 GetSecondSunDirection(double Hour, double Day = 0.0)
	{
		const double Node = 2.0 * Pi * (Day + Hour / 24.0) / SecondSunNodeDays;
		return GetOrbitPoint(Pi - GetPathAngle(Hour, 9.0, 22.0), SecondSunTilt, Node);
	}

	/**
	 * A ringed planet that shines by the suns' light: up through the afternoon and most of the night, gone
	 * in the morning. It goes the first sun's way, which brings it past the second sun late in
	 * the afternoon, sometimes right across it.
	 */
	inline Vec3 GetPlanetDirection(double Hour, double Day = 0.0)
	{
		const double Node = 2.0 * Pi * (Day + Hour / 24.0) / PlanetNodeDays;
		return GetOrbitPoint(GetPathAngle(Hour, 13.0, 3.0), PlanetTilt, Node, PlanetOffset);
	}

	/** How far round the planet has turned, in radians. It takes many days, so each night shows a new side of it. */
	inline double GetPlanetSpin(double Days)
	{
		return std::fmod(Days / PlanetSpinDays, 1.0) * 2.0 * Pi;
	}

	/**
	 * Where the planet's pole points. It wanders slowly round a cone, so that at the same hour the
	 * rings stand wide open on one day and are seen far more nearly edge on some days later.
	 */
	inline Vec3 GetPlanetAxis(double Days)
	{
		const Vec3 Middle = Normalize(Vec3{ 0.36, -0.24, 0.9 });
		const Vec3 First = Normalize(Cross(Middle, Vec3{ 1.0, 0.0, 0.0 }));
		const Vec3 Second = Cross(Middle, First);
		const double Around = 2.0 * Pi * Days / PlanetWobbleDays;
		return Middle * std::cos(PlanetWobble) + (First * std::cos(Around) + Second * std::sin(Around)) * std::sin(PlanetWobble);
	}

	/** How much of a sun's light gets past the planet in front of it and its rings, from 0 to 1. */
	inline double GetLightPastPlanet(const Vec3& Sun, const Vec3& Planet, const Vec3& Axis, double Size)
	{
		const double Apart = std::acos(std::clamp(Dot(Sun, Planet), -1.0, 1.0));
		if (Apart > PlanetSize * RingOuter * 1.2)
		{
			return 1.0;
		}

		const double Edge = std::clamp((Apart - PlanetSize + Size) / (2.0 * Size), 0.0, 1.0);
		const double Past = Edge * Edge * (3.0 - 2.0 * Edge);

		// Where the line to the sun goes through the plane of the rings, as Shaders/Planet.hlsli draws them.
		const Vec3 Across = Normalize(Cross(Axis, Planet));
		const Vec3 Up = Cross(Planet, Across);
		const double X = Dot(Sun, Across) / PlanetSize;
		const double Y = Dot(Sun, Up) / PlanetSize;
		const double Facing = Dot(Axis, Planet);
		const double Depth = -Y * Dot(Axis, Up) / (std::fabs(Facing) > 1e-3 ? Facing : 1e-3);
		const double Radius = Length(Across * X + Up * Y + Planet * Depth);
		return Radius > RingInner && Radius < RingOuter ? Past * 0.6 : Past;
	}
}
