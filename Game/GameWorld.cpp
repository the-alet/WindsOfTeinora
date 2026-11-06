#include "GameWorld.h"

#include "TeinoraSim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

namespace Teinora
{
	namespace
	{
		namespace P = Physics;

		constexpr double ChunkSize = 256.0;
		constexpr double SkirtDepth = 25.0;
		constexpr int RockAttempts = 6;
		constexpr int PropAttempts = 6;
		constexpr double LoneTrees = 0.025;     // the chance of a tree out in the open, where a wood would be sure of one
		constexpr double BrushHeight = 0.3;     // what rolls through grass this close to the ground brushes through it
		constexpr double BrushDrag = 1.3;       // per second
		constexpr double GroundContact = 0.06;  // this near counts as touching the ground

		/** One step of the Geometry Detail setting. Where two values are given, they are for near and far chunks. */
		struct DetailLevel
		{
			int Cells[2];     // terrain grid cells along a chunk side
			int NearRadius;   // rings of chunks at full detail, with physics
			int TreeAttempts; // to place a tree, in the thickest wood. Most fail, by design
			int BushAttempts;
			PlantDetail Plants[2];
			int RockSides[2];
			int RockRings;
			int GrassTufts;   // per chunk, near chunks only
		};

		// Far plants keep their outline with a few big cards, so that a wood does not change shape as it comes nearer.
		const DetailLevel DetailLevels[] = {
			{ { 32, 8 },   2, 70,  10, { { 5, 1, 0, 8, 1.7, 1.45 },   { 3, 1, 0, 4, 1.0, 1.7 } },  { 5, 4 },  2, 0 },
			{ { 64, 16 },  2, 100, 16, { { 6, 2, 3, 12, 1.7, 1.05 },  { 3, 1, 0, 5, 1.3, 1.6 } },  { 6, 5 },  2, 0 },
			{ { 128, 32 }, 3, 150, 24, { { 8, 2, 4, 17, 2.4, 0.85 },  { 4, 1, 0, 6, 1.7, 1.5 } },  { 10, 7 }, 3, 12000 },
			{ { 256, 64 }, 4, 220, 36, { { 10, 3, 4, 24, 1.7, 0.8 },  { 4, 1, 0, 7, 2.0, 1.45 } }, { 14, 9 }, 5, 48000 } };
		constexpr int MaxBuildsInFlight = 8;
		constexpr int MaxAppliesPerFrame = 4;
		constexpr std::size_t MaxBalls = 48;
		constexpr double ThrowStart = 2.0; // how far ahead of the eye a ball appears
		constexpr double ThrowReach = 300.0;
		constexpr double AirReadingAge = 1.0 / 30.0;

		/** Mass follows from what a thing is made of and how big it is, inertia from mass and shape. */
		struct PropSpec
		{
			P::Shape Collider;
			double Density; // kg/m^3 over the whole shape, so a crate's counts the air inside it
			double Friction;
			double Restitution;
			double DragCoefficient;
			double FrontalArea;
			double HalfHeight;
			bool bRolls;
			EPrimitive Primitive;
			Float3 Scale;
			Float3 Color;

			double GetVolume() const { return Collider.GetVolume(); }
			double GetMass() const { return Density * GetVolume(); }
		};

		const PropSpec& GetPropSpec(EPropKind Kind)
		{
			static const std::array<PropSpec, static_cast<std::size_t>(EPropKind::Count)> Specs = { {
				{ P::Shape::MakeBox({ 0.5, 0.5, 0.5 }), 120.0, 0.6, 0.15, 1.05, 1.0, 0.5, false, EPrimitive::Cube, { 1.0f, 1.0f, 1.0f }, { 0.45f, 0.28f, 0.12f } },
				{ P::Shape::MakeCapsule(0.3, 0.3), 100.0, 0.5, 0.2, 0.9, 0.5, 0.45, true, EPrimitive::Cylinder, { 0.6f, 0.6f, 1.2f }, { 0.30f, 0.14f, 0.06f } },
				{ P::Shape::MakeSphere(0.6), 2700.0, 0.9, 0.05, 0.47, 1.1, 0.6, true, EPrimitive::Sphere, { 1.2f, 1.2f, 1.2f }, { 0.40f, 0.40f, 0.42f } },
				{ P::Shape::MakeSphere(0.7), 2.1, 0.4, 0.8, 0.47, 1.5, 0.7, true, EPrimitive::Sphere, { 1.4f, 1.4f, 1.4f }, { 0.85f, 0.12f, 0.10f } },
				{ P::Shape::MakeSphere(0.35), 45.0, 0.5, 0.5, 0.47, 0.38, 0.35, true, EPrimitive::Sphere, { 0.7f, 0.7f, 0.7f }, { 0.95f, 0.75f, 0.15f } },
			} };
			return Specs[static_cast<std::size_t>(Kind)];
		}

