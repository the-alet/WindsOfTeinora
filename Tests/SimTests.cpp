// Plain assertions rather than a framework, so nothing extra has to be installed.
#include "Gaze.h"
#include "Liquid.h"
#include "MagicField.h"
#include "MagicShows.h"
#include "Plants.h"
#include "Render/SceneTypes.h"
#include "StarMap.h"
#include "Storms.h"
#include "SurfaceTextures.h"
#include "TeinoraSim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace Teinora;
using namespace Teinora::Sim;

namespace
{
	int Failures = 0;

	void Expect(bool bCondition, const char* What)
	{
		std::printf("[%s] %s\n", bCondition ? " ok " : "FAIL", What);
		Failures += bCondition ? 0 : 1;
	}

	void TestTerrain()
	{
		Expect(TerrainHeight(1234.5, -987.6, WorldSeed) == TerrainHeight(1234.5, -987.6, WorldSeed),
			"terrain is deterministic");
		Expect(TerrainHeight(1234.5, -987.6, WorldSeed) != TerrainHeight(1234.5, -987.6, WorldSeed + 1),
			"seed changes terrain");

		int Land = 0;
		int Sea = 0;
		int Peaks = 0;
		double MinHeight = 1e9;
		double MaxHeight = -1e9;
		double MaxStep = 0.0;
		double StepX = 0.0;
		double StepY = 0.0;
		constexpr double Extent = 30000.0;
		constexpr double Spacing = 150.0;
		for (double X = -Extent; X < Extent; X += Spacing)
		{
			for (double Y = -Extent; Y < Extent; Y += Spacing)
			{
				const double H = TerrainHeight(X, Y, WorldSeed);
				MinHeight = std::fmin(MinHeight, H);
				MaxHeight = std::fmax(MaxHeight, H);
				Land += H > SeaLevel ? 1 : 0;
				Sea += H <= SeaLevel ? 1 : 0;
				Peaks += H > 170.0 ? 1 : 0;
				// One grid cell of the finest terrain LOD.
				const double Step = std::fabs(TerrainHeight(X + 4.0, Y, WorldSeed) - H);
				if (Step > MaxStep)
				{
					MaxStep = Step;
					StepX = X;
					StepY = Y;
				}
			}
		}
		std::printf("       height %.1f..%.1f m, land %d, sea %d, snow %d, max step %.2f m at %.0f, %.0f\n",
			MinHeight, MaxHeight, Land, Sea, Peaks, MaxStep, StepX, StepY);
		Expect(Land > 0 && Sea > 0, "world has both land and sea");
		Expect(Land > Sea / 2, "land is not a rare exception");
		Expect(Peaks > 0, "mountains reach the snow line");
		Expect(MinHeight > -60.0 && MaxHeight < 600.0, "heights stay in the expected range");
		Expect(MaxStep < 45.0, "cliffs stay within what a chunk's mesh can show");

		// A cliff is one thing and a jump another: the steepest place found stays smooth when looked at closely.
		double Finest = 0.0;
		for (double Along = 0.0; Along < 4.0; Along += 0.125)
		{
			Finest = std::fmax(Finest, std::fabs(TerrainHeight(StepX + Along + 0.125, StepY, WorldSeed) - TerrainHeight(StepX + Along, StepY, WorldSeed)));
		}
		std::printf("       steepest eighth of a metre there: %.2f m\n", Finest);
		Expect(Finest < MaxStep * 0.2, "the terrain has no jumps in it");
		Expect(std::fabs(Length(TerrainNormal(50.0, 80.0, WorldSeed)) - 1.0) < 1e-9, "normals are unit length");
	}

	void TestLakes()
	{
		const std::vector<Lake> Lakes = FindLakes(0.0, 0.0, 9000.0, WorldSeed);
		std::printf("       %zu lakes within 9 km\n", Lakes.size());
		Expect(Lakes.size() >= 6, "there are lakes about");

		bool bBedBelow = true;
		bool bHeld = true;
		bool bAboveSea = true;
		bool bStill = true;
		bool bFound = true;
		double Deepest = 0.0;
		for (const Lake& Item : Lakes)
		{
			const double Bed = TerrainHeight(Item.X, Item.Y, WorldSeed);
			bBedBelow = bBedBelow && Bed < Item.Level - 0.5;
			bAboveSea = bAboveSea && Item.Level > SeaLevel + 4.0;
			Deepest = std::fmax(Deepest, Item.Level - Bed);

			const WaterBody Water = FindWater(Item.X, Item.Y, WorldSeed);
			bFound = bFound && Water.bLake && Water.Level == Item.Level;

			// A lake is calm: its surface is the same now and later, while the sea moves.
			bStill = bStill && WaterSurfaceAt(Item.X, Item.Y, 3.0, WorldSeed) == WaterSurfaceAt(Item.X, Item.Y, 7.0, WorldSeed);

			// All round the edge of the shape that holds the water, the ground stands above it.
			for (int Step = 0; Step < 48; ++Step)
			{
				const double Angle = 2.0 * Pi * Step / 48.0;
				const double Along = std::cos(Angle) * Item.Radius * Item.Stretch * Lake::WaterEdge * 0.999;
				const double Across = std::sin(Angle) * Item.Radius * Lake::WaterEdge * 0.999;
				const double X = Item.X + Along * std::cos(Item.Angle) - Across * std::sin(Item.Angle);
				const double Y = Item.Y + Along * std::sin(Item.Angle) + Across * std::cos(Item.Angle);
				bHeld = bHeld && TerrainHeight(X, Y, WorldSeed) >= Item.Level;
			}
		}
		std::printf("       deepest %.1f m\n", Deepest);
		Expect(bBedBelow, "each lake has a bed below its surface");
		Expect(bHeld, "and the ground holds its water in all round");
		Expect(bAboveSea, "and it stands clear of the sea");
		Expect(bStill, "a lake has no waves");
		Expect(bFound, "the water at a lake is that lake");
		Expect(WaterSurfaceAt(9000.0, 9000.0, 3.0, WorldSeed) != WaterSurfaceAt(9000.0, 9000.0, 7.0, WorldSeed) || FindWater(9000.0, 9000.0, WorldSeed).bLake, "while the sea does");
	}

	void TestBiomes()
	{
		double Coldest = 1.0;
		double Warmest = 0.0;
		int Woods = 0;
		int Open = 0;
		for (double X = -20000.0; X < 20000.0; X += 400.0)
		{
			for (double Y = -20000.0; Y < 20000.0; Y += 400.0)
			{
				const double Height = TerrainHeight(X, Y, WorldSeed);
				if (Height > SeaLevel)
				{
					const Biome Place = ReadBiome(X, Y, Height, WorldSeed);
					Coldest = std::fmin(Coldest, Place.Warmth);
					Warmest = std::fmax(Warmest, Place.Warmth);
					Woods += Place.Forest > 0.4 ? 1 : 0;
					Open += Place.Forest < 0.1 ? 1 : 0;
				}
			}
		}
		std::printf("       warmth %.2f..%.2f, %d in woods, %d in the open\n", Coldest, Warmest, Woods, Open);
		Expect(Warmest - Coldest > 0.5, "the land goes from cold to warm");
		Expect(Woods > 100 && Open > 100, "with woods and open country both");
	}

	void TestConditions()
	{
		const WorldConditions Usual = GetConditions();

		WorldConditions Still = Usual;
		Still.WindSpeed = 0.0;
		Still.WaveHeight = 0.0;
		SetConditions(Still);
		Expect(Length(WindAt({ 10.0, 20.0, 30.0 }, 5.0, WorldSeed)) == 0.0, "with the wind turned down there is none");
		Expect(WaterHeight(10.0, 20.0, 5.0) == SeaLevel, "and the sea can be stilled");

		WorldConditions Set = Usual;
		Set.WindSpeed = 10.0;
		Set.WindHeading = 0.0;
		SetConditions(Set);
		const Vec3 Steady = PrevailingWind(50.0, WorldSeed);
		Expect(Steady.X > 4.0 && std::fabs(Steady.Y) < 1e-9, "the wind blows the way it is set to, about as hard");

		SetConditions(Usual);
	}

	void TestPlants()
	{
		PlantDetail Detail;
		Detail.BranchLevels = 2;

		for (const EPlantKind Kind : { EPlantKind::Fir, EPlantKind::Pine, EPlantKind::Oak, EPlantKind::Birch, EPlantKind::Bush })
		{
			std::vector<TerrainVertex> Solid;
			std::vector<TerrainVertex> Leaves;
			std::vector<PlantLimb> Limbs;
			std::vector<PlantFoliage> Foliage;
			PlantOutput Output;
			Output.Solid = &Solid;
			Output.Leaves = &Leaves;
			Output.Limbs = &Limbs;
			Output.Foliage = &Foliage;
			GrowPlant(Kind, { 10.0, 20.0, 30.0 }, { 256.0, 512.0, 0.0 }, Kind == EPlantKind::Bush ? 2.0 : 16.0, Detail, 1234u, Output);
			std::printf("       plant %d: %zu solid and %zu leaf triangles, %zu limbs, %zu masses of leaves\n", static_cast<int>(Kind), Solid.size() / 3, Leaves.size() / 3, Limbs.size(), Foliage.size());
			Expect(!Solid.empty() && !Leaves.empty() && !Limbs.empty(), "a plant has wood, leaves and limbs to hit");
			Expect(Limbs.front().Root.X > 250.0 && Limbs.front().Root.Y > 500.0, "limbs are placed in the world");
		}

		// A ball crossing a limb near its tip, integrated as the physics would.
		const auto Fly = [](float Stiffness, float Mass, Vec3& OutBend, double& OutSwingBack)
		{
			constexpr double Dt = 1.0 / 120.0;
			std::vector<PlantLimb> Limbs(1);
			Limbs[0].Root = { 100.0, 100.0, 5.0 };
			Limbs[0].Tip = { 104.0, 100.0, 5.0 };
			Limbs[0].Radius = 0.06f;
			Limbs[0].Stiffness = Stiffness;
			Limbs[0].Mass = Mass;

			PlantField Field;
			Field.AddChunk(1u, std::move(Limbs), {}, { 0.0, 0.0, 0.0 }, 256.0);

			Vec3 Position{ 103.4, 95.0, 5.1 };
			Vec3 Velocity{ 0.0, 18.0, 0.0 };
			constexpr double BallMass = 8.0;
			std::vector<PlantMove> Moves;
			double Largest = 0.0;
			for (int Step = 0; Step < 90; ++Step)
			{
				Velocity += Field.Touch(Position, 0.35, Velocity, BallMass, Dt) * (Dt / BallMass);
				Position += Velocity * Dt;
				Field.Advance(Dt);
				Field.CollectMoves(Position, Moves, 1);
				Largest = Moves.empty() ? Largest : std::fmax(Largest, Length(Moves[0].Offset));
			}
			OutBend = Velocity;
			OutSwingBack = Largest;

			for (int Step = 0; Step < 1800; ++Step)
			{
				Field.Advance(Dt);
			}
			Field.CollectMoves(Position, Moves, 1);
			Expect(Moves.empty(), "a limb comes to rest again");
		};

		Vec3 PastTwig;
		Vec3 PastBough;
		double TwigBend = 0.0;
		double BoughBend = 0.0;
		Fly(120.0f, 0.8f, PastTwig, TwigBend);
		Fly(2400.0f, 12.0f, PastBough, BoughBend);
		std::printf("       ball at 18 m/s: leaves a twig at %.2f m/s, %.2f sideways, twig bends %.2f m; a bough at %.2f m/s, %.2f sideways, bends %.2f m\n",
			Length(PastTwig), std::hypot(PastTwig.X, PastTwig.Z), TwigBend, Length(PastBough), std::hypot(PastBough.X, PastBough.Z), BoughBend);
		Expect(Length(PastTwig) < 18.0 && Length(PastBough) < Length(PastTwig), "a limb takes speed from what hits it, a thick one more");
		Expect(std::hypot(PastBough.X, PastBough.Z) > 0.05 || PastBough.Y < 17.0, "and turns it from its path");
		Expect(TwigBend > BoughBend, "a thin limb gives way further");

		PlantField Leaves;
		PlantFoliage Crown;
		Crown.Center = { 100.0, 100.0, 5.0 };
		Crown.Radius = 3.0f;
		Crown.Density = 3.0f;
		Leaves.AddChunk(2u, {}, std::vector<PlantFoliage>{ Crown }, { 0.0, 0.0, 0.0 }, 256.0);
		const Vec3 Drag = Leaves.Touch({ 100.0, 100.0, 5.0 }, 0.35, { 20.0, 0.0, 0.0 }, 8.0, 1.0 / 120.0);
		Expect(Drag.X < 0.0, "a mass of leaves holds back what goes through it");
	}

