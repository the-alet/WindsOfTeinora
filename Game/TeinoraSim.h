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

	/**
	 * The level that the ground at a point measures its height from, which decides where shores are:
	 * a lake's around that lake, fading to the sea's where its basin gives way to the land about it.
	 * FindWater cannot do this, since it says what is at a point and so jumps at the edge of a lake.
	 */
	double GetShoreLevel(double X, double Y, std::uint32_t Seed);

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
	//
	// All of it is one substance, neutral to begin with, that moves in streams through space and
	// through whatever is in the way. What it obeys:
	//  1. The amount of magic is conserved. An aspect is a property of magic, not a substance.
	//  2. Out of contact with matter an aspect fades back to neutral, each at its own rate.
	//  3. No aspect ever turns into another: the only way between them is through neutral.
	//  4. Only neutral magic takes on an aspect, only from matter of that aspect, and a kilogram
	//     has one aspect at a time.
	//  5. Air gives its aspect as slowly as it fades, so in the air the two lie about half and half.
	//  6. Matter slows the stream and holds it, so magic lies denser against the ground.
	//  7. Only magic with an aspect acts on matter, and matter acts back on it just as hard.
	//     Neutral magic passes through everything and pushes nothing.
	//  8. Fire is the one aspect with forms, and pressure alone moves it between them: from fire
	//     to lightning to light as it rises, and back as it falls. They are one aspect, not three.
	//  9. Fire magic warms the air it is in, and that is the only way heat gets about: there is no
	//     transfer of heat besides fire magic going somewhere. Air warmed swells and pushes outwards.
	//     Wind brings air magic to fire. Where there is more fire than that, it burns the air magic,
	//     which goes back to neutral, and the heat gives neutral magic the aspect of fire, as
	//     oxygen feeds a blaze. Where there is less, the wind blows it away and it slowly weakens.
	// 10. Water and air magic moving through each other rub. Rubbing is hot, so neutral magic beside
	//     it takes the aspect of fire, the one aspect that comes from something other than matter.
	//     And it squeezes: the energy of their motion through each other is a pressure, which counts
	//     in 8 like any other. So where they rub hard enough, what it makes is lightning.
	// 11. Lightning held in a cloud is a charge. Past a limit it breaks through the air to the
	//     ground along one channel, and what was charged goes back to neutral.
	// 12. Only dense fire changes matter. Where it reaches MagicFierceFire, in any of its forms, what
	//     grows catches, and water boils to steam, which spends the fire back to neutral.

	inline constexpr bool bMagicEnabled = true;

	/** Kilograms of inertia per kilogram of magic, in case a constant turns out to be needed here. */
	inline constexpr double MagicInertia = 1.0;

	/** How far the magic view sees, in metres. Also in Shaders/Magic.hlsl and Unreal/Scripts/make_content.py. */
	inline constexpr double MagicSightRadius = 100.0;

	/** Metres over the ground where magic gives out. There is magic everywhere below that. */
	inline constexpr double MagicCeiling = 2000.0;

	/** Density in kg/m3 at which magic starts to bend light, so that it shows without the magic view. */
	inline constexpr double MagicVisibleDensity = 5.5;

	/** How fast a pressure difference spreads through magic, in m/s. Its pressure is its density times this squared. */
	inline constexpr double MagicSoundSpeed = 5.0;

	/** Magic is mostly transparent to matter, so only this much of it counts for pushing on a body. */
	inline constexpr double MagicDragShare = 0.3;

	/** How much of the air's motion magic takes up. What vapour holds moves with the air, so the two pass each other. */
	inline constexpr double MagicWindShare = 0.3;

	/** Where clouds are. Also in Shaders/Clouds.hlsli. */
	inline constexpr double CloudBottom = 1700.0;
	inline constexpr double CloudTop = 3100.0;

	/** How much faster the wind is up where the clouds are than near the ground, which holds it back. */
	inline constexpr double CloudWindRatio = 2.5;

	/**
	 * How fast clouds change their shape, in turns of their noise per second: a little in still air,
	 * and the faster the harder the wind blows up there, per m/s of it.
	 */
	inline constexpr double CloudChangeStill = 1.0 / 1500.0;
	inline constexpr double CloudChangePerWind = 1.0 / 6000.0;

	/** How much lightning a cloud holds, in kg/m3, before it breaks through the air. */
	inline constexpr double MagicBreakdown = 0.08;

	/** What a bolt brings down to each cell of the gas along its channel, in kg/m3 and in kg. */
	inline constexpr double MagicChannelDensity = 30.0;
	inline constexpr double MagicChannelAmount = MagicChannelDensity * 64.0;

	/** What magic can be. It all flows as one: the aspect only says what a kilogram of it does. */
	enum class EMagicType : std::uint8_t
	{
		Earth,   // brown; from land and rock, which lie in it, so it fades away with height
		Water,   // blue; the same from the sea
		Air,     // green turquoise; from the air, slowly, so about half of what is up there has it
		Nature,  // dark green; from plants, and very weak even among them
		Fire,    // orange; from what is very hot, the more of it and the hotter the more
		Lightning, // violet; fire magic under pressure
		Light,   // white; fire magic under more pressure still
		Neutral, // what all magic is until something gives it an aspect
		Count
	};

	/** Pressures of magic in Pa, its density times its sound speed squared, that take fire to its other forms. */
	inline constexpr double MagicLightningPressure = 125.0; // 5 kg/m3
	inline constexpr double MagicLightPressure = 200.0;     // 8 kg/m3

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

		/** All of it that has an aspect, which is all that matter feels. */
		double GetAspected() const { return GetTotal() - Get(EMagicType::Neutral); }

		/** All of the one aspect of fire, whatever form it has. */
		double GetAllFire() const { return Get(EMagicType::Fire) + Get(EMagicType::Lightning) + Get(EMagicType::Light); }
	};

	/** Something hot enough to give magic the aspect of fire. */
	struct HotSpot
	{
		Vec3 Position;
		double Radius = 0.0;      // metres
		double Depth = 0.0;       // how far it goes down or up from there, in metres
		double Temperature = 0.0; // degrees Celsius
		bool bLava = false;       // or else a campfire

		double GetVolume() const { return 3.14159265358979 * Radius * Radius * Depth; }
	};

	/** The campfires and pools of lava within a few kilometres of a point, with the one by the spawn and those placed. */
	std::vector<HotSpot> FindHotSpots(double X, double Y, std::uint32_t Seed);

	/** Lights a campfire on the ground at a point, for as long as the game runs. */
	void PlaceCampfire(const Vec3& Position);

	/** Goes up by one each time a campfire is placed, so that what keeps hot spots knows to look again. */
	std::uint32_t GetPlacedVersion();

	/** How hot the top of the ground gets, in degrees Celsius. SunHeat is 0 at night and about 1 under both suns. */
	double GetGroundTemperature(double Warmth, ESurface Surface, double SunHeat);

	/** How fast neutral magic takes on the aspect of fire from something hot filling a share of where it is, per second. */
	double MagicHeatImprint(double Temperature, double VolumeShare);

	/** The same for all the hot spots around a cell of the given size. */
	double MagicHotImprint(const Vec3& Center, double CellSize, const std::vector<HotSpot>& Spots);

	/** Moves fire magic towards the form that a pressure in Pa gives it. Nothing else changes a form. */
	void UpdateFireForms(MagicMix& Mix, double Pressure, double Dt);

	/**
	 * The pressure of water and air magic passing through each other at Speed, in Pa: half of the
	 * pair's reduced density times the speed squared, which is the energy of that motion per volume.
	 */
	double MagicRubPressure(double WaterMagic, double AirMagic, double Speed);

	/** How fast neutral magic takes on the aspect of fire from that rubbing, per second. */
	double MagicRubImprint(double RubPressure);

	/** How fast neutral magic takes on the aspect of water from cloud of this density, from 0 to 1, per second. */
	double MagicVapourImprint(double Vapour);

	/** How many kg/m3 of magic cloud of this density holds to itself, on top of what the air has. */
	double MagicHeldByVapour(double Vapour);

	/** How fast the air rises through a cloud so many metres deep, in m/s. Only tall ones churn. */
	double CloudUpdraft(double Depth);

	/** What wind in m/s does to fire magic: more fire than the wind brings air magic grows on it, and less is blown away. */
	void UpdateFireInWind(MagicMix& Mix, double WindSpeed, double Dt);

	/** How many kg/m3 of fire magic, in all its forms together, it takes to light what grows or boil water. */
	inline constexpr double MagicFierceFire = 8.5;

	/** Spends fire magic on boiling water, back to neutral, if there is enough of it. Returns how many kg/m3 went. */
	double BoilWater(MagicMix& Mix, double Dt);

	/** Whether something grows here that can burn. */
	bool CanBurn(double X, double Y, std::uint32_t Seed);

	/** The push of air swelling out of where fire magic warms it, on a body of this volume. FireSlope is how that magic rises per metre. */
	Vec3 FireBlast(const Vec3& FireSlope, double Volume);

	/** How fast the air it has warmed rises, in m/s. */
	double FireUpdraft(double FireMagic);

	/** Seconds for an aspect to fall to about a third, out of contact with what gave it. */
	double MagicFadeTime(EMagicType Type);

	struct MagicGround;

	/** How fast neutral magic takes on each aspect at a height over the land or water, per second. */
	MagicMix MagicImprint(double HeightAboveSurface, const MagicGround& Ground);

	/** How much of the stream's speed is left at a height over the land or water, from 0 to 1. */
	double MagicSlowing(double HeightAboveSurface);

	/** Advances what neutral magic takes on and what aspects lose. Nothing else changes an aspect, and the total stays as it is. */
	void UpdateMagicAspects(MagicMix& Mix, const MagicMix& Imprint, double Dt);

	/** The stream of aspected magic against a body, as air would push on it. Relative is the magic's speed past it. */
	Vec3 MagicDrag(const MagicMix& Mix, const Vec3& Relative, double DragCoefficient, double FrontalArea);

	/** Water magic carrying a body along with it. */
	Vec3 MagicViscosity(double WaterMagic, const Vec3& Relative, double Radius);

	/** Air magic pushing a body from where it is dense to where it is thin. Slope is its rise per metre. */
	Vec3 MagicPressure(const Vec3& AirSlope, double Volume);

	/** Earth magic holding a body to the ground under it, which fades within a metre or two of it. */
	Vec3 MagicCling(double EarthMagic, double Clearance, const Vec3& GroundNormal, double Mass);

	/** What natural magic does to the stiffness of what grows, at least 1. */
	double MagicStiffening(double NatureMagic);

	/** What earth magic does to a body's resistance to rolling, at least 1. */
	double MagicGrip(double EarthMagic);

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
		bool bWater = false;  // which of those it is
		double Land = 0.0;    // how much this counts as land rather than sea, from 0 to 1
		double Plants = 0.0;  // how much grows here, from 0 to 1
		double Trees = 0.0;   // how thickly trees stand here, from 0 to 1
		double Cool = 0.0;    // how warm the ground is with no sun on it, in degrees Celsius
		double Sunned = 0.0;  // and how many degrees the suns add to that at most
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
