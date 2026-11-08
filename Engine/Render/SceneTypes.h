// What a world hands to whatever draws it, free of any graphics API. The standalone
// renderer and the Unreal project both start from these.
#pragma once

#include "Core/Math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

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
		TerrainMolten = 88, // black crust with light in its cracks: red says how hot, from set hard to wide open
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

	/** One length of a lightning bolt's channel. */
	struct BoltSegment
	{
		Vec3 From;
		Vec3 To;
		double Current = 1.0; // 1 on the channel that reaches the ground, less on what branches off it
		double Reached = 0.0; // when the leader got here, as a share of its way down
	};

	/** A bolt as it stands at this moment. */
	struct BoltDraw
	{
		const std::vector<BoltSegment>* Segments = nullptr;
		double Leader = 1.0;   // how far down the leader has got, from 0 to 1
		double Faint = 0.0;    // how bright that leader is
		double Stroke = 0.0;   // how bright the channel is that reached the ground
		double Branches = 0.0; // and what branches off it, which goes out sooner
	};

	/** Something burning near the camera: it lights what is around it, and may have flames. */
	struct HotLight
	{
		Vec3 Point;               // where the light comes from
		Vec3 Color;
		double Reach = 0.0;       // metres to where it has gone
		double FlameHeight = 0.0; // none on what only glows
		double FlameBreadth = 0.36; // how far its flames spread at the foot, as a share of their height
	};

	/** A puff of steam off boiling water, or of smoke over a fire. */
	struct PuffDraw
	{
		Vec3 Point;
		double Radius = 0.0;    // metres
		double Thickness = 0.0; // from 0 for what has gone to 1 for what hides all behind it
		double Seed = 0.0;      // so that no two look alike
		double Floor = 0.0;     // the water or ground it came off, which hides what is below it
		double Shade = 1.0;     // how much of the light on it it gives back: all for steam, less for smoke
		double Firelight = 0.0; // how much a fire's light reaches it, as a share of that light at the flame
		double Glow = 0.0;      // light of its own, in the colour of light magic: a firefly is a small round glowing puff
	};

	/** The colour of a flame's light, which HotLight carries for each fire. Also in Shaders/Puffs.hlsl and Unreal/Scripts/make_content.py. */
	inline constexpr double FlameLightRed = 2.6;
	inline constexpr double FlameLightGreen = 1.1;
	inline constexpr double FlameLightBlue = 0.3;

	/** A corner of the sheet that a puff is drawn on. */
	struct PuffCorner
	{
		Vec3 Point;
		double Across = 0.0; // where it is on the sheet, from -1 to 1
		double Up = 0.0;
	};

	/**
	 * The sheet for a puff: a square turned to face the eye, cut off along the water or ground it
	 * came off, which would hide that part anyway. Returns how many corners are left, up to 5, in
	 * order around it.
	 */
	inline int CutPuffSheet(const PuffDraw& Puff, const Vec3& Eye, PuffCorner (&Out)[5])
	{
		const Vec3 Along = Normalize(Puff.Point - Eye);
		const Vec3 Side = std::fabs(Along.Z) > 0.999 ? Vec3{ 1.0, 0.0, 0.0 } : Normalize(Cross(Along, Vec3{ 0.0, 0.0, 1.0 }));
		const Vec3 Up = Cross(Side, Along);
		const auto MakeCorner = [&](double Across, double Rise)
		{
			return PuffCorner{ Puff.Point + (Side * Across + Up * Rise) * Puff.Radius, Across, Rise };
		};
		const PuffCorner Square[4] ={ MakeCorner(-1.0, -1.0), MakeCorner(1.0, -1.0), MakeCorner(1.0, 1.0), MakeCorner(-1.0, 1.0) };

		int Count = 0;
		for (int Index = 0; Index < 4; ++Index)
		{
			const PuffCorner& From = Square[Index];
			const PuffCorner& To = Square[(Index + 1) % 4];
			const double FromAbove = From.Point.Z - Puff.Floor;
			const double ToAbove = To.Point.Z - Puff.Floor;
			if (FromAbove >= 0.0)
			{
				Out[Count++] = From;
			}
			if ((FromAbove >= 0.0) != (ToAbove >= 0.0))
			{
				const double Share = FromAbove / (FromAbove - ToAbove);
				Out[Count++] = { From.Point + (To.Point - From.Point) * Share, From.Across + (To.Across - From.Across) * Share, From.Up + (To.Up - From.Up) * Share };
			}
		}
		return Count;
	}

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
	inline constexpr double PlanetSize = 0.035; // 1.7 times as far off as it once stood, at 0.06; still twice the suns' radius, so it covers both
	inline constexpr double RingInner = 1.38; // in radii of the planet
	inline constexpr double RingOuter = 2.5;

	// The second sun's orbit leans a little off the circle, one way for half of its slant and the other
	// way for the rest. No orbit moves: what changes from day to day is only where the bodies are on
	// them, each going round in a time of its own.
	inline constexpr double SecondSunTilt = 0.1;
	inline constexpr double PlanetSpinDays = 9.0;
	inline constexpr double PlanetWobble = 0.7;      // radians from the middle of its way round
	inline constexpr double PlanetWobbleDays = 29.0;

	inline Vec3 GetSkyAlong() { return { std::cos(SkyHeading), std::sin(SkyHeading), 0.0 }; }

	inline Vec3 GetSkyTop()
	{
		return { -std::sin(SkyHeading) * std::sin(SkyLean), std::cos(SkyHeading) * std::sin(SkyLean), std::cos(SkyLean) };
	}

	inline Vec3 GetSkyPole() { return Cross(GetSkyAlong(), GetSkyTop()); }

	// The sky is a sphere, very large, turning about an axis that runs SkyAxisDepth under the ground,
	// below the world's origin. The stars lie on it and the suns just inside it, and the directions
	// here are from its middle. From the ground, so far above that middle, they are seen lower, by
	// SeenFrom: about 23 degrees near the horizon, so the suns rise later and set sooner than their
	// circles say. The suns are drawn as wide as they always were, wherever they are seen.
	inline constexpr double SkyRadius = 1.0e8;       // metres
	inline constexpr double SunDistance = 9.9e7;     // from the sky's middle
	inline constexpr double SkyAxisDepth = 4.0e7;

	/** What the eye at Eye has to add to a direction from the sky's middle before normalising it, to see what lies Distance from that middle. */
	inline Vec3 GetSkyShift(const Vec3& Eye, double Distance = SkyRadius)
	{
		return (Vec3{ 0.0, 0.0, -SkyAxisDepth } - Eye) * (1.0 / Distance);
	}

	/** Where something Distance from the sky's middle, in a direction from that middle, is seen from Eye. */
	inline Vec3 SeenFrom(const Vec3& FromMiddle, const Vec3& Eye, double Distance = SkyRadius)
	{
		return Normalize(FromMiddle + GetSkyShift(Eye, Distance));
	}

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

	// The world's own reckoning of time. A day lasts DaySeconds of the seconds its physics runs in, twenty
	// minutes at the usual speed of time, and its clock cuts it into DayHours hours of HourMinutes minutes of
	// MinuteSeconds seconds, every one of them whole. The clock's seconds are its own: one second of the
	// physics is exactly one minute on the clock. Inside, the day is still counted in 24 parts, which the
	// suns' timetables are written in.
	inline constexpr double DaySeconds = 1200.0;
	inline constexpr int DayHours = 20;
	inline constexpr int HourMinutes = 60;
	inline constexpr int MinuteSeconds = 60;
	inline constexpr int DayClockSeconds = DayHours * HourMinutes * MinuteSeconds; // 72000, sixty to each second of the physics
	static_assert(DayClockSeconds % static_cast<int>(DaySeconds) == 0, "the clock's seconds go a whole number of times into the physics' own");

	/** The time of day as the world tells it. */
	struct ClockReading
	{
		int Hours = 0;
		int Minutes = 0;
		int Seconds = 0;
	};

	/** Reads the clock at Hour, in 24ths of a day. */
	inline ClockReading ReadClock(double Hour)
	{
		const int Total = std::clamp(static_cast<int>(std::floor(Hour / 24.0 * DayClockSeconds)), 0, DayClockSeconds - 1);
		return { Total / (HourMinutes * MinuteSeconds), Total / MinuteSeconds % HourMinutes, Total % MinuteSeconds };
	}

	/** One of the world's hours on from Hour, in 24ths of a day, onto the start of that hour, round the clock. */
	inline double StepClockHour(double Hour, int Step)
	{
		const double Hours = std::floor(Hour / 24.0 * DayHours + 0.001) + Step;
		return std::fmod(Hours + DayHours, static_cast<double>(DayHours)) * 24.0 / DayHours;
	}

	// The world has two suns. The yellow-orange one rises first and the blue-green one joins it from
	// the opposite side of the sky; the first then sets, the second after it, and night falls until
	// the first rises again. Day is a count that goes up by one at each midnight.

	inline Vec3 GetSunDirection(double Hour, double Day = 0.0)
	{
		(void)Day; // its orbit is the circle itself
		return GetOrbitPoint(GetPathAngle(Hour, 5.0, 18.0));
	}

	// Once in EclipseCycleDays both suns hide behind the planet at the same moment, and at no other
	// time, by their periods alone. All three orbits run through one point of the sky, where the first
	// sun is at SunsMeetHour every day. The second sun and the planet come back to it in times that go
	// a whole number of times into the cycle, 91 and 71 times, and as neither shares a factor with 90,
	// the three are there together only once in a cycle.
	inline constexpr double EclipseCycleDays = 90.0;
	inline constexpr double FirstEclipseDay = 3.0;
	inline constexpr double SunsMeetHour = 13.5;
	inline constexpr double SecondSunTurnHours = EclipseCycleDays * 24.0 / 91.0; // 23.74: it rises about 16 minutes earlier each day
	inline constexpr double PlanetTurnHours = EclipseCycleDays * 24.0 / 71.0;    // 30.42

	inline double GetEclipseDays() { return FirstEclipseDay + SunsMeetHour / 24.0; }

	/** Where on its circle the first sun is when the three meet: where the other two orbits cross it. */
	inline double GetMeetingAngle() { return GetPathAngle(SunsMeetHour, 5.0, 18.0); }

	/**
	 * Against the first sun's way round, on an orbit leaning off its circle and crossing it at the
	 * meeting point, once round in SecondSunTurnHours. So it keeps an hour of its own, which goes round
	 * the clock once in a cycle: in some weeks it lights the night. The suns run together in the days
	 * when they pass each other near the crossing.
	 */
	inline Vec3 GetSecondSunDirection(double Hour, double Day = 0.0)
	{
		const double Hours = (Day + Hour / 24.0 - GetEclipseDays()) * 24.0;
		return GetOrbitPoint(GetMeetingAngle() - 2.0 * Pi * Hours / SecondSunTurnHours, SecondSunTilt, GetMeetingAngle());
	}

	/** The suns as the ground sees them, from Eye or from the world's origin. */
	inline Vec3 GetSeenSun(double Hour, double Day = 0.0, const Vec3& Eye = {})
	{
		return SeenFrom(GetSunDirection(Hour, Day), Eye, SunDistance);
	}

	inline Vec3 GetSeenSecondSun(double Hour, double Day = 0.0, const Vec3& Eye = {})
	{
		return SeenFrom(GetSecondSunDirection(Hour, Day), Eye, SunDistance);
	}

	/** How much the suns are heating the ground: nothing at night, about 1 with both of them high. */
	inline double GetSunHeat(double Hour, double Day = 0.0)
	{
		const auto Height = [](const Vec3& Sun) { return std::max(Sun.Z, 0.0); };
		return Height(GetSeenSun(Hour, Day)) + Height(GetSeenSecondSun(Hour, Day)) * 0.6;
	}

	/**
	 * A ringed planet that shines by the suns' light. It goes its own way, owing nothing to the sky's
	 * turning or to the clock: round a great circle of its own, once in PlanetTurnHours, so it rises
	 * about 6 hours later each day, and some nights it is not up at all. Its circle runs through where
	 * the suns' orbits are seen to cross.
	 */
	inline Vec3 GetPlanetDirection(double Hour, double Day = 0.0)
	{
		const Vec3 Meeting = GetSeenSun(SunsMeetHour);
		const Vec3 Pole = Normalize(Cross(Meeting, Normalize(Vec3{ 0.25, 0.0, 1.0 }))); // its circle runs nearly overhead
		const Vec3 Onward = Cross(Pole, Meeting);
		const double Turned = 2.0 * Pi * (Day + Hour / 24.0 - GetEclipseDays()) * 24.0 / PlanetTurnHours;
		return Meeting * std::cos(Turned) + Onward * std::sin(Turned);
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

	/** How fast the suns' drops of light magic fly, in m/s: two thirds of the speed of light, always. */
	inline constexpr double LightDropSpeed = 2.0 / 3.0 * 299792458.0;

	/** Of the drops that strike the planet, the share it sends back towards the ground. */
	inline constexpr double PlanetDropShare = 0.05;

	/** Drops of light magic coming in from one way: the way they come from, and how many, as a share of a sun overhead's. */
	struct LightDropSource
	{
		Vec3 From;
		double Rate = 0.0;
	};

	/**
	 * The two suns' drops of light magic, and the few the planet sends back of those that strike it, as they
	 * reach the ground at Hour. They take about half a second from the suns, so what arrives left them that
	 * long before: an eclipse cuts them off that much later. A sun below the horizon sends none through the
	 * ground, and the planet hides those behind it as it does the light.
	 */
	inline std::array<LightDropSource, 3> GetLightDropSources(double Hour, double Day)
	{
		double Then = Hour - SunDistance / LightDropSpeed / DaySeconds * 24.0;
		double ThenDay = Day;
		if (Then < 0.0)
		{
			Then += 24.0;
			ThenDay -= 1.0;
		}
		const Vec3 Sun = GetSeenSun(Then, ThenDay);
		const Vec3 Second = GetSeenSecondSun(Then, ThenDay);
		const Vec3 Planet = GetPlanetDirection(Then, ThenDay);
		const Vec3 Axis = GetPlanetAxis(ThenDay + Then / 24.0);
		const auto Risen = [](const Vec3& Way)
		{
			const double Up = std::clamp((Way.Z + 0.02) / 0.05, 0.0, 1.0);
			return Up * Up * (3.0 - 2.0 * Up);
		};

		// The planet sends back the share of each sun's drops that strike the side of it turned to the ground.
		const double Struck = (0.5 - 0.5 * Dot(Planet, Sun)) + (0.5 - 0.5 * Dot(Planet, Second));
		return { {
			{ Sun, Risen(Sun) * GetLightPastPlanet(Sun, Planet, Axis, SunSize) },
			{ Second, Risen(Second) * GetLightPastPlanet(Second, Planet, Axis, SecondSunSize) },
			{ Planet, Risen(Planet) * PlanetDropShare * Struck } } };
	}
}