	void TestLiquid()
	{
		const std::vector<Lake> Lakes = FindLakes(0.0, 0.0, 9000.0, WorldSeed);
		const Lake* Largest = &Lakes.front();
		for (const Lake& Item : Lakes)
		{
			Largest = Item.Radius > Largest->Radius ? &Item : Largest;
		}

		LiquidField Water(WorldSeed);
		Water.Follow({ Largest->X, Largest->Y, 0.0 });
		Expect(!Water.IsStirred() && Water.GetHeight(Largest->X, Largest->Y) == 0.0, "still water starts still");

		Water.Displace(Largest->X, Largest->Y, 0.2, 0.35);
		const double Put = Water.GetExtraVolume();
		const double Peak = Water.GetHeight(Largest->X, Largest->Y);
		Expect(std::fabs(Put - 0.2) < 1e-3, "displaced water is all accounted for");
		Expect(Peak > 0.01 && Water.IsStirred(), "and stands up over the surface");

		for (int Step = 0; Step < 30; ++Step)
		{
			Water.Advance(1.0 / 60.0);
		}
		const double Here = std::fabs(Water.GetHeight(Largest->X, Largest->Y));
		double Ring = 0.0;
		for (int Angle = 0; Angle < 32; ++Angle)
		{
			Ring = std::fmax(Ring, std::fabs(Water.GetHeight(Largest->X + std::cos(Angle * 0.196) * 2.5, Largest->Y + std::sin(Angle * 0.196) * 2.5)));
		}
		std::printf("       ripple: %.4f m at first, after half a second %.4f in the middle and %.4f m at 2.5 m; %.3f m3 left of 0.2\n", Peak, Here, Ring, Water.GetExtraVolume());
		Expect(Here < Peak * 0.5, "a heap of water falls");
		Expect(Ring > 1e-4, "and runs outwards as a ripple");
		Expect(Water.GetExtraVolume() > 0.1, "without water going missing");

		for (int Step = 0; Step < 1800; ++Step)
		{
			Water.Advance(1.0 / 60.0);
		}
		Expect(!Water.IsStirred(), "and in the end it lies still again");

		// On dry land there is nothing to stir.
		LiquidField Dry(WorldSeed);
		const Vec3 Spawn = FindLandSpawn(WorldSeed);
		Dry.Follow({ Spawn.X + 900.0, Spawn.Y, 0.0 });
		Dry.Displace(Spawn.X + 900.0, Spawn.Y, 0.2, 0.35);
		Expect(Dry.GetExtraVolume() < 0.2, "nothing lands on water where there is none");
	}

	void TestSpawn()
	{
		const Vec3 Spawn = FindLandSpawn(WorldSeed);
		Expect(Spawn.Z > SeaLevel + 3.0, "spawn is above the water line");
		Expect(TerrainNormal(Spawn.X, Spawn.Y, WorldSeed).Z > 0.95, "spawn is on gentle ground");
	}

	void TestSurfaces()
	{
		Expect(ClassifySurface(1.0, 1.0) == ESurface::Sand, "shore is sand");
		Expect(ClassifySurface(50.0, 1.0) == ESurface::Grass, "lowland is grass");
		Expect(ClassifySurface(50.0, 0.5) == ESurface::Rock, "cliffs are rock");
		Expect(ClassifySurface(250.0, 1.0) == ESurface::Snow, "peaks are snow");
		Expect(SurfaceFriction(ESurface::Snow) < SurfaceFriction(ESurface::Rock), "snow is slipperier than rock");
	}

	void TestWind()
	{
		double MinSpeed = 1e9;
		double MaxSpeed = 0.0;
		for (double T = 0.0; T < 3600.0; T += 2.0)
		{
			const double Speed = Length(WindAt({ 100.0, 200.0, 10.0 }, T, WorldSeed));
			MinSpeed = std::fmin(MinSpeed, Speed);
			MaxSpeed = std::fmax(MaxSpeed, Speed);
		}
		std::printf("       wind %.1f..%.1f m/s\n", MinSpeed, MaxSpeed);
		Expect(MinSpeed > 1.0, "there is always some wind");
		Expect(MaxSpeed > MinSpeed + 5.0, "gusts change wind speed");
		Expect(MaxSpeed < 60.0, "wind stays below extreme values");

		const Vec3 Ground = WindAt({ 0.0, 0.0, 0.0 }, 100.0, WorldSeed);
		const Vec3 High = WindAt({ 0.0, 0.0, 400.0 }, 100.0, WorldSeed);
		Expect(Length(High) > Length(Ground) * 1.9, "wind is stronger at altitude");
	}

	void TestTurbulence()
	{
		Vec3 Sum;
		double Squares = 0.0;
		int Count = 0;
		constexpr double Extent = 1500.0;
		for (double X = -Extent; X < Extent; X += 53.1)
		{
			for (double Y = -Extent; Y < Extent; Y += 53.1)
			{
				for (double Z = 0.0; Z < 800.0; Z += 61.7)
				{
					const Vec3 Velocity = EddiesAt({ X, Y, Z }, {}, 40.0).Velocity;
					Sum += Velocity;
					Squares += LengthSquared(Velocity);
					++Count;
				}
			}
		}
		const double Mean = Length(Sum * (1.0 / Count));
		const double Rms = std::sqrt(Squares / (3.0 * Count));
		std::printf("       eddies mean %.3f m/s, rms per axis %.3f m/s\n", Mean, Rms);
		Expect(Mean < 0.1, "eddies average out to nothing");
		Expect(std::fabs(Rms - 1.0) < 0.1, "eddies have unit rms speed along each axis");

		// Finite differences of the velocity field, against its divergence and the analytic curl.
		constexpr double Step = 0.002;
		const Vec3 Point{ 123.4, -56.7, 89.0 };
		const auto At = [](const Vec3& Where) { return EddiesAt(Where, { 3.0, 4.0, 0.0 }, 17.0).Velocity; };
		const Vec3 DX = (At(Point + Vec3{ Step, 0.0, 0.0 }) - At(Point - Vec3{ Step, 0.0, 0.0 })) * (0.5 / Step);
		const Vec3 DY = (At(Point + Vec3{ 0.0, Step, 0.0 }) - At(Point - Vec3{ 0.0, Step, 0.0 })) * (0.5 / Step);
		const Vec3 DZ = (At(Point + Vec3{ 0.0, 0.0, Step }) - At(Point - Vec3{ 0.0, 0.0, Step })) * (0.5 / Step);
		const Vec3 Curl{ DY.Z - DZ.Y, DZ.X - DX.Z, DX.Y - DY.X };
		const Vec3 Spin = EddiesAt(Point, { 3.0, 4.0, 0.0 }, 17.0).Vorticity;
		std::printf("       divergence %.2e, curl error %.2e of %.3f 1/s\n", DX.X + DY.Y + DZ.Z, Length(Curl - Spin), Length(Spin));
		Expect(std::fabs(DX.X + DY.Y + DZ.Z) < 1e-5, "eddies have no divergence, like incompressible air");
		Expect(Length(Curl - Spin) < 1e-4, "vorticity is the curl of the velocity");

		// Kolmogorov's law: squared velocity differences grow as r^(2/3) between the largest and
		// smallest eddies. Averaged over directions, since one direction sees few of the 64 waves.
		const auto Structure = [](double Separation)
		{
			const Vec3 Axes[] = { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 }, { 0.6, 0.8, 0.0 }, { 0.0, 0.6, 0.8 }, { 0.8, 0.0, 0.6 } };
			double Total = 0.0;
			int Pairs = 0;
			for (double X = 0.0; X < 2000.0; X += 57.3)
			{
				for (double Y = 0.0; Y < 2000.0; Y += 61.9)
				{
					const Vec3 Point{ X, Y, 100.0 };
					const Vec3 Here = EddiesAt(Point, {}, 0.0).Velocity;
					for (const Vec3& Axis : Axes)
					{
						Total += LengthSquared(EddiesAt(Point + Axis * Separation, {}, 0.0).Velocity - Here);
						++Pairs;
					}
				}
			}
			return Total / Pairs;
		};
		const double Slope = std::log(Structure(24.0) / Structure(6.0)) / std::log(4.0);
		std::printf("       structure function grows as r^%.2f, Kolmogorov r^0.67\n", Slope);
		Expect(std::fabs(Slope - 2.0 / 3.0) < 0.1, "eddy energy follows the 2/3 law");

		Expect(Length(EddiesAt({ 50.0, 60.0, 70.0 }, { 10.0, 0.0, 0.0 }, 5.0).Velocity - EddiesAt({ 40.0, 60.0, 70.0 }, {}, 5.0).Velocity) < 1e-9,
			"eddies are carried along by the wind");
		Expect(TurbulenceIntensity(2.0, 0.03) > TurbulenceIntensity(100.0, 0.03), "air is rougher near the ground");
		Expect(TurbulenceIntensity(10.0, 0.03) > TurbulenceIntensity(10.0, 0.0005), "land stirs the air more than water");