		std::uint64_t ChunkKey(int X, int Y)
		{
			return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(X)) << 32) | static_cast<std::uint32_t>(Y);
		}

		int ToChunk(double Value)
		{
			return static_cast<int>(std::floor(Value / ChunkSize));
		}

		/** Small and reproducible, so a chunk regrows the same objects when it reloads. */
		class Random
		{
		public:
			explicit Random(std::uint64_t Seed) : State(Seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}

			double Next()
			{
				State ^= State << 13;
				State ^= State >> 7;
				State ^= State << 17;
				return static_cast<double>(State >> 11) / 9007199254740992.0;
			}

			double Range(double Min, double Max) { return Min + (Max - Min) * Next(); }

		private:
			std::uint64_t State;
		};

		std::uint8_t ToByte(double Value)
		{
			return static_cast<std::uint8_t>(std::clamp(Value, 0.0, 1.0) * 255.0);
		}

		struct Color
		{
			double R;
			double G;
			double B;
		};

		TerrainVertex MakeVertex(const Vec3& Position, const Vec3& Normal, const Color& Tint, double Sway, ETerrainMaterial Material = TerrainPlain)
		{
			TerrainVertex Vertex;
			Vertex.Position[0] = static_cast<float>(Position.X);
			Vertex.Position[1] = static_cast<float>(Position.Y);
			Vertex.Position[2] = static_cast<float>(Position.Z);
			Vertex.Normal[0] = static_cast<std::int8_t>(Normal.X * 127.0);
			Vertex.Normal[1] = static_cast<std::int8_t>(Normal.Y * 127.0);
			Vertex.Normal[2] = static_cast<std::int8_t>(Normal.Z * 127.0);
			Vertex.Normal[3] = Material;
			Vertex.Color[0] = ToByte(Tint.R);
			Vertex.Color[1] = ToByte(Tint.G);
			Vertex.Color[2] = ToByte(Tint.B);
			Vertex.Color[3] = ToByte(Sway);
			return Vertex;
		}

		// Vertices are not shared between triangles, which gives trees and rocks their flat facets.
		void AddFacet(std::vector<TerrainVertex>& Out, const Vec3& A, const Vec3& B, const Vec3& C, const Color& Tint, double SwayA = 0.0, double SwayB = 0.0, double SwayC = 0.0, ETerrainMaterial Material = TerrainPlain)
		{
			const Vec3 Normal = Normalize(Cross(B - A, C - A));
			Out.push_back(MakeVertex(A, Normal, Tint, SwayA, Material));
			Out.push_back(MakeVertex(B, Normal, Tint, SwayB, Material));
			Out.push_back(MakeVertex(C, Normal, Tint, SwayC, Material));
		}

		Color Scale(const Color& Tint, double Amount)
		{
			return { Tint.R * Amount, Tint.G * Amount, Tint.B * Amount };
		}

		/** A two-sided blade lit with the ground's normal, so grass blends into the terrain under it. Dark at the root. */
		void AddBlade(std::vector<TerrainVertex>& Out, const Vec3& A, const Vec3& B, const Vec3& Tip, const Vec3& Normal, const Color& Tint)
		{
			const Color Root = Scale(Tint, 0.55);
			const Color End = Scale(Tint, 1.3);
			Out.push_back(MakeVertex(A, Normal, Root, 0.0));
			Out.push_back(MakeVertex(B, Normal, Root, 0.0));
			Out.push_back(MakeVertex(Tip, Normal, End, 0.35));
			Out.push_back(MakeVertex(B, Normal, Root, 0.0));
			Out.push_back(MakeVertex(A, Normal, Root, 0.0));
			Out.push_back(MakeVertex(Tip, Normal, End, 0.35));
		}

		/** An open cone, or a frustum when TopRadius is positive. Sway is the weight at the top. */
		void AddFrustum(std::vector<TerrainVertex>& Out, const Vec3& Base, double BaseRadius, double TopRadius, double Height, int Sides, const Color& Tint, double BaseSway, double TopSway, ETerrainMaterial Material = TerrainPlain)
		{
			const Vec3 Top = Base + Vec3{ 0.0, 0.0, Height };
			for (int Side = 0; Side < Sides; ++Side)
			{
				const double Angle0 = 2.0 * Pi * Side / Sides;
				const double Angle1 = 2.0 * Pi * (Side + 1) / Sides;
				const Vec3 Ray0{ std::cos(Angle0), std::sin(Angle0), 0.0 };
				const Vec3 Ray1{ std::cos(Angle1), std::sin(Angle1), 0.0 };

				const Vec3 Bottom0 = Base + Ray0 * BaseRadius;
				const Vec3 Bottom1 = Base + Ray1 * BaseRadius;
				const Vec3 Top0 = Top + Ray0 * TopRadius;
				AddFacet(Out, Bottom0, Bottom1, Top0, Tint, BaseSway, BaseSway, TopSway, Material);
				if (TopRadius > 0.0)
				{
					AddFacet(Out, Bottom1, Top + Ray1 * TopRadius, Top0, Tint, BaseSway, TopSway, TopSway, Material);
				}
			}
		}

		/** The ground's broad variation, what the textures, which repeat, cannot show, and where the water stands. */
		Color GroundTint(double WorldX, double WorldY, double Height, double Moisture)
		{
			const double Shade = 1.0 + 0.16 * Sim::GradientNoise(WorldX / 37.0, WorldY / 37.0, Sim::WorldSeed + 22u);
			const double Patch = 0.5 + 0.5 * Sim::GradientNoise(WorldX / 90.0, WorldY / 90.0, Sim::WorldSeed + 21u);
			const double Lush = std::clamp(Moisture * 1.15 - 0.1 + (Patch - 0.5) * 0.45, 0.0, 1.0);
			return { Shade * 0.5, Lush, EncodeGroundLevel(Height - Sim::FindWater(WorldX, WorldY, Sim::WorldSeed).Level) };
		}

		Sim::ESurface ClassifyFacet(const Vec3& A, const Vec3& B, const Vec3& C, double OriginX, double OriginY)
		{
			const double Level = Sim::FindWater(OriginX + (A.X + B.X + C.X) / 3.0, OriginY + (A.Y + B.Y + C.Y) / 3.0, Sim::WorldSeed).Level;
			return Sim::ClassifySurface((A.Z + B.Z + C.Z) / 3.0, Normalize(Cross(B - A, C - A)).Z, Level);
		}

		EPlantKind PickTree(const Sim::Biome& Place, double Roll, double Second)
		{
			if (Roll < Place.Conifers)
			{
				// Firs in the cold, pines where it is drier.
				return Second < 0.75 - 0.9 * (Place.Warmth - 0.3) ? EPlantKind::Fir : EPlantKind::Pine;
			}
			return Second < 0.3 + 0.5 * Place.Moisture ? EPlantKind::Birch : EPlantKind::Oak;
		}

		double PickTreeHeight(EPlantKind Kind, double Roll, double Young)
		{
			double Low = 9.0;
			double High = 17.0;
			switch (Kind)
			{
			case EPlantKind::Fir:   Low = 11.0; High = 25.0; break;
			case EPlantKind::Pine:  Low = 12.0; High = 22.0; break;
			case EPlantKind::Birch: Low = 10.0; High = 19.0; break;
			default: break;
			}

			// Among the grown trees, a share of saplings.
			return (Low + (High - Low) * Roll) * (Young < 0.22 ? 0.45 : 1.0);
		}

		// Hangs a wall under a chunk edge to hide cracks against a chunk of different detail.
		void AddSkirt(std::vector<TerrainVertex>& Out, const Vec3& A, const Vec3& B)
		{
			const Vec3 Drop{ 0.0, 0.0, SkirtDepth };
			const Color Tint{ 0.28, 0.26, 0.24 };
			AddFacet(Out, A, B, B - Drop, Tint);
			AddFacet(Out, A, B - Drop, A - Drop, Tint);
			AddFacet(Out, B - Drop, B, A, Tint);
			AddFacet(Out, A - Drop, B - Drop, A, Tint);
		}
	}

	GameWorld::GameWorld(IChunkDisplay& InDisplay, Physics::World& InPhysics, int InViewRadius, int InDetail)
		: Display(InDisplay)
		, Physics(InPhysics)
		, ViewRadius(InViewRadius)
		, Detail(InDetail)
		, Liquid(Sim::WorldSeed)
		, Finished(std::make_shared<ResultQueue>())
		, Workers(std::max(2u, std::thread::hardware_concurrency() / 2))
	{
		std::array<P::SurfaceMaterial, static_cast<std::size_t>(Sim::ESurface::Count)> Surfaces;
		for (std::size_t Index = 0; Index < Surfaces.size(); ++Index)
		{
			const Sim::ESurface Surface = static_cast<Sim::ESurface>(Index);
			const Sim::SurfaceGive Give = Sim::GetSurfaceGive(Surface);
			Surfaces[Index] = { Sim::SurfaceFriction(Surface), Give.Stiffness, Give.Damping, Give.Restitution };
		}
		Physics.SetSurfaces(Surfaces.data(), static_cast<int>(Surfaces.size()));
	}

	GameWorld::~GameWorld() = default;

	void GameWorld::SetViewRadius(int Chunks_)
	{
		ViewRadius = Chunks_;
	}

	double GameWorld::GetViewDistance() const
	{
		return ViewRadius * ChunkSize;
	}

	void GameWorld::SetDetail(int Level)
	{
		Detail = Level;
	}

	void GameWorld::BuildTerrain(ChunkBuild& Build)
	{
		const int Cells = DetailLevels[Build.Detail].Cells[Build.Lod];
		const int Side = Cells + 1;
		const double Cell = ChunkSize / Cells;
		const Vec3 Origin{ Build.X * ChunkSize, Build.Y * ChunkSize, 0.0 };

		// One ring more than the grid, so that normals agree across the edges of chunks.
		const int Bordered = Side + 2;
		std::vector<double> Field(static_cast<std::size_t>(Bordered) * Bordered);
		const auto FieldAt = [&Field, Bordered](int X, int Y) -> double&
		{
			return Field[static_cast<std::size_t>(X + 1 + (Y + 1) * Bordered)];
		};
		for (int Y = -1; Y <= Side; ++Y)
		{
			for (int X = -1; X <= Side; ++X)
			{
				FieldAt(X, Y) = Sim::TerrainHeight(Origin.X + X * Cell, Origin.Y + Y * Cell, Sim::WorldSeed);
			}
		}

		// How damp the land is changes slowly, and is only worked out here and there.
		constexpr int DampCells = 8;
		double Damp[DampCells + 1][DampCells + 1];
		for (int Y = 0; Y <= DampCells; ++Y)
		{
			for (int X = 0; X <= DampCells; ++X)
			{
				const double WorldX = Origin.X + ChunkSize * X / DampCells;
				const double WorldY = Origin.Y + ChunkSize * Y / DampCells;
				Damp[Y][X] = Sim::ReadBiome(WorldX, WorldY, FieldAt(Cells * X / DampCells, Cells * Y / DampCells), Sim::WorldSeed).Moisture;
			}
		}
		const auto ReadDamp = [&Damp, Cells](int X, int Y)
		{
			const double GridX = static_cast<double>(X) / Cells * DampCells;
			const double GridY = static_cast<double>(Y) / Cells * DampCells;
			const int Column = std::min(static_cast<int>(GridX), DampCells - 1);
			const int Row = std::min(static_cast<int>(GridY), DampCells - 1);
			const double PartX = GridX - Column;
			const double PartY = GridY - Row;
			const double Low = Damp[Row][Column] + (Damp[Row][Column + 1] - Damp[Row][Column]) * PartX;
			const double High = Damp[Row + 1][Column] + (Damp[Row + 1][Column + 1] - Damp[Row + 1][Column]) * PartX;
			return Low + (High - Low) * PartY;
		};

		std::vector<Vec3> Grid(static_cast<std::size_t>(Side) * Side);
		std::vector<TerrainVertex> Corners(Grid.size());
		double Lowest = 1e9;
		double Highest = -1e9;
		for (int Y = 0; Y < Side; ++Y)
		{
			for (int X = 0; X < Side; ++X)
			{
				const std::size_t Index = static_cast<std::size_t>(X + Y * Side);
				const double Height = FieldAt(X, Y);
				const Vec3 Normal = Normalize(Vec3{ FieldAt(X - 1, Y) - FieldAt(X + 1, Y), FieldAt(X, Y - 1) - FieldAt(X, Y + 1), 2.0 * Cell });

				Grid[Index] = { X * Cell, Y * Cell, Height };
				Corners[Index] = MakeVertex(Grid[Index], Normal, GroundTint(Origin.X + X * Cell, Origin.Y + Y * Cell, Height, ReadDamp(X, Y)), 0.0, TerrainGround);
				Lowest = std::min(Lowest, Height);
				Highest = std::max(Highest, Height);
			}
		}

		const auto At = [&Grid, Side](int X, int Y) -> const Vec3&
		{
			return Grid[static_cast<std::size_t>(X + Y * Side)];
		};
		const auto AddCorner = [&Build, &Corners, Side](int X, int Y)
		{
			Build.Vertices.push_back(Corners[static_cast<std::size_t>(X + Y * Side)]);
		};

		Build.Vertices.reserve(static_cast<std::size_t>(Cells) * Cells * 6);
		for (int Y = 0; Y < Cells; ++Y)
		{
			for (int X = 0; X < Cells; ++X)
			{
				AddCorner(X, Y);
				AddCorner(X + 1, Y);
				AddCorner(X, Y + 1);
				AddCorner(X + 1, Y);
				AddCorner(X + 1, Y + 1);
				AddCorner(X, Y + 1);
			}
		}

		for (int Index = 0; Index < Cells; ++Index)
		{
			AddSkirt(Build.Vertices, At(Index, 0), At(Index + 1, 0));
			AddSkirt(Build.Vertices, At(Index, Cells), At(Index + 1, Cells));
			AddSkirt(Build.Vertices, At(0, Index), At(0, Index + 1));
			AddSkirt(Build.Vertices, At(Cells, Index), At(Cells, Index + 1));
		}

		if (Build.Lod == 0)
		{
			Build.Heights.resize(Grid.size());
			for (std::size_t Index = 0; Index < Grid.size(); ++Index)
			{
				Build.Heights[Index] = static_cast<float>(Grid[Index].Z);
			}

			// The same two triangles per cell as the mesh, so the ground is what is on screen.
			Build.Surfaces.reserve(static_cast<std::size_t>(Cells) * Cells * 2);
			for (int Y = 0; Y < Cells; ++Y)
			{
				for (int X = 0; X < Cells; ++X)
				{
					Build.Surfaces.push_back(static_cast<std::uint8_t>(ClassifyFacet(At(X, Y), At(X + 1, Y), At(X, Y + 1), Origin.X, Origin.Y)));
					Build.Surfaces.push_back(static_cast<std::uint8_t>(ClassifyFacet(At(X + 1, Y), At(X + 1, Y + 1), At(X, Y + 1), Origin.X, Origin.Y)));
				}
			}
		}

		Build.MinZ = static_cast<float>(Lowest - SkirtDepth);
		Build.MaxZ = static_cast<float>(Highest + 34.0); // room for the tallest tree
	}

	void GameWorld::ScatterObjects(ChunkBuild& Build)
	{
		Random Rng(ChunkKey(Build.X, Build.Y));
		const Vec3 Origin{ Build.X * ChunkSize, Build.Y * ChunkSize, 0.0 };
		const DetailLevel& Level = DetailLevels[Build.Detail];
		const PlantDetail& Plants = Level.Plants[Build.Lod];
		const int RockSides = Level.RockSides[Build.Lod];

		const auto RandomPoint = [&Rng]()
		{
			return Vec3{ Rng.Range(0.0, ChunkSize), Rng.Range(0.0, ChunkSize), 0.0 };
		};

		PlantOutput Output;
		Output.Solid = &Build.Vertices;
		Output.Leaves = &Build.Leaves;
		Output.Limbs = Build.Lod == 0 ? &Build.Limbs : nullptr;
		Output.Foliage = Build.Lod == 0 ? &Build.Foliage : nullptr;

		// Every roll is made whether or not it is used, so that a tree grows the same at each level of detail.
		for (int Attempt = 0; Attempt < Level.TreeAttempts; ++Attempt)
		{
			const Vec3 Local = RandomPoint();
			const double WorldX = Origin.X + Local.X;
			const double WorldY = Origin.Y + Local.Y;
			const double Chance = Rng.Next();
			const double KindRoll = Rng.Next();
			const double SecondRoll = Rng.Next();
			const double SizeRoll = Rng.Next();
			const double YoungRoll = Rng.Next();
			const auto PlantSeed = static_cast<std::uint64_t>(Rng.Next() * 9007199254740992.0);

			const double Height = Sim::TerrainHeight(WorldX, WorldY, Sim::WorldSeed);
			const double WaterLevel = Sim::FindWater(WorldX, WorldY, Sim::WorldSeed).Level;
			if (Height < WaterLevel + 1.4 || Sim::ClassifySurface(Height, Sim::TerrainNormal(WorldX, WorldY, Sim::WorldSeed).Z, WaterLevel) != Sim::ESurface::Grass)
			{
				continue;
			}

			const Sim::Biome Place = Sim::ReadBiome(WorldX, WorldY, Height, Sim::WorldSeed);
			if (Chance > LoneTrees + (1.0 - LoneTrees) * Place.Forest)
			{
				continue;
			}

			const EPlantKind Kind = PickTree(Place, KindRoll, SecondRoll);
			GrowPlant(Kind, Vec3{ Local.X, Local.Y, Height }, Origin, PickTreeHeight(Kind, SizeRoll, YoungRoll), Plants, PlantSeed, Output);
			if (Build.Lod == 0)
			{
				const double Standing = std::max(Output.TrunkHeight - Output.TrunkRadius * 2.0, 0.2);
				Build.Statics.push_back({ P::Shape::MakeCapsule(Output.TrunkRadius, Standing * 0.5), Vec3{ WorldX, WorldY, Height + Standing * 0.5 } });
			}
		}

		for (int Attempt = 0; Attempt < Level.BushAttempts; ++Attempt)
		{
			const Vec3 Local = RandomPoint();
			const double WorldX = Origin.X + Local.X;
			const double WorldY = Origin.Y + Local.Y;
			const double Chance = Rng.Next();
			const double SizeRoll = Rng.Next();
			const auto PlantSeed = static_cast<std::uint64_t>(Rng.Next() * 9007199254740992.0);

			const double Height = Sim::TerrainHeight(WorldX, WorldY, Sim::WorldSeed);
			const double WaterLevel = Sim::FindWater(WorldX, WorldY, Sim::WorldSeed).Level;
			if (Height < WaterLevel + 0.9 || Sim::ClassifySurface(Height, Sim::TerrainNormal(WorldX, WorldY, Sim::WorldSeed).Z, WaterLevel) != Sim::ESurface::Grass)
			{
				continue;
			}

			// Thickest along the edges of woods and wherever it is damp.
			const Sim::Biome Place = Sim::ReadBiome(WorldX, WorldY, Height, Sim::WorldSeed);
			const double Edge = 1.0 - std::fabs(Place.Forest - 0.45) * 2.0;
			if (Chance < 0.12 + 0.5 * std::max(Edge, 0.0) * Place.Moisture)
			{
				GrowPlant(EPlantKind::Bush, Vec3{ Local.X, Local.Y, Height }, Origin, 0.9 + 1.7 * SizeRoll, Plants, PlantSeed, Output);
			}
		}

		for (int Attempt = 0; Attempt < RockAttempts; ++Attempt)
		{
			const Vec3 Local = RandomPoint();
			const double WorldX = Origin.X + Local.X;
			const double WorldY = Origin.Y + Local.Y;
			const double Radius = Rng.Range(0.8, 3.0);
			const double Shade = Rng.Range(0.85, 1.1);

			const double Height = Sim::TerrainHeight(WorldX, WorldY, Sim::WorldSeed);
			const Vec3 Center{ Local.X, Local.Y, Height - Radius * 0.2 };
			const Color Tint{ 0.30 * Shade, 0.29 * Shade, 0.28 * Shade };

			// Stacked rings approximating the sphere that collides, with a skirt below for slopes.
			AddFrustum(Build.Vertices, Center - Vec3{ 0.0, 0.0, Radius * 0.5 }, Radius * 0.87, Radius, Radius * 0.5, RockSides, Tint, 0.0, 0.0, TerrainStone);
			for (int Ring = 0; Ring < Level.RockRings; ++Ring)
			{
				const double Lower = Pi * 0.5 * Ring / Level.RockRings;
				const double Upper = Pi * 0.5 * (Ring + 1) / Level.RockRings;
				const double TopRadius = Ring + 1 == Level.RockRings ? 0.0 : Radius * std::cos(Upper);
				AddFrustum(Build.Vertices, Center + Vec3{ 0.0, 0.0, Radius * std::sin(Lower) }, Radius * std::cos(Lower), TopRadius,
					Radius * (std::sin(Upper) - std::sin(Lower)), RockSides, Tint, 0.0, 0.0, TerrainStone);
			}

			if (Build.Lod == 0)
			{
				Build.Statics.push_back({ P::Shape::MakeSphere(Radius), Vec3{ WorldX, WorldY, Center.Z } });
			}
		}

		if (Build.Lod == 0)
		{
			for (int Attempt = 0; Attempt < PropAttempts; ++Attempt)
			{
				const Vec3 Local = RandomPoint();
				const double WorldX = Origin.X + Local.X;
				const double WorldY = Origin.Y + Local.Y;
				const double Height = std::max(Sim::TerrainHeight(WorldX, WorldY, Sim::WorldSeed), Sim::FindWater(WorldX, WorldY, Sim::WorldSeed).Level);

				PropSpawn Spawn;
				Spawn.Kind = static_cast<EPropKind>(static_cast<int>(Rng.Next() * 4.0));
				Spawn.Position = { WorldX, WorldY, Height + 2.0 };
				Build.Props.push_back(Spawn);
			}
		}
	}

	void GameWorld::GrowGrass(ChunkBuild& Build)
	{
		const int Tufts = DetailLevels[Build.Detail].GrassTufts;
		if (Build.Lod != 0 || Tufts == 0)
		{
			return;
		}

		Random Rng(ChunkKey(Build.X, Build.Y) ^ 0x5151u);
		const int Cells = DetailLevels[Build.Detail].Cells[0];
		const int Side = Cells + 1;
		const double Cell = ChunkSize / Cells;
		Build.Vertices.reserve(Build.Vertices.size() + static_cast<std::size_t>(Tufts) * 18);

		for (int Tuft = 0; Tuft < Tufts; ++Tuft)
		{
			const double X = Rng.Range(0.0, ChunkSize - 0.01);
			const double Y = Rng.Range(0.0, ChunkSize - 0.01);
			const int CellX = static_cast<int>(X / Cell);
			const int CellY = static_cast<int>(Y / Cell);
			const double PartX = X / Cell - CellX;
			const double PartY = Y / Cell - CellY;

			// The same triangle split as the mesh, so blades stand on the facet below them.
			const double H00 = Build.Heights[static_cast<std::size_t>(CellX + CellY * Side)];
			const double H10 = Build.Heights[static_cast<std::size_t>(CellX + 1 + CellY * Side)];
			const double H01 = Build.Heights[static_cast<std::size_t>(CellX + (CellY + 1) * Side)];
			const double H11 = Build.Heights[static_cast<std::size_t>(CellX + 1 + (CellY + 1) * Side)];
			const bool bLower = PartX + PartY <= 1.0;
			const double SlopeX = bLower ? H10 - H00 : H11 - H01;
			const double SlopeY = bLower ? H01 - H00 : H11 - H10;
			const double Height = bLower ? H00 + SlopeX * PartX + SlopeY * PartY : H11 - SlopeX * (1.0 - PartX) - SlopeY * (1.0 - PartY);

			const Vec3 Normal = Normalize(Vec3{ -SlopeX / Cell, -SlopeY / Cell, 1.0 });
			const double Shade = Rng.Range(0.85, 1.25);
			const double Facing = Rng.Range(0.0, Pi);

			// Reeds stand in the shallows and along the edge of the water, where grass gives out.
			const Sim::WaterBody Water = Sim::FindWater(X + Build.X * ChunkSize, Y + Build.Y * ChunkSize, Sim::WorldSeed);
			const double WaterLevel = Water.Level;
			const double Above = Height - WaterLevel;
			const bool bReeds = Water.bLake && WaterLevel < 140.0 && Above > -0.8 && Above < 0.7 && Normal.Z > 0.9 && Shade < 1.0;
			if (!bReeds && Sim::ClassifySurface(Height, Normal.Z, WaterLevel) != Sim::ESurface::Grass)
			{
				continue;
			}

			const Color Tint = bReeds ? Color{ 0.10 * Shade, 0.15 * Shade, 0.05 * Shade } : Color{ 0.065 * Shade, 0.15 * Shade, 0.03 * Shade };
			const double Width = bReeds ? 0.03 : 0.045;
			const double Lean = bReeds ? 0.22 : 0.14;
			for (int Blade = 0; Blade < 3; ++Blade)
			{
				const double Angle = Facing + Blade * Pi / 3.0;
				const Vec3 Across{ std::cos(Angle) * Width, std::sin(Angle) * Width, 0.0 };
				const Vec3 Leaning{ -std::sin(Angle) * Lean, std::cos(Angle) * Lean, 0.0 };
				const Vec3 Root{ X, Y, Height - 0.03 };
				const Vec3 Tip = Root + Leaning + Vec3{ 0.0, 0.0, bReeds ? std::max(WaterLevel + 0.6 - Height, 0.0) + Rng.Range(0.6, 1.4) : Rng.Range(0.22, 0.55) };
				AddBlade(Build.Vertices, Root - Across, Root + Across, Tip, Normal, Tint);
			}
		}
	}

	std::unique_ptr<GameWorld::ChunkBuild> GameWorld::BuildChunk(int X, int Y, int Lod, int Detail)
	{
		auto Build = std::make_unique<ChunkBuild>();
		Build->X = X;
		Build->Y = Y;
		Build->Lod = Lod;
		Build->Detail = Detail;
		BuildTerrain(*Build);
		ScatterObjects(*Build);
		Build->SolidVertexCount = static_cast<std::uint32_t>(Build->Vertices.size());
		Build->Vertices.insert(Build->Vertices.end(), Build->Leaves.begin(), Build->Leaves.end());
		Build->Leaves = {};
		Build->ShadowVertexCount = static_cast<std::uint32_t>(Build->Vertices.size());
		GrowGrass(*Build);
		return Build;
	}

	void GameWorld::Step(double Dt)
	{
		Physics.Step(Dt);
		Plants.Advance(Dt);
		Liquid.Advance(Dt);
	}

	void GameWorld::Update(const Vec3& Focus, double Time)
	{
		const int CenterX = ToChunk(Focus.X);
		const int CenterY = ToChunk(Focus.Y);

		AirDrift += Sim::PrevailingWind(Time, Sim::WorldSeed) * (Time - LastUpdateTime);
		LastUpdateTime = Time;

		Liquid.Follow(Focus);
		RequestChunks(CenterX, CenterY);
		ApplyFinishedChunks();
		CullProps(Time);
	}

	void GameWorld::RequestChunks(int CenterX, int CenterY)
	{
		struct Request
		{
			int X;
			int Y;
			int Lod;
			int Ring;
		};
		std::vector<Request> Requests;

		const auto RingOf = [CenterX, CenterY](int X, int Y)
		{
			return std::max(std::abs(X - CenterX), std::abs(Y - CenterY));
		};

		for (int Y = CenterY - ViewRadius; Y <= CenterY + ViewRadius; ++Y)
		{
			for (int X = CenterX - ViewRadius; X <= CenterX + ViewRadius; ++X)
			{
				const std::uint64_t Key = ChunkKey(X, Y);
				if (InFlight.contains(Key))
				{
					continue;
				}

				const auto Existing = Chunks.find(Key);
				const int CurrentLod = Existing != Chunks.end() ? Existing->second.Lod : -1;
				const bool bOutdated = Existing != Chunks.end() && Existing->second.Detail != Detail;
				const int Ring = RingOf(X, Y);

				// One extra ring of hysteresis stops rebuilds when moving along a border.
				const int NearRadius = DetailLevels[Detail].NearRadius;
				const bool bNear = Ring <= NearRadius || (CurrentLod == 0 && Ring <= NearRadius + 1);
				const int WantedLod = bNear ? 0 : 1;
				if (CurrentLod != WantedLod || bOutdated)
				{
					Requests.push_back({ X, Y, WantedLod, Ring });
				}
			}
		}

		bSettled = Requests.empty() && InFlight.empty();

		std::sort(Requests.begin(), Requests.end(), [](const Request& A, const Request& B) { return A.Ring < B.Ring; });
		for (const Request& Item : Requests)
		{
			if (static_cast<int>(InFlight.size()) >= MaxBuildsInFlight)
			{
				break;
			}

			InFlight[ChunkKey(Item.X, Item.Y)] = Item.Lod;
			Workers.Enqueue([Results = Finished, X = Item.X, Y = Item.Y, Lod = Item.Lod, Level = Detail]
			{
				std::unique_ptr<ChunkBuild> Build = BuildChunk(X, Y, Lod, Level);
				std::lock_guard Lock(Results->Mutex);
				Results->Items.push_back(std::move(Build));
			});
		}

		for (auto It = Chunks.begin(); It != Chunks.end();)
		{
			const int X = static_cast<std::int32_t>(It->first >> 32);
			const int Y = static_cast<std::int32_t>(It->first & 0xFFFFFFFFu);
			if (RingOf(X, Y) > ViewRadius + 1 && !InFlight.contains(It->first))
			{
				RemoveColliders(It->second);
				Plants.RemoveChunk(It->first);
				Display.HideChunk(X, Y);
				It = Chunks.erase(It);
			}
			else
			{
				++It;
			}
		}
	}

	void GameWorld::ApplyFinishedChunks()
	{
		for (int Applied = 0; Applied < MaxAppliesPerFrame; ++Applied)
		{
			std::unique_ptr<ChunkBuild> Build;
			{
				std::lock_guard Lock(Finished->Mutex);
				if (Finished->Items.empty())
				{
					return;
				}
				Build = std::move(Finished->Items.front());
				Finished->Items.pop_front();
			}
			ApplyChunk(*Build);
		}
	}

	void GameWorld::ApplyChunk(ChunkBuild& Build)
	{
		const std::uint64_t Key = ChunkKey(Build.X, Build.Y);
		InFlight.erase(Key);

		Chunk& Target = Chunks[Key];
		RemoveColliders(Target);

		// A chunk rebuilt at a new detail level is still near: its props are already out there.
		const bool bSpawnProps = Target.Lod != 0;

		Target.Lod = Build.Lod;
		Target.Detail = Build.Detail;

		ChunkMesh Mesh;
		Mesh.Origin = { Build.X * ChunkSize, Build.Y * ChunkSize, 0.0 };
		Mesh.Size = static_cast<float>(ChunkSize);
		Mesh.MinZ = Build.MinZ;
		Mesh.MaxZ = Build.MaxZ;
		Mesh.Vertices = Build.Vertices.data();
		Mesh.VertexCount = static_cast<std::uint32_t>(Build.Vertices.size());
		Mesh.SolidVertexCount = Build.SolidVertexCount;
		Mesh.ShadowVertexCount = Build.ShadowVertexCount;
		Display.ShowChunk(Build.X, Build.Y, Mesh);

		Target.Heights.clear();
		Target.Surfaces.clear();
		if (!Build.Heights.empty())
		{
			P::GroundDesc Ground;
			Ground.OriginX = Build.X * ChunkSize;
			Ground.OriginY = Build.Y * ChunkSize;
			Ground.Cells = DetailLevels[Build.Detail].Cells[0];
			Ground.CellSize = ChunkSize / Ground.Cells;
			Ground.Heights = Build.Heights.data();
			Ground.Surfaces = Build.Surfaces.data();
			Target.Ground = Physics.AddGround(Ground);

			Target.Cells = Ground.Cells;
			Target.Heights = std::move(Build.Heights);
			Target.Surfaces = std::move(Build.Surfaces);
			Plants.AddChunk(Key, std::move(Build.Limbs), std::move(Build.Foliage), Mesh.Origin, ChunkSize);
		}
		else
		{
			Plants.RemoveChunk(Key);
		}

		for (const StaticSpawn& Spawn : Build.Statics)
		{
			P::BodyDesc Desc;
			Desc.Collider = Spawn.Collider;
			Desc.Position = Spawn.Position;
			Desc.Friction = 0.7;
			Target.Statics.push_back(Physics.CreateBody(Desc));
		}
		if (bSpawnProps)
		{
			for (const PropSpawn& Spawn : Build.Props)
			{
				SpawnProp(Spawn.Kind, Spawn.Position, {});
			}
		}
	}

	void GameWorld::RemoveColliders(Chunk& Target)
	{
		if (Target.Ground != P::NoGround)
		{
			Physics.RemoveGround(Target.Ground);
			Target.Ground = P::NoGround;
		}
		for (const P::BodyId Id : Target.Statics)
		{
			Physics.DestroyBody(Id);
		}
		Target.Statics.clear();
	}

	bool GameWorld::HasGroundAt(double X, double Y) const
	{
		const auto Found = Chunks.find(ChunkKey(ToChunk(X), ToChunk(Y)));
		return Found != Chunks.end() && Found->second.Ground != P::NoGround;
	}

	bool GameWorld::ReadGround(double X, double Y, GroundPoint& Out) const
	{
		const int ChunkX = ToChunk(X);
		const int ChunkY = ToChunk(Y);
		const auto Found = Chunks.find(ChunkKey(ChunkX, ChunkY));
		if (Found == Chunks.end() || Found->second.Heights.empty())
		{
			return false;
		}

		const Chunk& Target = Found->second;
		const int Cells = Target.Cells;
		const int Side = Cells + 1;
		const double Cell = ChunkSize / Cells;
		const double GridX = (X - ChunkX * ChunkSize) / Cell;
		const double GridY = (Y - ChunkY * ChunkSize) / Cell;
		const int CellX = std::clamp(static_cast<int>(GridX), 0, Cells - 1);
		const int CellY = std::clamp(static_cast<int>(GridY), 0, Cells - 1);
		const double PartX = GridX - CellX;
		const double PartY = GridY - CellY;

		// The same two triangles to a cell as the mesh and the physics.
		const std::size_t Corner = static_cast<std::size_t>(CellX) + static_cast<std::size_t>(CellY) * Side;
		const double H00 = Target.Heights[Corner];
		const double H10 = Target.Heights[Corner + 1];
		const double H01 = Target.Heights[Corner + Side];
		const double H11 = Target.Heights[Corner + Side + 1];
		const bool bLower = PartX + PartY <= 1.0;
		const double SlopeX = bLower ? H10 - H00 : H11 - H01;
		const double SlopeY = bLower ? H01 - H00 : H11 - H10;

		Out.Height = bLower ? H00 + SlopeX * PartX + SlopeY * PartY : H11 - SlopeX * (1.0 - PartX) - SlopeY * (1.0 - PartY);
		Out.Normal = Normalize(Vec3{ -SlopeX / Cell, -SlopeY / Cell, 1.0 });
		Out.Surface = static_cast<Sim::ESurface>(Target.Surfaces[(static_cast<std::size_t>(CellX) + static_cast<std::size_t>(CellY) * Cells) * 2 + (bLower ? 0 : 1)]);
		return true;
	}

	void GameWorld::DisplaceWater(std::size_t Index, const Vec3& Position, double Volume, double Radius)
	{
		if (Index >= WaterByBody.size())
		{
			WaterByBody.resize(Index + 1);
		}

		// A body floating still pushes nothing new aside. The water only hears of what has changed.
		WaterReading& Last = WaterByBody[Index];
		constexpr double Share = 0.6; // of what the body holds out: some of it goes down rather than out at the surface
		if (std::fabs(Volume - Last.Volume) < 2e-3 && LengthSquared(Position - Last.Position) < 0.04)
		{
			return;
		}
		if (Last.Volume > 0.0)
		{
			Liquid.Displace(Last.Position.X, Last.Position.Y, -Last.Volume * Share, Radius);
		}
		if (Volume > 0.0)
		{
			Liquid.Displace(Position.X, Position.Y, Volume * Share, Radius);
		}
		Last.Position = Position;
		Last.Volume = Volume;
	}

	void GameWorld::SpawnProp(EPropKind Kind, const Vec3& Position, const Vec3& Velocity)
	{
		const PropSpec& Spec = GetPropSpec(Kind);

		P::BodyDesc Desc;
		Desc.Collider = Spec.Collider;
		Desc.Mass = Spec.GetMass();
		Desc.Position = Position;
		Desc.Velocity = Velocity;
		Desc.Friction = Spec.Friction;
		Desc.Restitution = Spec.Restitution;

		const P::BodyId Id = Physics.CreateBody(Desc);
		Props.push_back({ Id, Kind });

		if (static_cast<std::size_t>(Id) >= PropKindByBody.size())
		{
			PropKindByBody.resize(static_cast<std::size_t>(Id) + 1, -1);
		}
		PropKindByBody[static_cast<std::size_t>(Id)] = static_cast<int>(Kind);
		if (static_cast<std::size_t>(Id) < WaterByBody.size())
		{
			WaterByBody[static_cast<std::size_t>(Id)] = {}; // whatever used this number before has gone
		}
	}

	void GameWorld::RemoveProp(std::size_t Index)
	{
		const P::BodyId Id = Props[Index].Body;
		PropKindByBody[static_cast<std::size_t>(Id)] = -1;
		Physics.DestroyBody(Id);
		Props.erase(Props.begin() + static_cast<std::ptrdiff_t>(Index));
	}

	void GameWorld::ThrowBall(const Vec3& Eye, const Vec3& Gaze, double Speed)
	{
		const PropSpec& Spec = GetPropSpec(EPropKind::Ball);
		const Vec3 Start = Eye + Gaze * ThrowStart;

		// A ball flying along the line of sight would fall away from it at once, so the
		// launch is raised by just what it takes for the arc to come down on what is seen.
		Vec3 Velocity = Gaze * Speed;
		P::RayHit Hit;
		if (Speed > 0.0 && Physics.Raycast(Eye, Gaze, ThrowReach, Hit) && Hit.Distance > ThrowStart + 1.0)
		{
			const Vec3 Target = Hit.Point + Hit.Normal * Spec.Collider.Radius;
			const Vec3 Flat{ Target.X - Start.X, Target.Y - Start.Y, 0.0 };
			const double Distance = Length(Flat);
			const double DragPerMass = 0.5 * Sim::AirDensity * Spec.DragCoefficient * Spec.FrontalArea / Spec.GetMass();
			const double NetGravity = Sim::Gravity * (1.0 - Sim::AirDensity / Spec.Density);

			double Angle = 0.0;
			if (Distance > 0.01 && Sim::SolveThrowAngle(Distance, Target.Z - Start.Z, Speed, DragPerMass, NetGravity, Angle))
			{
				Velocity = (Flat * (std::cos(Angle) / Distance) + Vec3{ 0.0, 0.0, std::sin(Angle) }) * Speed;
			}
		}

		std::size_t Balls = 0;
		std::size_t Oldest = Props.size();
		for (std::size_t Index = Props.size(); Index > 0; --Index)
		{
			if (Props[Index - 1].Kind == EPropKind::Ball)
			{
				++Balls;
				Oldest = Index - 1;
			}
		}
		if (Balls >= MaxBalls)
		{
			RemoveProp(Oldest);
		}
		SpawnProp(EPropKind::Ball, Start, Velocity);
	}

	void GameWorld::CullProps(double Time)
	{
		for (std::size_t Index = Props.size(); Index > 0; --Index)
		{
			P::Body& Item = Physics.GetBody(Props[Index - 1].Body);

			// A body must not outlive the ground under it, or it falls for ever.
			if (!HasGroundAt(Item.Position.X, Item.Position.Y))
			{
				RemoveProp(Index - 1);
				continue;
			}

			// Sleeping bodies get no forces, so a strong enough gust has to wake them.
			if (Item.bAsleep)
			{
				const PropSpec& Spec = GetPropSpec(Props[Index - 1].Kind);
				const Vec3 Wind = Sim::WindAt(Item.Position, Time, Sim::WorldSeed);
				const double Force = Length(Sim::AeroForce(Wind, Spec.DragCoefficient, Spec.FrontalArea));
				if (Force > Spec.GetMass() * Sim::Gravity * Spec.Friction * 0.5)
				{
					Physics.WakeBody(Props[Index - 1].Body);
				}
			}
		}
	}

	void GameWorld::ApplyForces(P::BodyId Id, P::Body& Item, double Time, double Dt)
	{
		const int Kind = static_cast<std::size_t>(Id) < PropKindByBody.size() ? PropKindByBody[static_cast<std::size_t>(Id)] : -1;
		if (Kind < 0)
		{
			return;
		}

		const std::size_t Index = static_cast<std::size_t>(Id);
		if (Index >= AirByBody.size())
		{
			AirByBody.resize(Index + 1);
		}
		AirReading& Air = AirByBody[Index];
		if (Time < Air.Time || Time - Air.Time >= AirReadingAge)
		{
			Air.Flow = Sim::AirFlowAt(Item.Position, AirDrift, Time, Sim::WorldSeed);
			Air.Time = Time;
		}

		const PropSpec& Spec = GetPropSpec(static_cast<EPropKind>(Kind));
		const double WaterZ = Sim::WaterSurfaceAt(Item.Position.X, Item.Position.Y, Time, Sim::WorldSeed) + Liquid.GetHeight(Item.Position.X, Item.Position.Y);
		const double Submerged = Sim::SubmergedFraction(Item.Position.Z, Spec.HalfHeight, WaterZ);
		const double Exposed = 1.0 - Submerged;

		Item.Force += (Sim::AeroForce(Air.Flow.Velocity - Item.Velocity, Spec.DragCoefficient, Spec.FrontalArea) + Sim::AirLift(Spec.GetVolume())) * Exposed;

		// Spinning air turns what is in it, until the body keeps pace at half the vorticity.
		const double Radius = std::cbrt(Spec.GetVolume() * 3.0 / (4.0 * Pi));
		const Vec3 Torque = Sim::AeroTorque(Air.Flow.Vorticity * 0.5 - Item.AngularVelocity, Radius);
		Item.AngularAcceleration += Torque * (Exposed / (0.4 * Item.GetMass() * Radius * Radius));

		if (Submerged > 0.0)
		{
			// Water carries along what floats in it, and is shoved aside by what comes in.
			Item.Force += Sim::WaterForce(Submerged, Spec.GetVolume(), Item.Velocity - Liquid.GetFlow(Item.Position.X, Item.Position.Y), 1.2, Item.GetMass());
			Item.AngularAcceleration += Item.AngularVelocity * (-2.4 * Submerged);
		}
		DisplaceWater(Index, Item.Position, Submerged * Spec.GetVolume(), Spec.HalfHeight);

		const double Speed = Length(Item.Velocity);
		if (Speed > 0.05)
		{
			Item.Force += Plants.Touch(Item.Position, Spec.HalfHeight, Item.Velocity, Item.GetMass(), Dt);
		}

		GroundPoint Ground;
		if (!ReadGround(Item.Position.X, Item.Position.Y, Ground))
		{
			return;
		}

		const double Clearance = Item.Position.Z - Spec.HalfHeight - Ground.Height;
		if (Clearance < GroundContact && Spec.bRolls)
		{
			// A round thing rolling on soft ground pushes a small bow wave of it along, which coulomb friction knows nothing of.
			const double Rolling = Sim::GetSurfaceGive(Ground.Surface).Rolling * Sim::Gravity * Ground.Normal.Z;
			const Vec3 Along = Item.Velocity - Ground.Normal * Dot(Item.Velocity, Ground.Normal);
			const double Pace = Length(Along);
			if (Pace > 1e-3)
			{
				Item.Force -= Along * (Item.GetMass() * std::min(Rolling, Pace / Dt) / Pace);
			}

			const double Spin = Length(Item.AngularVelocity);
			if (Spin > 1e-3)
			{
				Item.AngularAcceleration -= Item.AngularVelocity * (std::min(Rolling / Spec.HalfHeight, Spin / Dt) / Spin);
			}
		}

		// And grass catches at what goes through it.
		if (Clearance < BrushHeight && Ground.Surface == Sim::ESurface::Grass)
		{
			Item.Force -= Item.Velocity * (Item.GetMass() * BrushDrag * Exposed);
		}
	}

	void GameWorld::CollectObjects(std::vector<ObjectDraw>& Out) const
	{
		for (const Prop& Item : Props)
		{
			const P::Body& Target = Physics.GetBody(Item.Body);
			const PropSpec& Spec = GetPropSpec(Item.Kind);

			ObjectDraw Draw;
			Draw.Primitive = Spec.Primitive;
			Draw.Position = Target.Position;
			Draw.Rotation = Target.Rotation;
			Draw.Scale = Spec.Scale;
			Draw.Color = Spec.Color;
			Out.push_back(Draw);
		}
	}
}
