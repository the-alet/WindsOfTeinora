// World generation and environmental forces, in SI units.
#pragma once

#include "Core/Math.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace Teinora::Sim
{
	inline constexpr std::uint32_t WorldSeed = 0x7E1A0A17u;
	inline constexpr double SeaLevel = 0.0;
	inline constexpr double Gravity = 9.81;
	inline constexpr double AirDensity = 1.225;
	inline constexpr double WaterDensity = 1000.0;

	enum class ESurface : std::uint8_t
	{
		Sand,
		Grass,
		Rock,
		Snow,
		Count
	};

	/** Coherent 2D gradient noise, roughly in [-1, 1]. */
	double GradientNoise(double X, double Y, std::uint32_t Seed);

	/** The same noise repeating every PeriodX by PeriodY lattice cells, for textures that tile. */
	double TiledNoise(double X, double Y, int PeriodX, int PeriodY, std::uint32_t Seed);

	/** Sum of Octaves noise layers, normalised back to roughly [-1, 1]. */
	double FractalNoise(double X, double Y, std::uint32_t Seed, int Octaves);

	/** What can be changed about the world while it runs, from the world menu. */
	struct WorldConditions
	{
		double WindSpeed = 3.0;    // m/s of the steady wind near the ground; gusts come on top
		double WindHeading = -1.0; // radians it blows towards; below zero it wanders by itself
		double WaveHeight = 1.0;   // share of the usual height of the sea's waves
	};

	WorldConditions GetConditions();
	void SetConditions(const WorldConditions& Conditions);

	/** Ground height at a world position, deterministic for a given seed. */
	double TerrainHeight(double X, double Y, std::uint32_t Seed);

	/** Unit ground normal, from central differences of TerrainHeight. */
	Vec3 TerrainNormal(double X, double Y, std::uint32_t Seed);

	/** A body of still water above the sea, in a basin the terrain has been shaped around. */
	struct Lake
	{
		double X = 0.0;
		double Y = 0.0;
		double Radius = 0.0;  // across its narrow way: the shore wanders in and out of it
		double Stretch = 1.0; // how many times longer it is than it is wide
		double Angle = 0.0;   // which way the long side lies
		double Level = 0.0;   // height of its surface
		double Depth = 0.0;   // at the deepest
		std::uint32_t Id = 0;

		/** All of it lies within this distance of its middle. */
		double GetReach() const { return Radius * Stretch * WaterEdge; }

		/** Whether a point lies within the shape that holds it. The water ends at the shore, always inside; the ground stands above Level at the edge. */
		bool Holds(double PointX, double PointY) const
		{
			const double OffX = PointX - X;
			const double OffY = PointY - Y;
			const double Along = (OffX * std::cos(Angle) + OffY * std::sin(Angle)) / Stretch;
			const double Across = OffY * std::cos(Angle) - OffX * std::sin(Angle);
			return Along * Along + Across * Across < Radius * Radius * WaterEdge * WaterEdge;
		}

		static constexpr double WaterEdge = 1.3; // also in Shaders/Water.hlsl
	};

	/** The lakes with any water within Distance of a point. */
	std::vector<Lake> FindLakes(double X, double Y, double Distance, std::uint32_t Seed);

	/** The water that a point of the map belongs to, whether or not the ground there is below it. */
	struct WaterBody
	{
		double Level = SeaLevel;
		bool bLake = false; // a lake lies still; only the sea has waves
	};

	WaterBody FindWater(double X, double Y, std::uint32_t Seed);

	/** Height of the water's surface at a point, waves included, or below any ground if there is no water here. */
	double WaterSurfaceAt(double X, double Y, double Time, std::uint32_t Seed);

	/** Picks a surface from height, the Z component of the unit normal and the level of the nearest water. */
	ESurface ClassifySurface(double Height, double NormalZ, double WaterLevel = SeaLevel);

	/** How the land varies from place to place: what grows there, and how much of it. */
	struct Biome
	{
		double Warmth = 0.5;   // from 0, the cold of the heights, to 1
		double Moisture = 0.5; // from 0, dry, to 1
		double Forest = 0.0;   // how thickly trees grow here, from 0 to 1
		double Conifers = 0.5; // the share of them that are conifers
	};

	Biome ReadBiome(double X, double Y, double Height, std::uint32_t Seed);

	/** Friction coefficient of a surface, used by the physics contacts. */
	double SurfaceFriction(ESurface Surface);

	/** How a surface gives under a blow and takes the energy out of it. */
	struct SurfaceGive
	{
		double Stiffness = 0.0;   // 1/s^2: the spring a body lands on, per unit of its mass; zero for rigid ground
		double Damping = 0.0;     // 1/s: what soaks up the blow
		double Restitution = 0.0; // of rigid ground
		double Rolling = 0.0;     // coefficient of rolling resistance
	};

	SurfaceGive GetSurfaceGive(ESurface Surface);

	/** Finds gentle dry ground, spiralling outwards from the origin. */
	Vec3 FindLandSpawn(std::uint32_t Seed);

	/** Picks the direction with the highest ground ahead, so the first view is of the land. */
	double FindInlandYaw(const Vec3& From, std::uint32_t Seed);

	/** The steady part of the wind, near the ground, which carries everything else along. */
	Vec3 PrevailingWind(double Time, std::uint32_t Seed);

	/** Air velocity without eddies: a drifting prevailing wind plus travelling gust fronts. */
	Vec3 WindAt(const Vec3& Position, double Time, std::uint32_t Seed);

	/** How air moves at a point. */
	struct AirFlow
	{
		Vec3 Velocity;
		Vec3 Vorticity; // curl of the velocity: twice the rate the air spins at
	};

	/**
	 * Turbulent eddies, as a sum of random Fourier modes (Kraichnan's synthetic turbulence).
	 * They have an rms speed of 1 m/s along each axis, no divergence, and Kolmogorov's k^-5/3
	 * energy spectrum between 160 m and 2 m. Drift is how far the wind has carried them, and
	 * modes finer than SmallestEddy metres are left out, for sampling on a coarse grid.
	 */
	AirFlow EddiesAt(const Vec3& Position, const Vec3& Drift, double Time, double SmallestEddy = 0.0);

	/** Rms speed of turbulence as a share of the wind speed, in the surface layer: 1 / ln(z / z0). */
	double TurbulenceIntensity(double HeightAboveGround, double Roughness);

	/** Wind plus turbulence as strong as the wind, the height and the ground below make it. */
	AirFlow AirFlowAt(const Vec3& Position, const Vec3& Drift, double Time, std::uint32_t Seed);

	/** Height of the sea at a point, with its waves. The same waves are computed in Shaders/Common.hlsli. */
	double WaterHeight(double X, double Y, double Time);

	/** Quadratic drag from air moving at RelativeAir with respect to the body. */
	Vec3 AeroForce(const Vec3& RelativeAir, double DragCoefficient, double FrontalArea, double Density = AirDensity);

	/** Turning moment on a body of about this radius, from air spinning at RelativeSpin around it. */
	Vec3 AeroTorque(const Vec3& RelativeSpin, double Radius, double Density = AirDensity);

	/** Archimedes lift from displaced air, which only matters for light, large bodies like balloons. */
	Vec3 AirLift(double Volume);

	inline constexpr double MaxThrowSpeed = 40.0;

	/** How fast a ball leaves the hand after the button was held this long. A quick tap only lets go of it. */
	double ThrowSpeedAfterHold(double Seconds);

	/**
	 * The flattest launch angle at Speed that reaches a point Distance away and Rise above the
	 * start, in still air. DragPerMass is 0.5 rho Cd A / m. Returns false if it is out of reach.
	 */
	bool SolveThrowAngle(double Distance, double Rise, double Speed, double DragPerMass, double NetGravity, double& OutAngle);

	/** Fraction of a body of the given half height that is below the water line. */
	double SubmergedFraction(double CenterZ, double HalfHeight, double WaterZ = SeaLevel);

	/** Archimedes lift plus linear water drag for a partly submerged body. */
	Vec3 WaterForce(double SubmergedShare, double Volume, const Vec3& Velocity, double DragPerSecond, double Mass);

	// Magic is a transparent matter that flows and swirls as air does, measured in kilograms.
	// In every formula that amount stands in for mass, and gravity does not act on it.

	/** Off until the behaviour of magic and how its kinds act on each other have been reworked. */
	inline constexpr bool bMagicEnabled = false;

	/** Kilograms of inertia per kilogram of magic, in case a constant turns out to be needed here. */
	inline constexpr double MagicInertia = 1.0;

	/** Density in kg/m3 at which magic starts to bend light, so that it shows without the magic view. */
	inline constexpr double MagicVisibleDensity = 5.5;

	/** The kinds of magic. They mix and flow together, and each one's amount is conserved on its own. */
	enum class EMagicType : std::uint8_t
	{
		Earth,  // brown; at home in the ground, so it lies thin over land and thinner away from it
		Water,  // blue; the same for the sea
		Air,    // green turquoise; at home in the open, thin everywhere, and the only kind that gets dense, around the wells
		Nature, // dark green; only close to plants, and very weak even there
		Count
	};

	inline constexpr int MagicTypeCount = static_cast<int>(EMagicType::Count);

	/** Density of each kind of magic at a point, in kg/m3. */
	struct MagicMix
	{
		double Amounts[MagicTypeCount] = {};

		double GetTotal() const
		{
			double Total = 0.0;
			for (const double Amount : Amounts)
			{
				Total += Amount;
			}
			return Total;
		}

		double Get(EMagicType Type) const { return Amounts[static_cast<int>(Type)]; }
	};

	/** A place the world wells up with air magic, in a dense cloud around Position. */
	struct MagicWell
	{
		Vec3 Position;
		double Radius = 0.0;
		double Peak = 0.0; // density at the centre, in kg/m3
	};

	/** The wells within a few kilometres of a point. */
	std::vector<MagicWell> FindMagicWells(double X, double Y, std::uint32_t Seed);

	/** What magic cares about in the column of the world over a point of the map. */
	struct MagicGround
	{
		double Surface = 0.0; // height of the land or the water
		double Land = 0.0;    // how much this counts as land rather than sea, from 0 to 1
		double Plants = 0.0;  // how much grows here, from 0 to 1
	};

	/** How much a ground height counts as land rather than sea, from 0 to 1. */
	double MagicLandShare(double GroundHeight);

	MagicGround ReadMagicGround(double X, double Y, std::uint32_t Seed);

	/**
	 * The most of a kind that can stay at a point, in kg/m3. Earth and water magic cannot be dense
	 * outside their element: 2 to 3 kg/m3 right over it at most, less above, and little away from
	 * it. Natural magic hardly exists away from plants, and is very weak beside them.
	 */
	double MagicLimit(EMagicType Type, double HeightAboveSurface, const MagicGround& Ground);

	/** The magic that the world restores at a point: thin layers over land and sea, plus the wells. */
	MagicMix MagicBaseline(const Vec3& Position, const std::vector<MagicWell>& Wells, const MagicGround& Ground, std::uint32_t Seed);
}