		const AirFlow Near = AirFlowAt({ 40.0, 40.0, 30.0 }, {}, 10.0, WorldSeed);
		Expect(Length(Near.Velocity - WindAt({ 40.0, 40.0, 30.0 }, 10.0, WorldSeed)) > 0.05, "turbulence is added to the wind");
	}

	void TestDayCycle()
	{
		const auto Up = [](const Vec3& Direction) { return Direction.Z > 0.0; };

		Expect(Up(GetSunDirection(7.0)) && !Up(GetSecondSunDirection(7.0)), "the morning has the first sun alone");
		Expect(Up(GetSunDirection(13.0)) && Up(GetSecondSunDirection(13.0)), "the second sun joins it");
		Expect(!Up(GetSunDirection(20.0)) && Up(GetSecondSunDirection(20.0)), "the first sun sets before the second");
		Expect(!Up(GetSunDirection(23.5)) && !Up(GetSecondSunDirection(23.5)) && !Up(GetSunDirection(2.0)), "then night falls");

		// From the ground, so far above the sky's middle, the suns are seen lower than their circles: up later, down sooner.
		Expect(Up(GetSunDirection(6.0)) && !Up(GetSeenSun(6.0)) && Up(GetSeenSun(8.0)), "the first sun is seen to rise after its circle comes over the horizon");
		Expect(Up(GetSunDirection(17.0)) && !Up(GetSeenSun(17.0)), "and to set before it goes under");
		Expect(std::fabs(GetSkyShift({}, SunDistance).Z) > 0.39, "the sky's middle lies far under the ground");

		// The planet goes its own way, once round in PlanetTurnHours whatever the clock and the sky are doing.
		std::vector<double> Rises;
		for (double Hour = 0.0; Hour < 96.0 && Rises.size() < 3; Hour += 0.01)
		{
			if (GetPlanetDirection(Hour).Z <= 0.0 && GetPlanetDirection(Hour + 0.01).Z > 0.0)
			{
				Rises.push_back(Hour);
			}
		}
		std::printf("       the planet rises at %.1f h, then %.1f h and %.1f h later\n", Rises.empty() ? -1.0 : Rises[0],
			Rises.size() > 1 ? Rises[1] - Rises[0] : -1.0, Rises.size() > 2 ? Rises[2] - Rises[1] : -1.0);
		Expect(Rises.size() == 3 && std::fabs(Rises[1] - Rises[0] - PlanetTurnHours) < 0.05 && std::fabs(Rises[2] - Rises[1] - PlanetTurnHours) < 0.05,
			"the planet keeps a turn of its own, rising about 6 hours later each day");

		// The second sun keeps a time of its own too, a little shorter than the day.
		const auto SecondRise = [](double Day)
		{
			for (double Hour = 0.0; Hour < 24.0; Hour += 0.005)
			{
				if (GetSecondSunDirection(Hour, Day).Z <= 0.0 && GetSecondSunDirection(Hour + 0.005, Day).Z > 0.0)
				{
					return Hour;
				}
			}
			return -1.0;
		};
		const double Earlier = (SecondRise(3.0) - SecondRise(4.0)) * 60.0;
		std::printf("       the second sun rises at %.2f h on day 3 and %.0f minutes earlier the next day\n", SecondRise(3.0), Earlier);
		Expect(Earlier > 14.0 && Earlier < 18.0, "the second sun rises about 16 minutes earlier each day");

		double Biggest = 0.0;
		for (double Hour = 0.0; Hour < 24.0; Hour += 0.05)
		{
			Biggest = std::fmax(Biggest, Length(GetSunDirection(Hour + 0.05) - GetSunDirection(Hour)));
			Biggest = std::fmax(Biggest, Length(GetSecondSunDirection(Hour + 0.05) - GetSecondSunDirection(Hour)));
			Biggest = std::fmax(Biggest, Length(GetPlanetDirection(Hour + 0.05) - GetPlanetDirection(Hour)));
		}
		Expect(Biggest < 0.1, "nothing jumps across the sky, midnight included");

		const Vec3 Going = GetSunDirection(12.1) - GetSunDirection(11.9);
		const Vec3 Coming = GetSecondSunDirection(12.1) - GetSecondSunDirection(11.9);
		Expect(Dot(Going, Coming) < 0.0, "the two suns cross the sky in opposite directions");
	}

	void TestSkyMeetings()
	{
		constexpr int Days = 2000;
		constexpr double Merged = SunSize + SecondSunSize; // the two discs touch
		int Merges = 0;
		int Eclipses = 0;
		int Partial = 0;
		std::vector<int> TripleDays;
		double Widest = 0.0;
		bool bOrderHolds = true;

		// As the ground sees them, which is what an eclipse is.
		for (int Day = 0; Day < Days; ++Day)
		{
			double Closest = Pi;
			double Darkest = 1.0;
			bool bTriple = false;
			for (double Hour = 0.0; Hour < 24.0; Hour += 1.0 / 120.0)
			{
				const Vec3 Sun = GetSeenSun(Hour, Day);
				const Vec3 Second = GetSeenSecondSun(Hour, Day);
				const Vec3 Planet = GetPlanetDirection(Hour, Day);
				const Vec3 Axis = GetPlanetAxis(Day + Hour / 24.0);
				if (Sun.Z > 0.0 && Second.Z > 0.0)
				{
					Closest = std::fmin(Closest, std::acos(std::fmin(Dot(Sun, Second), 1.0)));
				}
				const double PastSecond = Second.Z > 0.0 ? GetLightPastPlanet(Second, Planet, Axis, SecondSunSize) : 1.0;
				const double PastFirst = Sun.Z > 0.0 ? GetLightPastPlanet(Sun, Planet, Axis, SunSize) : 1.0;
				Darkest = std::fmin(Darkest, std::fmin(PastFirst, PastSecond));
				bTriple = bTriple || (PastFirst < 0.05 && PastSecond < 0.05);
				Widest = std::fmax(Widest, std::asin(std::fabs(Dot(GetSecondSunDirection(Hour, Day), GetSkyPole())))); // how far off the first sun's circle
			}
			if (Day < 40 && (Closest < Merged || Darkest < 0.05))
			{
				std::printf("       day %d:%s%s%s\n", Day, Closest < Merged ? " suns merge" : "", Darkest < 0.05 ? " planet hides a sun" : "", bTriple ? " and both" : "");
			}
			Merges += Closest < Merged ? 1 : 0;
			Eclipses += Darkest < 0.05 ? 1 : 0;
			Partial += Darkest < 0.95 ? 1 : 0;
			if (bTriple)
			{
				TripleDays.push_back(Day);
			}

			// The first sun keeps the clock's day whatever the date.
			bOrderHolds = bOrderHolds && GetSunDirection(7.0, Day).Z > 0.0 && GetSunDirection(20.0, Day).Z < 0.0 && GetSunDirection(2.0, Day).Z < 0.0;
		}

		const double MergeShare = static_cast<double>(Merges) / Days;
		const double EclipseShare = static_cast<double>(Eclipses) / Days;
		std::printf("       suns merge on %.1f%% of days, the planet hides a sun on %.1f%% and touches one on %.1f%%; orbit leans up to %.3f rad\n",
			MergeShare * 100.0, EclipseShare * 100.0, Partial * 100.0 / Days, Widest);
		std::printf("       both suns behind the planet on %zu days:", TripleDays.size());
		for (std::size_t Index = 0; Index < std::min<std::size_t>(TripleDays.size(), 6); ++Index)
		{
			std::printf(" %d", TripleDays[Index]);
		}
		std::printf("\n");
		Expect(MergeShare > 0.04 && MergeShare < 0.3, "the suns run together on some days, not all");
		Expect(EclipseShare > 0.005 && EclipseShare < 0.25, "the planet eclipses a sun on some days, not all");
		bool bEveryCycle = !TripleDays.empty() && TripleDays.front() == static_cast<int>(FirstEclipseDay);
		for (const int Day : TripleDays)
		{
			bEveryCycle = bEveryCycle && std::fmod(Day - FirstEclipseDay, EclipseCycleDays) == 0.0;
		}
		Expect(bEveryCycle && TripleDays.size() == static_cast<std::size_t>((Days - FirstEclipseDay) / EclipseCycleDays) + 1,
			"both suns hide behind the planet together once in each cycle of 90 days, and on no other day");
		const double Eclipse = SunsMeetHour;
		const Vec3 Covering = GetPlanetDirection(Eclipse, FirstEclipseDay + EclipseCycleDays * 3.0);
		const Vec3 CoverAxis = GetPlanetAxis(FirstEclipseDay + EclipseCycleDays * 3.0 + Eclipse / 24.0);
		Expect(GetLightPastPlanet(GetSeenSun(Eclipse, FirstEclipseDay + EclipseCycleDays * 3.0), Covering, CoverAxis, SunSize) < 0.01
			&& GetLightPastPlanet(GetSeenSecondSun(Eclipse, FirstEclipseDay + EclipseCycleDays * 3.0), Covering, CoverAxis, SecondSunSize) < 0.01,
			"at the height of it neither sun sends any light");
		Expect(Widest > 0.05 && Widest < 0.2, "the orbits lie only slightly at an angle");
		Expect(bOrderHolds, "the first sun keeps the clock's day whatever the date");

		const Vec3 Planet = GetPlanetDirection(16.0, 3.0);
		const Vec3 Axis = GetPlanetAxis(3.0);
		Expect(GetLightPastPlanet(Planet, Planet, Axis, SecondSunSize) < 1e-9, "a sun right behind the planet sends no light");
		Expect(GetLightPastPlanet(GetSunDirection(9.0), GetPlanetDirection(21.0), Axis, SunSize) == 1.0, "one far from it is untouched");

		const double DayTurn = GetPlanetSpin(11.0) - GetPlanetSpin(10.0);
		Expect(DayTurn > 0.35 && DayTurn < 1.0, "the planet shows a new side each day, but only a part of a turn");

		// How wide the rings stand open each time the planet comes back to the same point of its circle: 1 is face on, 0 edge on.
		for (const double Share : { 0.1, 0.25, 0.4 })
		{
			double Narrowest = 1.0;
			double Widest2 = 0.0;
			double MostInATurn = 0.0;
			double Last = -1.0;
			for (int Turn = 0; Turn < 100; ++Turn)
			{
				const double Hours = (Turn + Share) * PlanetTurnHours;
				const double Open = std::fabs(Dot(GetPlanetAxis(Hours / 24.0), GetPlanetDirection(Hours)));
				Narrowest = std::fmin(Narrowest, Open);
				Widest2 = std::fmax(Widest2, Open);
				MostInATurn = Last >= 0.0 ? std::fmax(MostInATurn, std::fabs(Open - Last)) : 0.0;
				Last = Open;
			}
			std::printf("       rings %.0f%% of the way round stand open from %.2f to %.2f, changing by up to %.2f a turn\n", Share * 100.0, Narrowest, Widest2, MostInATurn);
			Expect(Widest2 - Narrowest > 0.25, "the rings open and close over the days");
			Expect(MostInATurn < 0.25, "but slowly");
		}
	}

	void TestStarMap()
	{
		const std::vector<SkyStar> Stars = MakeStarMap(WorldSeed);
		Expect(static_cast<int>(Stars.size()) == StarMapCount, "the map holds all its stars");

		const std::vector<SkyStar> Again = MakeStarMap(WorldSeed);
		Expect(std::memcmp(Stars.data(), Again.data(), Stars.size() * sizeof(SkyStar)) == 0, "the same seed draws the same sky");
		const std::vector<SkyStar> Other = MakeStarMap(WorldSeed + 1);
		Expect(std::memcmp(Stars.data(), Other.data(), sizeof(SkyStar)) != 0, "another seed draws another");

		const auto Count = [&Stars](double Magnitude)
		{
			int Result = 0;
			for (const SkyStar& Star : Stars)
			{
				Result += GetStarMagnitude(Star.Flux) < Magnitude ? 1 : 0;
			}
			return Result;
		};
		std::printf("       stars below magnitude 1, 3, 4.5, 6: %d, %d, %d, %d\n", Count(1.0), Count(3.0), Count(4.5), Count(6.0));
		Expect(Count(1.0) > 10 && Count(1.0) < 24, "about as many bright stars as the real sky has");
		Expect(Count(3.0) > 120 && Count(3.0) < 200, "and middling ones");
		Expect(Count(6.0) > 4000 && Count(6.0) < 6000, "and about 5000 down to the limit of the eye");

		int North = 0;
		int Facing = 0;
		double Worst = 0.0;
		for (const SkyStar& Star : Stars)
		{
			const Vec3 Direction{ Star.Direction.X, Star.Direction.Y, Star.Direction.Z };
			Worst = std::fmax(Worst, std::fabs(Length(Direction) - 1.0));
			North += Direction.Z > 0.0 ? 1 : 0;
			Facing += Direction.X > 0.0 ? 1 : 0;
		}
		Expect(Worst < 1e-5, "every star lies on the sphere");
		Expect(std::abs(North - StarMapCount / 2) < 800 && std::abs(Facing - StarMapCount / 2) < 800, "they cover the sphere evenly, with no band across it");

		const StarSheet Sheet = MakeStarSheet(Stars);
		std::printf("       stars placed on the sheet: %d of %d\n", Sheet.Placed, StarMapCount);
		Expect(Sheet.Placed > StarMapCount * 99 / 100, "nearly all stars find a place on it");

		double WorstPlace = 0.0;
		double WorstStops = 0.0;
		int BrightFound = 0;
		for (const SkyStar& Star : Stars)
		{
			const Vec3 Direction{ Star.Direction.X, Star.Direction.Y, Star.Direction.Z };
			double U = 0.0;
			double V = 0.0;
			FoldDirection(Direction, U, V);
			Worst = std::fmax(Worst, Length(UnfoldDirection(U, V) - Direction));

			// Read it back the way the shader does.
			const int X = std::min(static_cast<int>(U * StarCellSide), StarCellSide - 1);
			const int Y = std::min(static_cast<int>(V * StarCellSide), StarCellSide - 1);
			const std::uint8_t* Cell = &Sheet.Pixels[(static_cast<std::size_t>(Y) * StarCellSide + X) * StarsPerCell * 8];
			for (int Slot = 0; Slot < StarsPerCell; ++Slot, Cell += 8)
			{
				const Vec3 Found = UnfoldDirection((X + Cell[0] / 255.0) / StarCellSide, (Y + Cell[1] / 255.0) / StarCellSide);
				if (Cell[2] != 0 && Cell[4] == Star.Color[0] && Cell[7] == Star.Color[3] && Length(Found - Direction) < 1e-4)
				{
					WorstPlace = std::fmax(WorstPlace, Length(Found - Direction));
					WorstStops = std::fmax(WorstStops, std::fabs((Cell[2] - 1.0) / 254.0 * StarFluxStops - std::log2(static_cast<double>(Star.Flux))));
					BrightFound += GetStarMagnitude(Star.Flux) < 5.0 ? 1 : 0;
					break;
				}
			}
		}
		Expect(Worst < 1e-5, "folding a direction out and back changes nothing");
		Expect(WorstPlace < 3e-5 && WorstStops < 0.05, "a star keeps its place and brightness on the sheet");
		Expect(BrightFound == Count(5.0), "and none bright enough to notice is lost");
	}

	void TestThrowHold()
	{
		Expect(ThrowSpeedAfterHold(0.0) == 0.0 && ThrowSpeedAfterHold(0.1) == 0.0, "a quick tap only drops the ball");
		Expect(ThrowSpeedAfterHold(0.5) > 0.0 && ThrowSpeedAfterHold(1.0) > ThrowSpeedAfterHold(0.5), "holding longer throws harder");
		Expect(ThrowSpeedAfterHold(9.0) == MaxThrowSpeed, "the strength of a throw has a limit");
	}

	void TestMagic()
	{
		const Vec3 Center{ 0.0, 0.0, 0.0 };
		const auto Hump = [&Center](const Vec3& Position)
		{
			return 0.5 + 20.0 * std::exp(-LengthSquared(Position - Center) / (2.0 * 30.0 * 30.0));
		};

		// Shut in walls: the only thing that can change the amount of magic is a mistake.
		MagicField::Options Closed;
		Closed.bOpen = false;
		Closed.Threads = 4;
		MagicField Box(WorldSeed, Closed);
		Box.SetDensity(EMagicType::Nature, Hump);
		Box.SetVelocity([](const Vec3& Position) { return Vec3{ -Position.Y * 0.1, Position.X * 0.1, 0.0 }; });

		const double Start = Box.GetTotalAmount();
		double Lowest = 1e9;
		for (int Step = 0; Step < 200; ++Step)
		{
			Box.Step(Center, {}, Step * 0.05, 0.05);
		}
		for (double X = -180.0; X < 180.0; X += 8.0)
		{
			for (double Y = -180.0; Y < 180.0; Y += 8.0)
			{
				Lowest = std::fmin(Lowest, Box.GetDensity({ X, Y, 0.0 }));
			}
		}
		const double End = Box.GetTotalAmount();
		std::printf("       %.0f kg of magic became %.0f kg, thinnest %.3f kg/m3, peak %.2f, spin %.3f 1/s\n", Start, End, Lowest, Box.GetDensity(Center), Box.GetPeakSpin());
		Expect(std::fabs(End - Start) < Start * 1e-4, "the amount of magic is conserved, whatever aspect it has");
		Expect(Lowest >= 0.0, "density never goes negative");
		Expect(Box.GetDensity(Center) < 20.0, "pressure spreads a dense cloud out");
		Expect(Box.GetPeakSpin() > 0.01, "the gas keeps swirling");

		// A flat layer of gas with weight would sink. Magic has none, so nothing moves it.
		MagicField Layer(WorldSeed, Closed);
		Layer.SetDensity(EMagicType::Earth, [](const Vec3&) { return 3.0; });
		for (int Step = 0; Step < 100; ++Step)
		{
			Layer.Step(Center, {}, Step * 0.05, 0.05);
		}
		const double Fallen = Length(Layer.GetVelocity({ 10.0, 10.0, 10.0 }));
		std::printf("       after 5 s at rest, magic moves at %.6f m/s\n", Fallen);
		Expect(Fallen < 1e-3, "gravity does not act on magic");
		Expect(std::fabs(Layer.GetDensity({ 0.0, 0.0, 90.0 }) - Layer.GetDensity({ 0.0, 0.0, -90.0 })) < 1e-3, "magic does not settle downwards");

		// Twice the amount in a uniform push gets the same acceleration, as with any mass.
		MagicField Thin(WorldSeed, Closed);
		MagicField Dense(WorldSeed, Closed);
		const auto Slope = [](double Base) { return [Base](const Vec3& Position) { return Base * (1.0 + Position.X * 0.002); }; };
		Thin.SetDensity(EMagicType::Nature, Slope(1.0));
		Dense.SetDensity(EMagicType::Nature, Slope(2.0));
		Thin.Step(Center, {}, 0.0, 0.05);
		Dense.Step(Center, {}, 0.0, 0.05);
		const double ThinSpeed = Thin.GetVelocity({ 20.0, 0.0, 0.0 }).X;
		const double DenseSpeed = Dense.GetVelocity({ 20.0, 0.0, 0.0 }).X;
		std::printf("       pressure gives %.4f and %.4f m/s in a step, predicted %.4f\n", ThinSpeed, DenseSpeed, -5.0 * 5.0 * 0.002 / (1.0 + 20.0 * 0.002) * 0.05);
		Expect(ThinSpeed < 0.0 && std::fabs(ThinSpeed - DenseSpeed) < 1e-4, "acceleration is pressure force over the amount of magic");

		// Earth magic against water magic with the same total density: no pressure difference, so
		// diffusion alone mixes them. The profile across the border should be an error function.
		MagicField Mixing(WorldSeed, Closed);
		Mixing.SetDensity(EMagicType::Earth, [](const Vec3& Position) { return Position.X < 0.0 ? 2.0 : 0.0; });
		Mixing.SetDensity(EMagicType::Water, [](const Vec3& Position) { return Position.X < 0.0 ? 0.0 : 2.0; });
		for (int Step = 0; Step < 200; ++Step)
		{
			Mixing.Step(Center, {}, Step * 0.05, 0.05);
		}
		const double Soaked = Mixing.GetDensity({ 6.0, 2.0, 2.0 }, EMagicType::Earth);
		const double EarthSoaking = 3.0 / MagicInertness(EMagicType::Earth); // m2/s, the slowest of all
		const double Predicted = std::erfc(6.0 / (2.0 * std::sqrt(EarthSoaking * 10.0))) * std::exp(-10.0 / MagicFadeTime(EMagicType::Earth));
		std::printf("       after 10 s, earth magic 6 m into the water magic: %.3f kg/m3, predicted %.3f\n", Soaked, Predicted);
		Expect(std::fabs(Soaked - Predicted) < 0.03, "kinds of magic diffuse into each other at the set rate");
		Expect(Mixing.GetDensity({ 6.0, 2.0, 2.0 }) > 1.99 && Mixing.GetDensity({ 6.0, 2.0, 2.0 }) < 2.01, "mixing leaves the total density alone");

		// The axioms, on one point of space at a time.
		const auto Only = [](EMagicType Type, double Amount)
		{
			MagicMix Mix;
			Mix.Amounts[static_cast<int>(Type)] = Amount;
			return Mix;
		};

		MagicMix Fading = Only(EMagicType::Earth, 2.0);
		for (int Step = 0; Step < 100; ++Step)
		{
			UpdateMagicAspects(Fading, {}, 4.2);
		}
		Expect(std::fabs(Fading.Get(EMagicType::Earth) - 2.0 * std::exp(-1.0)) < 1e-6, "out of contact an aspect fades at its own rate");
		Expect(std::fabs(Fading.Get(EMagicType::Neutral) - 2.0 * (1.0 - std::exp(-1.0))) < 1e-6, "and what it loses comes back as neutral magic");
		Expect(Fading.Get(EMagicType::Water) == 0.0 && Fading.Get(EMagicType::Air) == 0.0, "with nothing to give another aspect, it stays neutral");

		MagicGround Dry;
		Dry.Land = 1.0;
		MagicMix Bare = Only(EMagicType::Water, 1.0);
		UpdateMagicAspects(Bare, MagicImprint(-2.0, Dry), 0.05);
		Expect(Bare.Get(EMagicType::Earth) == 0.0, "an aspect never turns straight into another: only neutral magic takes one");

		MagicMix Open_ = Only(EMagicType::Neutral, 1.0);
		const MagicMix InTheAir = MagicImprint(500.0, Dry);
		for (int Step = 0; Step < 4000; ++Step)
		{
			UpdateMagicAspects(Open_, InTheAir, 0.1);
		}
		std::printf("       left in the air, magic settles to %.3f air and %.3f neutral\n", Open_.Get(EMagicType::Air), Open_.Get(EMagicType::Neutral));
		Expect(std::fabs(Open_.Get(EMagicType::Air) / Open_.Get(EMagicType::Neutral) - 1.0) < 0.01, "in the air, air magic and neutral magic mix about evenly");

		MagicMix Everything;
		for (double& Amount : Everything.Amounts)
		{
			Amount = 0.7;
		}
		const double TotalBefore = Everything.GetTotal();
		UpdateMagicAspects(Everything, MagicImprint(1.0, Dry), 7.0);
		Expect(std::fabs(Everything.GetTotal() - TotalBefore) < 1e-12, "aspects coming and going change no amount");
		for (const double Amount : Everything.Amounts)
		{
			Expect(Amount >= 0.0, "and nothing goes below zero");
		}

		// Only magic with an aspect acts on matter.
		const Vec3 Past{ 20.0, 0.0, 0.0 };
		Expect(LengthSquared(MagicDrag(Only(EMagicType::Neutral, 10.0), Past, 0.47, 0.38)) == 0.0, "neutral magic pushes nothing");
		Expect(MagicDrag(Only(EMagicType::Earth, 2.5), Past, 0.47, 0.38).X > 0.0, "the stream of aspected magic carries a body along");
		const double Thick = MagicViscosity(2.5, Past, 0.35).X;
		std::printf("       in 2.5 kg/m3 of water magic a ball of 8 kg takes %.1f s to be carried along\n", 8.1 * Past.X / Thick);
		Expect(Thick > 0.0 && std::fabs(8.1 * Past.X / Thick - 4.9) < 0.1, "water magic carries a ball along in some seconds");
		Expect(MagicPressure({ 0.2, 0.0, 0.0 }, 1.0).X < 0.0, "air magic pushes from dense to thin");
		Expect(MagicCling(3.0, 0.0, { 0.0, 0.0, 1.0 }, 8.0).Z < 0.0, "earth magic holds a body to the ground");
		Expect(LengthSquared(MagicCling(3.0, 30.0, { 0.0, 0.0, 1.0 }, 8.0)) < 1e-6, "but only right beside it");
		Expect(MagicStiffening(0.14) > 1.0 && MagicStiffening(0.14) < 2.0 && MagicStiffening(0.0) == 1.0, "natural magic stiffens what grows, a little");

		// Fire: what is hot gives it, the more of it and the hotter, and nothing else does.
		Expect(MagicHeatImprint(30.0, 1.0) == 0.0, "nothing cool gives the aspect of fire");
		Expect(MagicHeatImprint(1000.0, 1.0) > MagicHeatImprint(500.0, 1.0), "the hotter, the more of it");
		Expect(MagicHeatImprint(1000.0, 0.5) > MagicHeatImprint(1000.0, 0.05), "and the bigger, the more of it");
		// Heat gathers fire magic: densest in the fire, thinner further out, and further round bigger fires.
		const HotSpot Pool{ { 0.0, 0.0, 0.0 }, 12.0, 3.0, 1100.0, true };
		const HotSpot Camp{ { 0.0, 0.0, 0.0 }, 0.7, 0.8, 900.0, false };
		const auto CampAt = [&Camp](double Away) { return MagicHotDraw({ Away, 0.0, 0.5 }, Camp).Density; };
		std::printf("       a campfire's heat gathers %.1f kg/m3 of fire magic in it, %.1f at 4 m, %.2f at 8 m, %.3f at 12 m\n", CampAt(0.0), CampAt(4.0), CampAt(8.0), CampAt(12.0));
		Expect(CampAt(0.0) > CampAt(4.0) && CampAt(4.0) > CampAt(8.0) && CampAt(8.0) > CampAt(12.0) && CampAt(12.0) > 0.0, "a fire's magic is densest in it and thinner further out");
		Expect(CampAt(0.0) * MagicSoundSpeed * MagicSoundSpeed < MagicLightningPressure && CampAt(0.0) < MagicFierceFire, "a campfire's magic stays fire, and does not set what is around it alight");
		Expect(MagicHotDraw({ 20.0, 0.0, 0.0 }, Pool).Density > MagicHotDraw({ 20.0, 0.0, 0.0 }, Camp).Density * 10.0, "a lake of lava reaches far further");
		Expect(MagicHotDraw({ MagicHotReach(Camp), 0.0, 0.0 }, Camp).Density < 0.01 * CampAt(0.0), "and nothing reaches past where it is reckoned");
		const HotSpot Cold{ { 0.0, 0.0, 0.0 }, 0.7, 0.8, 30.0, false };
		Expect(MagicHotDraw({}, Cold).Density == 0.0, "nothing cool draws any");

		// Nature magic takes up light magic and grows by all of it, and water magic more slowly, growing by less.
		MagicMix Feeding;
		Feeding.Amounts[static_cast<int>(EMagicType::Nature)] = 0.5;
		Feeding.Amounts[static_cast<int>(EMagicType::Light)] = 1.0;
		Feeding.Amounts[static_cast<int>(EMagicType::Water)] = 1.0;
		const double FedTotal = Feeding.GetTotal();
		UpdateNatureFeeding(Feeding, 1.0);
		const double LightTaken = 1.0 - Feeding.Get(EMagicType::Light);
		const double WaterTaken = 1.0 - Feeding.Get(EMagicType::Water);
		std::printf("       in a second 0.5 kg/m3 of nature magic takes up %.3f of light magic and %.4f of water magic, and grows to %.3f\n", LightTaken, WaterTaken, Feeding.Get(EMagicType::Nature));
		Expect(LightTaken > 0.3 && WaterTaken > 0.0 && WaterTaken < 0.2 * LightTaken, "nature magic takes up light magic, and water magic far more slowly");
		Expect(Feeding.Get(EMagicType::Nature) > 0.5 + LightTaken && Feeding.Get(EMagicType::Nature) < 0.5 + LightTaken + WaterTaken, "it grows by all of the light and by less of the water");
		Expect(std::fabs(Feeding.GetTotal() - FedTotal) < 1e-12, "and changes no amount of magic");

		// The suns' drops come by day, and none during the triple eclipse, when the planet hides both suns and turns its dark side here.
		const auto Drops = [](double Hour, double Day)
		{
			double Sum = 0.0;
			for (const LightDropSource& Each : GetLightDropSources(Hour, Day))
			{
				Sum += Each.Rate;
			}
			return Sum;
		};
		std::printf("       drops of light magic at noon %.2f, at midnight %.2f, in the triple eclipse %.3f\n", Drops(12.0, 1.0), Drops(0.5, 1.0), Drops(SunsMeetHour, FirstEclipseDay));
		Expect(Drops(12.0, 1.0) > 0.5 && Drops(SunsMeetHour, FirstEclipseDay) < 0.05, "the suns pelt the ground by day, and the planet stops it in the triple eclipse");
		Expect(GetLightDropSources(12.0, 1.0)[2].Rate <= PlanetDropShare * 2.0, "the planet sends back only a few of the drops");

		// Everything holds magic inside itself, kept up by the magic around it, and falls apart without it.
		const auto Holding = [](double Around, double Fire, double Seconds)
		{
			double Held = MatterHolds;
			for (double Time = 0.0; Time < Seconds; Time += 0.1)
			{
				Held = UpdateHeldMagic(Held, Around, Fire, 0.1);
			}
			return Held;
		};
		Expect(std::fabs(Holding(0.7, 0.0, 300.0) - MatterHolds) < 1e-9 && HoldsTogether(Holding(0.7, 0.0, 300.0)), "where there is magic about, a thing keeps all it holds");
		Expect(!HoldsTogether(Holding(0.0, 0.0, 120.0)), "where there is none, it runs out and the thing falls apart");
		Expect(!HoldsTogether(Holding(0.7, MagicFierceFire + 1.0, 10.0)) && HoldsTogether(Holding(0.7, MagicFierceFire - 1.0, 10.0)), "fierce fire burns it out, and lesser fire does not");

		// Thin magic does nothing to bodies, and over that only the excess does.
		Expect(Only(EMagicType::Earth, MagicActsOnMatter - 1.0).GetActing().GetTotal() == 0.0, "magic thinner than MagicActsOnMatter acts on nothing");
		Expect(std::fabs(Only(EMagicType::Water, MagicActsOnMatter + 10.0).GetActing().Get(EMagicType::Water) - 10.0) < 1e-9, "and denser, only what it has over that acts");
		Expect(Only(EMagicType::Neutral, 500.0).GetActing().GetTotal() == 0.0, "and neutral magic never does");
		Expect(MagicDewPoint < MagicActsOnMatter, "magic shows before it pushes anything");
		for (int Type = 0; Type < static_cast<int>(EMagicType::Neutral); ++Type)
		{
			Expect(MagicManifestDensity(static_cast<EMagicType>(Type)) > MagicDewPoint, "every aspect makes something only once it shows");
		}

		const double NoonSand = GetGroundTemperature(0.8, ESurface::Sand, 0.8);
		std::printf("       sand in a warm place: %.0f degrees at night, %.0f at noon\n", GetGroundTemperature(0.8, ESurface::Sand, 0.0), NoonSand);
		Expect(MagicHeatImprint(GetGroundTemperature(0.8, ESurface::Sand, 0.0), 0.075) == 0.0, "ground gives no fire with no sun on it");
		Expect(MagicHeatImprint(NoonSand, 0.075) > 0.0, "but ground the suns have heated does");
		Expect(GetSunHeat(0.5, 1.0) == 0.0 && GetSunHeat(12.0, 1.0) > 0.3, "the suns heat the ground by day and not by night");

		// Forms change with pressure alone, both ways, and stay one aspect.
		MagicMix Pressed = Only(EMagicType::Fire, 1.0);
		const auto Settle = [&](double Pressure)
		{
			for (int Step = 0; Step < 100; ++Step)
			{
				UpdateFireForms(Pressed, Pressure, 0.05);
			}
		};
		Settle(MagicLightningPressure + 10.0);
		Expect(Pressed.Get(EMagicType::Lightning) > 0.99, "under pressure fire becomes lightning");
		Settle(MagicLightPressure + 10.0);
		Expect(Pressed.Get(EMagicType::Light) > 0.99, "under more pressure lightning becomes light");
		Settle(MagicLightningPressure + 10.0);
		Expect(Pressed.Get(EMagicType::Lightning) > 0.99, "as pressure falls light goes back to lightning");
		Settle(10.0);
		Expect(Pressed.Get(EMagicType::Fire) > 0.99, "and lightning back to fire");
		Expect(std::fabs(Pressed.GetTotal() - 1.0) < 1e-12 && Pressed.GetAspected() == Pressed.GetTotal(), "forms change no amount and leave no neutral magic");
		MagicMix NotFire = Only(EMagicType::Water, 1.0);
		UpdateFireForms(NotFire, MagicLightPressure + 10.0, 1.0);
		Expect(NotFire.Get(EMagicType::Water) == 1.0, "and pressure does nothing to any other aspect");

		// A ball of fire magic squeezed hard is light. As it spreads, the pressure falls and it goes
		// back through lightning to fire, in the gas just as in one cell.
		MagicField Ball(WorldSeed, Closed);
		Ball.SetDensity(EMagicType::Fire, [&Center](const Vec3& Position) { return Length(Position - Center) < 12.0 ? 50.0 : 0.05; });
		const double BallAmount = Ball.GetTotalAmount();
		bool bLight = false;
		bool bLightning = false;
		for (int Step = 0; Step < 200; ++Step)
		{
			Ball.Step(Center, {}, Step * 0.05, 0.05);
			bLight = bLight || Ball.GetDensity(Center, EMagicType::Light) > 5.0;
			bLightning = bLightning || Ball.GetDensity(Center, EMagicType::Lightning) > 5.0;
		}
		std::printf("       after 10 s of spreading: %.2f kg/m3 at the centre of the ball, fire %.2f, lightning %.2f, light %.2f, neutral %.2f\n", Ball.GetDensity(Center),
			Ball.GetDensity(Center, EMagicType::Fire), Ball.GetDensity(Center, EMagicType::Lightning), Ball.GetDensity(Center, EMagicType::Light), Ball.GetDensity(Center, EMagicType::Neutral));
		Expect(bLight, "fire magic squeezed hard enough is light");
		Expect(bLightning, "and as it spreads it passes through lightning");
		Expect(Ball.GetDensity(Center, EMagicType::Light) < 0.25, "and ends with no light left");
		Expect(std::fabs(Ball.GetTotalAmount() - BallAmount) < 1e-3 * BallAmount, "all of which changes no amount of magic");

		// Heat goes where fire magic goes: with none of it there is none.
		Expect(FireUpdraft(0.0) == 0.0 && FireUpdraft(0.05) > 0.5 && FireUpdraft(50.0) < 7.0, "and warm air rises");

		// And what acts on matter is acted on: an impulse moves magic as it would any mass.
		MagicField Struck(WorldSeed, Closed);
		Struck.SetDensity(EMagicType::Air, [](const Vec3&) { return 2.0; });
		Struck.Step(Center, {}, 0.0, 0.05);
		const Vec3 Where{ 10.0, 10.0, 10.0 };
		const double Before = Struck.GetVelocity(Where).X;
		Struck.Push({ Where, { 2.0 * 4.0 * 4.0 * 4.0 * 3.0, 0.0, 0.0 } });
		const double AirInertness = MagicInertness(EMagicType::Air);
		Expect(std::fabs(Struck.GetVelocity(Where).X - Before - 3.0 / AirInertness) < 0.01 * 3.0 / AirInertness, "a push on magic is the impulse over the amount of it, as inert as its aspect");

		// Each aspect is as inert as the order says, and evens itself out the faster the less inert it is.
		const EMagicType ByInertness[] = { EMagicType::Earth, EMagicType::Water, EMagicType::Nature, EMagicType::Fire, EMagicType::Air, EMagicType::Light };
		bool bOrdered = true;
		for (std::size_t Index = 1; Index < std::size(ByInertness); ++Index)
		{
			bOrdered = bOrdered && MagicInertness(ByInertness[Index]) < MagicInertness(ByInertness[Index - 1]);
		}
		Expect(bOrdered, "earth is the most inert, then water, nature, fire, air, and light the least");
		// A lump of one aspect in neutral magic as dense: nothing pushes, so it only soaks outwards.
		const auto Spread = [&Center, &Closed](EMagicType Type)
		{
			MagicField Lump(WorldSeed, Closed);
			const auto Inside = [&Center](const Vec3& Position) { return Length(Position - Center) < 8.0; };
			Lump.SetDensity(Type, [&Inside](const Vec3& Position) { return Inside(Position) ? 4.0 : 0.0; });
			Lump.SetDensity(EMagicType::Neutral, [&Inside](const Vec3& Position) { return Inside(Position) ? 0.0 : 4.0; });
			for (int Step = 0; Step < 100; ++Step)
			{
				Lump.Step(Center, {}, Step * 0.05, 0.05);
			}
			return Lump.GetDensity(Center + Vec3{ 12.0, 0.0, 0.0 }, Type);
		};
		const double EarthOut = Spread(EMagicType::Earth);
		const double FireOut = Spread(EMagicType::Fire);
		std::printf("       a lump of 4 kg/m3 after 5 s, 12 m from its middle: earth %.4f, fire %.4f\n", EarthOut, FireOut);
		Expect(FireOut > EarthOut * 1.5, "fire spreads out faster than earth");

		// In the open: a baseline from the world, carried and stirred by moving air.
		const std::vector<MagicWell> Wells = FindMagicWells(0.0, 0.0, WorldSeed);
		Expect(!Wells.empty(), "the world has wells of magic");
		const MagicWell& Well = Wells.front();
		MagicGround OnLand;
		OnLand.Land = 1.0;
		OnLand.Plants = 1.0;
		MagicGround AtSea;

		MagicGround UnderWell = OnLand;
		UnderWell.Surface = Well.Position.Z - 20.0;
		const double Peak = MagicBaseline(Well.Position, Wells, UnderWell, WorldSeed).GetTotal();
		std::printf("       %zu wells, one %.1f kg/m3 at %.0f %.0f, thin air %.2f kg/m3\n", Wells.size(), Peak, Well.Position.X, Well.Position.Y,
			MagicBaseline(Well.Position + Vec3{ Well.Radius * 5.0, 0.0, 0.0 }, Wells, UnderWell, WorldSeed).GetTotal());
		Expect(Peak > MagicDewPoint, "wells are over the dew point, so they show");

		double DensestWell = 0.0;
		for (const MagicWell& Each : Wells)
		{
			DensestWell = std::fmax(DensestWell, Each.Peak);
		}
		Expect(DensestWell <= 55.0, "dense magic comes to 55 kg/m3");

		double ThickestLayer = 0.0;
		for (double Spot = 0.0; Spot < 6000.0; Spot += 37.0)
		{
			const Vec3 Low{ Spot, Spot * 0.7, 2.0 };
			ThickestLayer = std::fmax(ThickestLayer, MagicBaseline(Low, {}, OnLand, WorldSeed).Get(EMagicType::Earth));
			ThickestLayer = std::fmax(ThickestLayer, MagicBaseline(Low, {}, AtSea, WorldSeed).Get(EMagicType::Water));
		}
		std::printf("       the thickest layer over land or sea is %.2f kg/m3\n", ThickestLayer);
		Expect(ThickestLayer > 2.0 && ThickestLayer <= 3.0, "magic lying on land and sea reaches 2 to 3 kg/m3");

		const Vec3 Away{ 9000.0, 9000.0, 10.0 };
		const MagicMix OverLand = MagicBaseline(Away, {}, OnLand, WorldSeed);
		const MagicMix OverSea = MagicBaseline(Away, {}, AtSea, WorldSeed);
		Expect(OverLand.Get(EMagicType::Earth) > OverLand.Get(EMagicType::Water) * 3.0, "earth magic prevails over land");
		Expect(OverSea.Get(EMagicType::Water) > OverSea.Get(EMagicType::Earth) * 3.0, "water magic prevails over the sea");
		Expect(MagicLandShare(30.0) == 1.0 && MagicLandShare(-30.0) == 0.0, "land and sea are told apart by height");
		Expect(MagicLimit(EMagicType::Water, 5.0, OnLand) < 0.5 && MagicLimit(EMagicType::Earth, 5.0, AtSea) < 0.5, "neither gets dense away from its element");
		Expect(MagicLimit(EMagicType::Earth, 300.0, OnLand) < 0.1, "earth magic thins out with height");
		Expect(MagicLimit(EMagicType::Air, 300.0, AtSea) > 1.0, "air magic is at home at any height");
		Expect(MagicBaseline(Away, {}, AtSea, WorldSeed).Get(EMagicType::Air) > 0.0, "air magic is everywhere");
		const MagicMix InWell = MagicBaseline(Well.Position, Wells, UnderWell, WorldSeed);
		Expect(InWell.Get(EMagicType::Air) > InWell.Get(EMagicType::Earth) && std::fabs(InWell.Get(EMagicType::Air) - InWell.Get(EMagicType::Neutral)) < 1e-9, "wells are air and neutral magic, half and half");
		Expect(MagicLimit(EMagicType::Nature, 0.2, OnLand) > 0.5 && MagicLimit(EMagicType::Nature, 20.0, OnLand) < 0.15, "the blades of grass hold nature magic, and over them it is weak");
		Expect(MagicLimit(EMagicType::Nature, 2.0, AtSea) < 0.01, "natural magic stays near plants");

		MagicField::Options Open;
		Open.Threads = 4;
		MagicField Field(WorldSeed, Open);
		const Vec3 Above = Well.Position + Vec3{ 60.0, 0.0, 0.0 };
		for (int Step = 0; Step < 120; ++Step)
		{
			const double Time = Step * 0.05;
			Field.Step(Above, PrevailingWind(0.0, WorldSeed) * Time, Time, 0.05);
		}
		MagicVolume Volume;
		Field.WriteVolume(Volume);
		const Vec3 High = Above + Vec3{ -20.0, 13.0, 70.0 };
		std::printf("       a well's grid after 6 s: %.1f kg/m3 at its centre, moving at %.1f m/s, spin up to %.2f 1/s\n",
			Field.GetDensity(Well.Position), Length(Field.GetVelocity(High)), Field.GetPeakSpin());
		Expect(Field.GetDensity(Well.Position) > 0.5 * MagicDewPoint, "a well holds its magic against the wind");
		Expect(Length(Field.GetVelocity(High)) > 0.5, "wind carries magic along");
		Expect(Field.GetPeakSpin() > 0.1, "moving air sets magic swirling");
		Expect(std::fabs(Volume.GetMix(High).GetTotal() - Field.GetDensity(High)) < 2.0, "a volume reads back what the grid holds");
		Expect(Volume.Contains(High) && !Volume.Contains(High + Vec3{ 0.0, 0.0, 400.0 }), "a volume knows its bounds");

		// Moving the grid far away must leave nothing behind.
		for (int Step = 0; Step < 4; ++Step)
		{
			Field.Step(Above + Vec3{ 150.0 * (Step + 1), 80.0, 30.0 }, {}, 6.0, 0.05);
		}
		Expect(Field.GetTotalAmount(EMagicType::Earth) > 0.0 && std::isfinite(Field.GetTotalAmount(EMagicType::Water)), "the grid follows the camera");
	}

	void TestStorms()
	{

		// Rubbing goes as the square of the speed, and takes both of them.
		Expect(MagicRubPressure(0.0, 1.0, 30.0) == 0.0 && MagicRubPressure(1.0, 0.0, 30.0) == 0.0, "water magic or air magic alone rubs against nothing");
		Expect(std::fabs(MagicRubPressure(1.0, 1.0, 20.0) - 100.0 * MagicDewPoint / 5.5) < 1e-9, "rubbing presses as half the reduced density by the speed squared, squeezed as the dew point says");
		Expect(MagicRubImprint(0.0) == 0.0 && MagicRubImprint(200.0) > MagicRubImprint(100.0), "harder rubbing gives more of the aspect of fire");
		Expect(CloudUpdraft(100.0) == 0.0 && CloudUpdraft(1200.0) > 30.0, "only tall clouds churn");

		// A cell of cloud rubbed hard makes lightning, from neutral magic, by the rules for fire.
		MagicMix Rubbed;
		Rubbed.Amounts[static_cast<int>(EMagicType::Water)] = 0.8;
		Rubbed.Amounts[static_cast<int>(EMagicType::Air)] = 0.4;
		Rubbed.Amounts[static_cast<int>(EMagicType::Neutral)] = 0.4;
		const double Hard = MagicRubPressure(0.8, 0.4, 30.0);
		std::printf("       water 0.8 and air 0.4 at 30 m/s rub at %.0f Pa\n", Hard);
		Expect(Hard + 1.6 * MagicSoundSpeed * MagicSoundSpeed >= MagicLightningPressure && Hard + 1.6 * MagicSoundSpeed * MagicSoundSpeed < MagicLightPressure, "hard rubbing is enough for lightning and not for light");
		MagicMix Rates;
		Rates.Amounts[static_cast<int>(EMagicType::Fire)] = MagicRubImprint(Hard);
		const double Before = Rubbed.GetTotal();
		for (int Step = 0; Step < 120; ++Step)
		{
			UpdateMagicAspects(Rubbed, Rates, 0.25);
			UpdateFireForms(Rubbed, Rubbed.GetTotal() * MagicSoundSpeed * MagicSoundSpeed + Hard, 0.25);
		}
		Expect(Rubbed.Get(EMagicType::Lightning) > MagicBreakdown && Rubbed.Get(EMagicType::Fire) < 0.1 * Rubbed.Get(EMagicType::Lightning), "the rubbing of a storm makes lightning, and hardly any plain fire");
		Expect(std::fabs(Rubbed.GetTotal() - Before) < 1e-9, "and leaves the amount of magic as it was");

		// A sheet of real clouds: no amount is lost, and tall clouds strike.
		constexpr int NoiseSize = 64;
		StormField Sky(WorldSeed, MakeCloudNoise(NoiseSize), NoiseSize, CloudBottom, 1.0 / 80000.0);
		const Vec3 Wind{ 4.0, 0.0, 0.0 };
		Sky.Update({}, {}, Wind, 1.5, 0.0, 0.0);
		const double SkyAmount = Sky.GetTotalAmount();
		for (int Step = 1; Step <= 240; ++Step)
		{
			Sky.Update({}, {}, Wind, 1.5, 0.0, Step * 0.25);
		}
		std::printf("       %d bolts in a minute over 19 km of heavy cloud\n", Sky.GetBoltCount());
		Expect(Sky.GetBoltCount() > 0, "tall clouds strike");
		Expect(std::fabs(Sky.GetTotalAmount() + Sky.GetRainedDown() - Sky.GetCameIn() - SkyAmount) < 1e-6 * SkyAmount, "neither the wind, their rubbing nor their bolts change the amount of magic, only what the rain takes down and the wind brings across the edges");

		// Clouds are where water magic gathers: what the suns lift off the water, run together by the wind.
		const auto Survey = [](const StormField& Field, double& OutMost, int& OutRaining, int& OutStorming)
		{
			OutMost = 0.0;
			OutRaining = 0;
			OutStorming = 0;
			for (double Y = -9400.0; Y < 9400.0; Y += StormField::CellSize)
			{
				for (double X = -9400.0; X < 9400.0; X += StormField::CellSize)
				{
					const double Water = Field.Read({ X, Y, 0.0 }).Mix.Get(EMagicType::Water);
					OutMost = std::max(OutMost, Water);
					OutRaining += Water > MagicRainWater ? 1 : 0;
					OutStorming += Water > MagicStormWater ? 1 : 0;
				}
			}
		};
		StormField Weather(WorldSeed, MakeCloudNoise(NoiseSize), NoiseSize, CloudBottom, 1.0 / 80000.0);
		const Vec3 Aloft = WindAt({}, 0.0, WorldSeed);
		double Most = 0.0;
		int Raining = 0;
		int Storming = 0;
		for (int Minute = 0; Minute <= 20; ++Minute)
		{
			if (Minute % 5 == 0)
			{
				Survey(Weather, Most, Raining, Storming);
				std::printf("       after %2d min of sun: water magic in the clouds up to %.1f kg/m3, %d cells raining, %d stormy\n", Minute, Most, Raining, Storming);
			}
			for (int Step = 0; Step < 240; ++Step)
			{
				const double Time = Minute * 60.0 + Step * 0.25;
				Weather.Update({}, Aloft * CloudWindRatio * Time, Aloft, 1.0, 1.0, Time);
			}
		}
		{
			double MostAir = 0.0;
			double MostNeutral = 0.0;
			double MostLightning = 0.0;
			for (double Y = -9400.0; Y < 9400.0; Y += StormField::CellSize)
			{
				for (double X = -9400.0; X < 9400.0; X += StormField::CellSize)
				{
					const StormField::Reading Over = Weather.Read({ X, Y, 0.0 });
					MostAir = std::max(MostAir, Over.Mix.Get(EMagicType::Air));
					MostNeutral = std::max(MostNeutral, Over.Mix.Get(EMagicType::Neutral));
					MostLightning = std::max(MostLightning, Over.Mix.Get(EMagicType::Lightning));
				}
			}
			std::printf("       most air magic in the clouds %.2f, neutral %.2f, lightning %.3f kg/m3\n", MostAir, MostNeutral, MostLightning);
		}
		std::printf("       %d bolts in those 20 minutes\n", Weather.GetBoltCount());
		{
			// Rain must only fall from a cloud that is there to be seen. Over each cell of the sheet: what the
			// weather says, and how much of the sky Shaders/Clouds.hlsli draws there, which MeasureCloud reads
			// the same way at the cover that matches what the cell's water magic gathers.
			int RainyCells = 0;
			int RainWithoutCloud = 0;
			int ChargedClear = 0;
			double CoverWhereRaining = 0.0;
			for (double Y = -9400.0; Y < 9400.0; Y += StormField::CellSize)
			{
				for (double X = -9400.0; X < 9400.0; X += StormField::CellSize)
				{
					const StormField::Reading Over = Weather.Read({ X, Y, 0.0 });
					const double Water = Over.Mix.Get(EMagicType::Water);
					const Sim::Weather Here = GetWeather(Water, Over.Mix.Get(EMagicType::Lightning));
					ChargedClear += Over.CloudDepth <= 0.0 && Over.Mix.Get(EMagicType::Lightning) >= MagicBreakdown ? 1 : 0;
					if (Here.Rain <= 0.0)
					{
						continue;
					}
					++RainyCells;
					RainWithoutCloud += Over.CloudDepth <= 0.0 ? 1 : 0;
					int Covered = 0;
					for (int Spot = 0; Spot < 16; ++Spot)
					{
						const double Gathered = Over.CloudDepth / CloudDepthFor(MagicStormWater);
						Covered += Weather.MeasureCloud(X + (Spot % 4) * 100.0, Y + (Spot / 4) * 100.0, ShownCloudCover(Gathered)) > 0.0 ? 1 : 0;
					}
					CoverWhereRaining += Covered / 16.0;
				}
			}
			std::printf("       %d cells with rain: %d of them with no cloud at all, %.0f%% of their sky drawn with cloud; %d clear cells hold a charge\n",
				RainyCells, RainWithoutCloud, RainyCells > 0 ? 100.0 * CoverWhereRaining / RainyCells : 0.0, ChargedClear);
			Expect(RainWithoutCloud == 0 && ChargedClear == 0, "it rains, and lightning gathers, only in a cloud");
			Expect(RainyCells == 0 || CoverWhereRaining / RainyCells > 0.7, "and where it rains, most of the sky is drawn with cloud");
		}
		Expect(Raining > 0, "water magic gathers in the clouds until it rains");
		Expect(Weather.GetRainedDown() > 0.0, "and the rain takes water magic down");
		Expect(GetWeather(MagicRainWater - 1.0, 0.0).Rain == 0.0 && GetWeather(MagicRainWater + 6.0, 0.0).Rain > 0.0, "it rains only over 30 kg/m3 of water magic");
		Expect(GetWeather(MagicStormWater - 1.0, 0.0).Storm == 0.0 && GetWeather(MagicStormWater + 5.0, 0.0).Storm > 0.0, "and storms over 50");
		Expect(CloudDepthFor(MagicCloudWater - 1.0) == 0.0 && CloudDepthFor(MagicStormWater) > 1000.0, "thin water magic makes no cloud, and a storm's is tall");

		// The sky is open under scattered cloud and closed by the time it rains, which is what takes the suns' shadows away.
		const double RainGathered = CloudDepthFor(MagicRainWater) / CloudDepthFor(MagicStormWater);
		Expect(GetOvercast(0.3) == 0.0 && GetOvercast(RainGathered) > 0.7 && GetOvercast(0.6) == 1.0, "the sky is open under scattered cloud and closed where it rains");

		// The eye takes some of the cast out of a coloured light, keeps grey as bright, and leaves the night alone.
		const Vec3 BlueGreen{ 0.72, 1.78, 1.65 };
		const Vec3 Gains = GetWhiteBalance(BlueGreen);
		const auto GetGrey = [](const Vec3& Color) { return 0.2126 * Color.X + 0.7152 * Color.Y + 0.0722 * Color.Z; };
		std::printf("       under the blue-green sun alone the eye shows red %.2f times as strong, and green %.2f\n", Gains.X, Gains.Y);
		Expect(Gains.X > 1.1 && Gains.X < 1.5 && Gains.Y < 1.0, "the eye takes some of a coloured light's cast out, and not all");
		Expect(std::fabs(GetGrey({ BlueGreen.X * Gains.X, BlueGreen.Y * Gains.Y, BlueGreen.Z * Gains.Z }) - GetGrey(BlueGreen)) < 1e-9, "and leaves it as bright");
		const Vec3 AtNight = GetWhiteBalance({ 0.003, 0.005, 0.012 });
		Expect(std::fabs(AtNight.X - 1.0) < 0.05 && std::fabs(AtNight.Z - 1.0) < 0.05, "in the dark it does not, and the night keeps its blue");

		// The wind blows differently from place to place, so it runs things together.
		const Vec3 Here = LargeWindAt(0.0, 0.0, 100.0, WorldSeed);
		double Widest = 0.0;
		for (double Away = 1000.0; Away <= 20000.0; Away += 1000.0)
		{
			const Vec3 There = LargeWindAt(Away, Away * 0.4, 100.0, WorldSeed);
			Widest = std::max(Widest, std::acos(std::clamp(Dot(Normalize(Here), Normalize(There)), -1.0, 1.0)));
		}
		std::printf("       the wind turns up to %.0f degrees from one place to another within 20 km\n", Widest * 180.0 / Pi);
		Expect(Widest > 0.6, "the wind does not blow the same way everywhere");

		// What a bolt brings to the gas is dense enough to change matter at first, and goes from lightning to fire by itself.
		MagicField::Options Sealed;
		Sealed.bOpen = false;
		MagicField Struck(WorldSeed, Sealed);
		const Vec3 Middle{ 0.0, 0.0, 0.0 };
		Struck.Step(Middle, {}, 0.0, 0.05);
		for (double Up = -40.0; Up < 40.0; Up += 4.0)
		{
			Struck.Deposit({ Middle + Vec3{ 0.0, 0.0, Up }, EMagicType::Lightning, MagicChannelDensity });
		}
		double MostPlainFire = 0.0;
		for (int Step = 1; Step <= 200; ++Step)
		{
			Struck.Step(Middle, {}, Step * 0.05, 0.05);
			MostPlainFire = std::max(MostPlainFire, Struck.GetDensity(Middle, EMagicType::Fire));
		}
		std::printf("       a bolt leaves up to %.2f kg/m3 of plain fire\n", MostPlainFire);
		Expect(MostPlainFire > 0.1, "a bolt's lightning turns to fire");
		Expect(MostPlainFire < MagicFierceFire, "too thin by then to change matter");
		Expect(Struck.GetDensity(Middle, EMagicType::Lightning) < 0.01, "and none of it stays lightning");
		Expect(Dot(FireBlast({ 0.25, 0.0, 0.0 }, 1.0), Vec3{ 1.0, 0.0, 0.0 }) < 0.0, "swelling air pushes out of where the fire is");

		// More fire than the wind brings air magic grows on it, using that up. Less is blown away.
		const auto InWind = [](double Fire, double Air, double Wind, double Draft = 0.0)
		{
			MagicMix Mix;
			Mix.Amounts[static_cast<int>(EMagicType::Fire)] = Fire;
			Mix.Amounts[static_cast<int>(EMagicType::Air)] = Air;
			Mix.Amounts[static_cast<int>(EMagicType::Neutral)] = 1.0;
			const double Total = Mix.GetTotal();
			for (int Step = 0; Step < 20; ++Step)
			{
				UpdateFireInWind(Mix, Wind, Draft, 0.05);
			}
			Expect(std::fabs(Mix.GetTotal() - Total) < 1e-9, "wind on fire changes no amount of magic");
			return Mix;
		};
		const MagicMix Fed = InWind(1.0, 0.1, 8.0);
		const MagicMix Calm = InWind(1.0, 0.1, 1.0);
		const MagicMix Blown = InWind(0.02, 0.2, 8.0);
		std::printf("       in a second, 1 kg/m3 of fire in wind of 8 m/s grows to %.3f, in 1 m/s to %.3f; 0.02 in wind of 8 m/s falls to %.4f\n", Fed.Get(EMagicType::Fire), Calm.Get(EMagicType::Fire), Blown.Get(EMagicType::Fire));
		Expect(Fed.Get(EMagicType::Fire) > Calm.Get(EMagicType::Fire) && Calm.Get(EMagicType::Fire) > 1.0, "wind feeds a fire bigger than itself, the more the harder it blows");
		Expect(Fed.Get(EMagicType::Air) < 0.1, "and that fire burns up air magic");
		Expect(Blown.Get(EMagicType::Fire) < 0.02 && Blown.Get(EMagicType::Fire) > 0.015, "wind slowly wears down a fire smaller than itself");
		const MagicMix Drawing = InWind(1.0, 0.1, 0.0, FireUpdraft(1.0));
		Expect(Drawing.Get(EMagicType::Fire) > 1.0 && Drawing.Get(EMagicType::Air) < 0.1, "in still air a fire's own draft draws air magic in and burns it");
		Expect(InWind(0.02, 0.2, 0.0, FireUpdraft(0.02)).Get(EMagicType::Fire) >= 0.02, "but its draft blows nothing out");

		// Only dense fire boils water, in whatever form it has, and that spends the fire.
		const auto Boiled = [](EMagicType Form, double Amount)
		{
			MagicMix Mix;
			Mix.Amounts[static_cast<int>(Form)] = Amount;
			Mix.Amounts[static_cast<int>(EMagicType::Neutral)] = 1.0;
			const double Total = Mix.GetTotal();
			const double Spent = BoilWater(Mix, 0.05);
			Expect(std::fabs(Mix.GetTotal() - Total) < 1e-9, "boiling water changes no amount of magic");
			return Spent;
		};
		Expect(Boiled(EMagicType::Fire, MagicFierceFire * 0.99) == 0.0, "fire short of fierce leaves water alone");
		Expect(Boiled(EMagicType::Fire, MagicFierceFire) > 0.0 && Boiled(EMagicType::Light, 160.0) > Boiled(EMagicType::Lightning, 60.0), "fierce fire of any form boils it, the more the denser");

		// On the ground the gas itself finds where that happens.
		const auto FindGround = [](bool bWater)
		{
			for (double Spot = 0.0; Spot < 40000.0; Spot += 61.0)
			{
				const Vec3 Place{ Spot, Spot * 0.6, 0.0 };
				const MagicGround Ground = ReadMagicGround(Place.X, Place.Y, WorldSeed);
				if (bWater ? Ground.bWater : (Ground.Plants > 0.0 && !Ground.bWater))
				{
					return Vec3{ Place.X, Place.Y, Ground.Surface };
				}
			}
			return Vec3{};
		};
		const auto Touch = [](const Vec3& Place, double Density)
		{
			MagicField::Options Open;
			MagicField Gas(WorldSeed, Open);
			Gas.Step(Place, {}, 0.0, 0.05);
			for (double Up = 0.0; Up < 120.0; Up += 4.0)
			{
				Gas.Deposit({ Place + Vec3{ 0.0, 0.0, Up }, EMagicType::Lightning, Density });
			}
			std::vector<FireContact> Found;
			int StepsTouching = 0;
			for (int Step = 1; Step <= 40; ++Step)
			{
				const std::size_t Before = Found.size();
				Gas.Step(Place, {}, Step * 0.05, 0.05);
				Gas.TakeContacts(Found);
				StepsTouching += Found.size() > Before ? 1 : 0;
			}
			std::printf("       %.1f kg/m3 along a channel touches the surface for %d steps of the gas, %zu cells in all\n", Density, StepsTouching, Found.size());
			return Found;
		};
		const std::vector<FireContact> OnWater = Touch(FindGround(true), MagicChannelDensity);
		const std::vector<FireContact> OnPlants = Touch(FindGround(false), MagicChannelDensity);
		double AllBoiled = 0.0;
		for (const FireContact& Each : OnWater)
		{
			AllBoiled += Each.Boiled;
		}
		std::printf("       a bolt on water boils away %.0f kg of its fire\n", AllBoiled);
		Expect(!OnWater.empty() && AllBoiled > 0.0, "a bolt on water boils it");
		Expect(!OnPlants.empty() && OnPlants.front().Boiled == 0.0, "and on plants it does not");
		Expect(Touch(FindGround(true), MagicFierceFire * 0.5).empty(), "weaker fire leaves them alone");

		// Magic is there all the way up to about 2 km over the ground, and not above that.
		const MagicGround Plain = ReadMagicGround(300.0, 200.0, WorldSeed);
		Expect(MagicBaseline({ 300.0, 200.0, Plain.Surface + 1500.0 }, {}, Plain, WorldSeed).GetTotal() > 0.02, "magic reaches high over the ground");
		Expect(MagicBaseline({ 300.0, 200.0, Plain.Surface + 2500.0 }, {}, Plain, WorldSeed).GetTotal() == 0.0, "but gives out above 2 km");

		Sky.ForceStrike({ 300.0, 200.0, 0.0 });
		std::vector<BoltDraw> Bolts;
		Sky.CollectBolts(Bolts);
		Expect(!Bolts.empty() && Bolts.back().Stroke > 0.9, "a bolt lights up once it reaches the ground");
		const std::vector<BoltSegment>& Channel = *Bolts.back().Segments;
		double Lowest = 1e9;
		for (const BoltSegment& Segment : Channel)
		{
			Lowest = std::min(Lowest, Segment.To.Z);
		}
		const double Under = std::max(TerrainHeight(300.0, 200.0, WorldSeed), 0.0);
		std::printf("       a bolt of %d segments, down to %.0f m over ground at about %.0f m\n", static_cast<int>(Channel.size()), Lowest, Under);
		Expect(Channel.size() > 60 && Channel.size() < 1500 && Lowest < Under + 200.0, "and its channel goes all the way down");
	}

	void TestWaves()
	{
		double Lowest = 1e9;
		double Highest = -1e9;
		for (double T = 0.0; T < 60.0; T += 0.37)
		{
			const double H = WaterHeight(T * 13.0, T * 7.0, T);
			Lowest = std::fmin(Lowest, H);
			Highest = std::fmax(Highest, H);
		}
		std::printf("       waves %.2f..%.2f m\n", Lowest, Highest);
		Expect(Highest > 0.2 && Lowest < -0.2, "the sea has waves");
		Expect(Highest < 1.0 && Lowest > -1.0, "waves stay small");
		Expect(std::fabs(WaterHeight(10.0, 20.0, 5.0) - WaterHeight(10.0 + 4096.0, 20.0 - 4096.0, 5.0)) < 1e-9,
			"waves repeat every 4096 m, so shaders can wrap the camera position");
	}

	void TestGaze()
	{
		constexpr double Dt = 1.0 / 120.0;

		const auto Rest = [](Gaze& Target, double Seconds)
		{
			for (int Step = 0; Step < static_cast<int>(Seconds / Dt); ++Step)
			{
				Target.Turn(0.0, 0.0, Dt);
			}
		};

		// A short flick: 100 counts in a twentieth of a second.
		Gaze View;
		for (int Step = 0; Step < 6; ++Step)
		{
			View.Turn(100.0 / 6.0, 0.0, Dt);
		}
		const double Flick = 100.0 * Gaze::Sensitivity;
		Expect(std::fabs(View.Yaw + View.EyeYaw - Flick) < 1e-9, "the focus goes straight to the point of gaze");
		Expect(View.EyeYaw > Flick * 0.9, "the focus shifts first, while the camera has hardly moved");

		Rest(View, 0.15);
		Expect(View.Yaw > Flick * 0.1 && View.Yaw < Flick * 0.75, "then the camera follows, partway after a moment");
		Expect(std::fabs(View.Yaw + View.EyeYaw - Flick) < 1e-9, "the gaze stays on its point in the world as the camera turns");

		Rest(View, 3.0);
		Expect(std::fabs(View.EyeYaw - Gaze::NeutralRadius) < 1e-3, "the camera stops once the gaze is inside the neutral circle");
		Expect(View.Yaw <= Flick - Gaze::NeutralRadius + 1e-9, "the camera settles without overshooting");

		// Inside the neutral circle the camera does not turn at all.
		Gaze Inside;
		Inside.Turn(25.0, 0.0, Dt);
		Rest(Inside, 2.0);
		Expect(Inside.Yaw == 0.0 && Inside.EyeYaw == 25.0 * Gaze::Sensitivity, "a gaze inside the neutral circle leaves the camera alone");

		// It is a circle, not a square: the same travel on both axes reaches further.
		Gaze Corner;
		Corner.Turn(25.0, 25.0, Dt);
		Rest(Corner, 3.0);
		Expect(Corner.Yaw > 0.0 && Corner.Pitch > 0.0, "the neutral zone is round, so a diagonal can leave it");
		Expect(std::fabs(std::hypot(Corner.EyeYaw, Corner.EyePitch) - Gaze::NeutralRadius) < 1e-3, "after a diagonal the gaze rests on the edge of the circle");

		// However far the mouse goes in one frame, the camera does not jump.
		Gaze Jump;
		Jump.Turn(1000.0, 0.0, Dt);
		Expect(Jump.Yaw < 1000.0 * Gaze::Sensitivity * 0.01, "the camera never jumps with the mouse");
		Expect(std::fabs(Jump.EyeYaw - Gaze::FocusLimit) < 1e-9, "during a fast sweep the focus waits on its limit, on screen");

		Rest(Jump, 4.0);
		const double Left = 1000.0 * Gaze::Sensitivity - Jump.Yaw;
		Expect(Left > -1e-3 && Left < Gaze::NeutralRadius + 1e-3, "the camera still arrives where the mouse pointed, without passing it");

		// Just inside and just outside the critical circle, over the same short time.
		const auto TurnFrom = [&Rest](double Offset)
		{
			Gaze Item;
			Item.Aim(0.0, 0.0);
			Item.Turn(Offset / Gaze::Sensitivity, 0.0, Dt);
			Rest(Item, 0.06);
			return Item.Yaw;
		};
		const double Inner = TurnFrom(Gaze::CriticalRadius - 0.01);
		const double Outer = TurnFrom(Gaze::CriticalRadius + 0.01);
		std::printf("       camera turned %.3f rad inside the critical circle, %.3f rad outside it\n", Inner, Outer);
		Expect(Outer > Inner * 2.0, "beyond the critical circle the camera turns much faster");

		Gaze Critical;
		Critical.Turn(0.8 / Gaze::Sensitivity, 0.0, Dt);
		Rest(Critical, 0.35);
		Expect(std::hypot(Critical.EyeYaw, Critical.EyePitch) < Gaze::CriticalRadius, "the camera brings the gaze back inside the critical circle");
		Rest(Critical, 4.0);
		Expect(Critical.EyeYaw > -1e-3 && Critical.EyeYaw < Gaze::NeutralRadius + 1e-3, "and then settles with the gaze inside the neutral circle");

		// Reversing: the focus answers at once, and the camera comes after it.
		Gaze Reverse;
		for (int Step = 0; Step < 6; ++Step)
		{
			Reverse.Turn(30.0, 0.0, Dt);
		}
		Rest(Reverse, 3.0);
		const double Settled = Reverse.Yaw;
		Reverse.Turn(-90.0, 0.0, Dt);
		Expect(Reverse.EyeYaw < -0.08 && std::fabs(Reverse.Yaw - Settled) < 1e-3, "reversing the mouse moves the focus before the camera");

		// The same motion over the same time lands in the same place at any frame rate.
		const auto RunAt = [](int FramesPerSecond)
		{
			const double Step = 1.0 / FramesPerSecond;
			Gaze Item;
			Item.Turn(150.0, 0.0, Step);
			for (int Frame = 1; Frame < FramesPerSecond / 5; ++Frame)
			{
				Item.Turn(0.0, 0.0, Step);
			}
			return Item.Yaw;
		};
		Expect(std::fabs(RunAt(30) - RunAt(960)) < 1e-9, "following does not depend on the frame rate");

		Gaze Direct;
		Direct.Aim(0.0, 0.0, 0.1, 0.0);
		Direct.TurnCamera(100.0, 0.0);
		Expect(Direct.EyeYaw == 0.0 && std::fabs(Direct.Yaw - (0.1 + 100.0 * Gaze::Sensitivity)) < 1e-12, "with the effect off the mouse turns the camera directly");
	}

	void TestForces()
	{
		const Vec3 Drag = AeroForce({ 10.0, 0.0, 0.0 }, 1.0, 1.0);
		Expect(std::fabs(Drag.X - 61.25) < 1e-9, "drag follows 0.5 rho Cd A v^2");
		const Vec3 Reverse = AeroForce({ -10.0, 0.0, 0.0 }, 1.0, 1.0);
		Expect(Reverse.X == -Drag.X, "drag points along the relative air");

		Expect(SubmergedFraction(5.0, 1.0) == 0.0, "body above water is dry");
		Expect(SubmergedFraction(-5.0, 1.0) == 1.0, "deep body is fully submerged");
		Expect(std::fabs(SubmergedFraction(0.0, 1.0) - 0.5) < 1e-12, "body on the water line is half submerged");

		// A crate at half the density of water must float half submerged.
		constexpr double Volume = 1.0;
		const double Mass = 500.0;
		const Vec3 Lift = WaterForce(0.5, Volume, {}, 0.0, Mass);
		Expect(std::fabs(Lift.Z - Mass * Gravity) < 1e-9, "wood floats half submerged");

		const Vec3 Damped = WaterForce(1.0, 0.0, { 0.0, 0.0, -2.0 }, 1.5, 10.0);
		Expect(Damped.Z > 0.0, "water drag opposes motion");
	}
}

namespace
{
	void TestThrowAngle()
	{
		// In a vacuum the flatter arc has a closed form to check against.
		const auto Flatter = [](double Distance, double Rise, double Speed)
		{
			const double Speed2 = Speed * Speed;
			const double Root = std::sqrt(Speed2 * Speed2 - 9.81 * (9.81 * Distance * Distance + 2.0 * Rise * Speed2));
			return std::atan((Speed2 - Root) / (9.81 * Distance));
		};

		double Angle = 0.0;
		const bool bLevel = Sim::SolveThrowAngle(50.0, 0.0, 32.0, 0.0, 9.81, Angle);
		std::printf("       level target: %.4f rad, exact %.4f\n", Angle, Flatter(50.0, 0.0, 32.0));
		Expect(bLevel && std::abs(Angle - Flatter(50.0, 0.0, 32.0)) < 0.01, "throw angle to a level target matches the vacuum formula");

		const bool bRaised = Sim::SolveThrowAngle(30.0, 10.0, 32.0, 0.0, 9.81, Angle);
		std::printf("       raised target: %.4f rad, exact %.4f\n", Angle, Flatter(30.0, 10.0, 32.0));
		Expect(bRaised && std::abs(Angle - Flatter(30.0, 10.0, 32.0)) < 0.01, "throw angle to a raised target matches the vacuum formula");

		const bool bLowered = Sim::SolveThrowAngle(40.0, -30.0, 32.0, 0.0, 9.81, Angle);
		Expect(bLowered && Angle < 0.0, "a target far below is thrown down at, not up");

		Expect(!Sim::SolveThrowAngle(200.0, 0.0, 32.0, 0.0, 9.81, Angle), "beyond v^2 / g a target is out of reach");

		double Thin = 0.0;
		double Thick = 0.0;
		Sim::SolveThrowAngle(30.0, 0.0, 32.0, 0.0, 9.81, Thin);
		const bool bDrag = Sim::SolveThrowAngle(30.0, 0.0, 32.0, 0.0135, 9.81, Thick);
		std::printf("       with drag %.4f rad, without %.4f\n", Thick, Thin);
		Expect(bDrag && Thick > Thin, "air drag calls for a higher throw");
	}

	void TestFireAround()
	{
		// The campfire by the spawn, in the gas as the game runs it.
		const Vec3 Spawn = FindLandSpawn(WorldSeed);
		HotSpot Camp;
		double Nearest = 1e30;
		for (const HotSpot& Spot : FindHotSpots(Spawn.X, Spawn.Y, WorldSeed))
		{
			const double Apart = Length(Spot.Position - Spawn);
			if (!Spot.bLava && Apart < Nearest)
			{
				Nearest = Apart;
				Camp = Spot;
			}
		}
		Expect(Nearest < 100.0, "there is a campfire by the spawn");

		MagicField::Options Open;
		Open.Threads = 8;
		MagicField Gas(WorldSeed, Open);
		constexpr double Dt = 0.1;
		double Time = 0.0;
		const auto Run = [&](int Steps)
		{
			for (int Step = 0; Step < Steps; ++Step)
			{
				Gas.Step(Camp.Position, PrevailingWind(0.0, WorldSeed) * Time, Time, Dt);
				Time += Dt;
			}
		};

		// A cloud of water magic let go well away from the fire, where nothing heats it, to see whether it sinks.
		Run(20);
		const Vec3 Release = Camp.Position + Vec3{ 90.0, 0.0, 60.0 };
		std::vector<double> Background;
		for (double Z = -40.0; Z <= 40.0; Z += 4.0)
		{
			for (double Y = -40.0; Y <= 40.0; Y += 4.0)
			{
				for (double X = -40.0; X <= 40.0; X += 4.0)
				{
					Background.push_back(Gas.GetDensity(Release + Vec3{ X, Y, Z }, EMagicType::Water));
				}
			}
		}
		for (double Z = -8.0; Z <= 8.0; Z += 4.0)
		{
			for (double Y = -8.0; Y <= 8.0; Y += 4.0)
			{
				for (double X = -8.0; X <= 8.0; X += 4.0)
				{
					Gas.Deposit({ Release + Vec3{ X, Y, Z }, EMagicType::Water, 6.0 });
				}
			}
		}
		const auto Middle = [&]
		{
			// Only what is over the water magic that was there before, which lies thicker lower down.
			double Mass = 0.0;
			double Height = 0.0;
			std::size_t Index = 0;
			for (double Z = -40.0; Z <= 40.0; Z += 4.0)
			{
				for (double Y = -40.0; Y <= 40.0; Y += 4.0)
				{
					for (double X = -40.0; X <= 40.0; X += 4.0)
					{
						const double Water = std::max(Gas.GetDensity(Release + Vec3{ X, Y, Z }, EMagicType::Water) - Background[Index++], 0.0);
						Mass += Water;
						Height += Water * Z;
					}
				}
			}
			return Height / std::max(Mass, 1e-9);
		};
		const double MiddleBefore = Middle();
		Run(150);
		const double MiddleAfter = Middle();
		std::printf("       water magic let go 60 m up has its middle %+.1f m from there, and %+.1f m 15 s later\n", MiddleBefore, MiddleAfter);
		Expect(MiddleAfter > MiddleBefore - 1.0, "magic weighs nothing: a cloud of it does not sink");

		// Fire magic at the fire, and on rings around it at the same height, the mean over each ring.
		Run(150);
		const auto Ring = [&](double Away, double Up)
		{
			if (Away == 0.0)
			{
				return Gas.GetDensity(Camp.Position + Vec3{ 0.0, 0.0, Up }, EMagicType::Fire);
			}
			double Sum = 0.0;
			for (int Turn = 0; Turn < 16; ++Turn)
			{
				const double Angle = Turn * 3.14159265358979 / 8.0;
				Sum += Gas.GetDensity(Camp.Position + Vec3{ std::cos(Angle) * Away, std::sin(Angle) * Away, Up }, EMagicType::Fire);
			}
			return Sum / 16.0;
		};
		const double Away[] = { 0.0, 4.0, 8.0, 12.0, 16.0, 24.0 };
		for (const double Up : { 1.0, 6.0, 14.0 })
		{
			std::printf("       fire magic %2.0f m up, by distance from the campfire:", Up);
			for (const double Each : Away)
			{
				std::printf(" %.0fm %.3f", Each, Ring(Each, Up));
			}
			std::printf("\n");
		}
		Expect(Ring(0.0, 1.0) > 2.0, "a campfire holds fire magic");
		Expect(Ring(8.0, 1.0) > 0.1 && Ring(8.0, 1.0) < 0.5 * Ring(0.0, 1.0), "and there is less of it around, further from it");
		Expect(Ring(16.0, 1.0) < Ring(8.0, 1.0) && Ring(0.0, 1.0) < MagicFierceFire, "less still further out, and never enough to set the grass alight");
		const double AirAtFire = Gas.GetDensity(Camp.Position + Vec3{ 0.0, 0.0, 1.0 }, EMagicType::Air);
		const double AirAway = Gas.GetDensity(Camp.Position + Vec3{ 40.0, 0.0, 1.0 }, EMagicType::Air);
		std::printf("       air magic in the campfire %.3f kg/m3, 40 m from it %.3f\n", AirAtFire, AirAway);
		Expect(AirAtFire < 0.5 * AirAway, "the fire burns the air magic around it");
		const double RisingOver = Gas.GetVelocity(Camp.Position + Vec3{ 0.0, 0.0, 6.0 }).Z;
		const double RisingAway = Gas.GetVelocity(Camp.Position + Vec3{ 40.0, 0.0, 6.0 }).Z;
		std::printf("       magic rises at %.2f m/s over the campfire, %.2f m/s 40 m off\n", RisingOver, RisingAway);
		Expect(RisingOver > RisingAway + 0.3, "fire magic rises, and takes the magic over it up in a stream");
		Expect(Length(MagicStirredWind(20.0, { 3.0, 0.0, 0.0 })) > 2.0 && Length(MagicStirredWind(0.02, { 3.0, 0.0, 0.0 })) < 0.02, "a stream of dense air magic is a wind, and thin air magic stirs none");

		// Drops of light magic from a sun overhead come to rest in the gas.
		const double LightBefore = Gas.GetTotalAmount(EMagicType::Light) + Gas.GetTotalAmount(EMagicType::Nature);
		Gas.SetLightDrops({ { { { 0.0, 0.0, 1.0 }, 20.0 }, {}, {} } });
		Run(4);
		const double LightAfter = Gas.GetTotalAmount(EMagicType::Light) + Gas.GetTotalAmount(EMagicType::Nature);
		std::printf("       a sun 20 times as strong overhead adds %.0f kg of light and nature magic in 0.4 s\n", LightAfter - LightBefore);
		Expect(LightAfter > LightBefore + 1000.0, "the suns pelt the world with drops of light magic");
	}
}

namespace
{
	void TestMagicShows()
	{
		// A small box of gas around a point on the ground, thin everywhere but in the cell with the surface in it.
		const auto Around = [](const Vec3& Point, EMagicType Type, double Density, double Above = 0.0)
		{
			MagicVolume Box;
			Box.SizeX = Box.SizeY = Box.SizeZ = 8;
			Box.CellSize = 4.0;
			Box.Corner = Vec3{ std::floor(Point.X / 4.0) * 4.0 - 16.0, std::floor(Point.Y / 4.0) * 4.0 - 16.0, std::floor((Point.Z + Above) / 4.0) * 4.0 - 16.0 };
			for (std::vector<float>& Kind : Box.Amounts)
			{
				Kind.assign(512, 0.05f);
			}
			for (std::vector<float>& Axis : Box.Flow)
			{
				Axis.assign(512, 0.0f);
			}
			Box.Amounts[static_cast<int>(Type)][4 + 8 * (4 + 8 * 4)] = static_cast<float>(Density);
			return Box;
		};
		const auto Ground = [](bool bWater)
		{
			for (double Spot = 0.0; Spot < 40000.0; Spot += 61.0)
			{
				const double X = Spot;
				const double Y = Spot * 0.6;
				const double Height = TerrainHeight(X, Y, WorldSeed);
				const double Shore = GetShoreLevel(X, Y, WorldSeed);
				if (bWater ? Shore > Height + 2.0 : (Height > Shore + 2.0 && TerrainNormal(X, Y, WorldSeed).Z > 0.95))
				{
					return Vec3{ X, Y, std::max(Height, Shore) };
				}
			}
			return Vec3{};
		};
		const Vec3 Land = Ground(false);
		const Vec3 Lake = Ground(true);

		// Fireflies: 3 at the density that makes them, and one more for each 3 kg/m3 over.
		MagicShows Light;
		Light.Update(Around(Land, EMagicType::Light, 46.0, 20.0), 0.0, 0.05);
		std::printf("       light magic at 46 kg/m3 calls up %zu fireflies\n", Light.GetFireflies().size());
		Expect(Light.GetFireflies().size() == 5, "light magic over 40 makes fireflies, 3 and one more for each 3 kg/m3 over");
		MagicShows Faint;
		Faint.Update(Around(Land, EMagicType::Light, 39.0, 20.0), 0.0, 0.05);
		Expect(Faint.GetFireflies().empty(), "and none short of that");

		// Ten together make a big one, which sends out waves of light magic.
		MagicShows Gathering;
		const MagicVolume Dense = Around(Land, EMagicType::Light, 40.0 + 3.0 * 7.0, 20.0);
		bool bBig = false;
		std::vector<MagicDeposit> Sent;
		for (int Step = 0; Step < 200 && !bBig; ++Step)
		{
			Gathering.Update(Dense, Step * 0.05, 0.05);
			bBig = std::any_of(Gathering.GetFireflies().begin(), Gathering.GetFireflies().end(), [](const Firefly& Each) { return Each.bBig; });
		}
		for (int Step = 200; Step < 240; ++Step)
		{
			Gathering.Update(Dense, Step * 0.05, 0.05);
		}
		Gathering.TakeDeposits(Sent);
		std::printf("       ten fireflies made a big one: %s, and it let out %zu cells of light magic\n", bBig ? "yes" : "no", Sent.size());
		Expect(bBig, "ten fireflies together make a big one");
		Expect(!Sent.empty() && Sent.front().Type == EMagicType::Light && Sent.front().Density == 1.0, "which sends out waves of light magic of density 1");

		// Dense air magic over the ground spins a whirlwind.
		MagicShows Air;
		Air.Update(Around(Land, EMagicType::Air, 45.0), 0.0, 0.05);
		const Vec3 Whirl = Air.GetWhirl(Land + Vec3{ 3.0, 0.0, 2.0 });
		std::printf("       air magic at 45 kg/m3 spins a whirlwind of %zu, wind %.1f m/s 3 m out\n", Air.GetWhirlwinds().size(), Length(Whirl));
		Expect(!Air.GetWhirlwinds().empty() && Length(Whirl) > 3.0, "dense air magic spins a whirlwind");

		// Dense earth magic makes minerals, dense nature magic plants, and dense water magic a current.
		std::vector<MagicGrowth> Made;
		MagicShows Earth;
		Earth.Update(Around(Land, EMagicType::Earth, 60.0), 0.0, 0.05);
		Earth.TakeGrowths(Made);
		Expect(Made.size() == 1 && Made.front().Kind == EMagicGrowth::Minerals, "dense earth magic makes minerals");
		MagicShows Nature;
		Made.clear();
		Nature.Update(Around(Land, EMagicType::Nature, 45.0), 0.0, 0.05);
		Nature.TakeGrowths(Made);
		Expect(Made.size() == 1 && Made.front().Kind == EMagicGrowth::Plant, "dense nature magic makes plants");
		MagicShows Water;
		MagicVolume Pool = Around(Lake, EMagicType::Water, 50.0);
		Pool.Flow[0].assign(512, 1.0f);
		Water.Update(Pool, 0.0, 0.05);
		const Vec3 Current = Water.GetCurrent(Lake + Vec3{ 1.0, 1.0, 0.0 });
		std::printf("       water magic at 50 kg/m3 drives a current of %.2f m/s\n", Length(Current));
		Expect(Current.X > 0.5, "dense water magic drives a current the way it goes");
	}
}

int main(int Count, char** Args)
{
	if (Count > 1 && std::strcmp(Args[1], "fire-around") == 0)
	{
		TestFireAround();
		std::printf("%d failure(s)\n", Failures);
		return Failures == 0 ? 0 : 1;
	}

	TestTerrain();
	TestSpawn();
	TestSurfaces();
	TestWind();
	TestTurbulence();
	TestDayCycle();
	TestSkyMeetings();
	TestStarMap();
	TestLakes();
	TestBiomes();
	TestConditions();
	TestPlants();
	TestLiquid();
	TestThrowHold();
	TestMagic();
	TestMagicShows();
	TestFireAround();
	TestStorms();
	TestWaves();
	TestGaze();
	TestForces();
	TestThrowAngle();
	std::printf("%d failure(s)\n", Failures);
	return Failures == 0 ? 0 : 1;
}
