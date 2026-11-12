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
			{ { 32, 8 },   2, 70,  10, { { 5, 1, 0, 8, 1.7, 1.45 },   { 3, 1, 0, 4, 1.0, 1.7 } },  { 8, 5 },   3, 4000 },
			{ { 64, 16 },  2, 100, 16, { { 6, 2, 3, 12, 1.7, 1.05 },  { 3, 1, 0, 5, 1.3, 1.6 } },  { 10, 6 },  4, 8000 },
			{ { 128, 32 }, 3, 150, 24, { { 8, 2, 4, 17, 2.4, 0.85 },  { 4, 1, 0, 6, 1.7, 1.5 } },  { 16, 8 },  6, 12000 },
			{ { 256, 64 }, 4, 220, 36, { { 10, 3, 4, 24, 1.7, 0.8 },  { 4, 1, 0, 7, 2.0, 1.45 } }, { 24, 10 }, 9, 48000 } };
		constexpr int MaxBuildsInFlight = 8;
		constexpr int MaxAppliesPerFrame = 4;
		constexpr std::size_t MaxBalls = 48;
		constexpr double ThrowStart = 2.0; // how far ahead of the eye a ball appears
		constexpr double ThrowReach = 300.0;
		constexpr double AirReadingAge = 1.0 / 30.0;
		constexpr double BurnTime = 60.0;       // seconds, as long as the fuel lasts
		constexpr double SteamLife = 12.0;      // seconds
		constexpr double SmokeLife = 12.0;
		constexpr double FlameReach = 26.0;     // metres that a flame's light goes
		constexpr double FireKeptInWind = 0.5;  // of the fire magic a fire's heat gathers, what a light wind leaves in it
		constexpr double DustLife = 4.0;        // seconds the dust of something that fell apart hangs in the air
		constexpr int DenseGrass = 4;           // times as many clumps of grass in the chunks right around the eye

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
			Sim::EMagicType Aspect; // of the magic it holds inside itself, by what it is made of

			double GetVolume() const { return Collider.GetVolume(); }
			double GetMass() const { return Density * GetVolume(); }
		};

		const PropSpec& GetPropSpec(EPropKind Kind)
		{
			static const std::array<PropSpec, static_cast<std::size_t>(EPropKind::Count)> Specs = { {
				{ P::Shape::MakeBox({ 0.5, 0.5, 0.5 }), 120.0, 0.6, 0.15, 1.05, 1.0, 0.5, false, EPrimitive::Cube, { 1.0f, 1.0f, 1.0f }, { 0.45f, 0.28f, 0.12f }, Sim::EMagicType::Nature }, // wood
				{ P::Shape::MakeCapsule(0.3, 0.3), 100.0, 0.5, 0.2, 0.9, 0.5, 0.45, true, EPrimitive::Cylinder, { 0.6f, 0.6f, 1.2f }, { 0.30f, 0.14f, 0.06f }, Sim::EMagicType::Nature },
				{ P::Shape::MakeSphere(0.35), 45.0, 0.5, 0.5, 0.47, 0.38, 0.35, true, EPrimitive::Sphere, { 0.7f, 0.7f, 0.7f }, { 0.95f, 0.75f, 0.15f }, Sim::EMagicType::Earth }, // clay and earth
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

		/**
		 * A card of grass standing on the ground, cut out by the grass blades texture: its foot is the ground's, its
		 * top sways. Lit with the ground's normal, as a blade is, so that it blends into the ground under it.
		 */
		void AddGrassCard(std::vector<TerrainVertex>& Out, const Vec3& Foot, const Vec3& Across, const Vec3& Up, const Vec3& Normal, double Tone)
		{
			const auto Corner = [&](double U, double V)
			{
				return MakeVertex(Foot + Across * (U * 2.0 - 1.0) + Up * V, Normal, Color{ U, V, Tone }, V * 0.35, TerrainGrass);
			};
			const TerrainVertex A = Corner(0.0, 0.0);
			const TerrainVertex B = Corner(1.0, 0.0);
			const TerrainVertex C = Corner(1.0, 1.0);
			const TerrainVertex D = Corner(0.0, 1.0);
			Out.insert(Out.end(), { A, B, C, A, C, D });
		}

		/**
		 * Minerals that dense earth magic has made: a cluster of six-sided crystals with pointed tips, leaning
		 * out from where they grew, in one of a few colours of stone, rule 14 in TeinoraSim.h.
		 */
		void AddCrystals(std::vector<TerrainVertex>& Out, const Vec3& Base, double Size, std::uint32_t Seed)
		{
			constexpr Color Kinds[] = {
				{ 0.42, 0.24, 0.62 }, // amethyst
				{ 0.80, 0.82, 0.86 }, // rock crystal
				{ 0.72, 0.52, 0.18 }, // citrine
				{ 0.16, 0.50, 0.34 }, // emerald
				{ 0.20, 0.36, 0.66 }, // sapphire
			};
			Random Rng(static_cast<std::uint64_t>(Seed) * 2654435761ull + 1ull);
			const Color Tint = Kinds[static_cast<std::size_t>(Rng.Next() * std::size(Kinds)) % std::size(Kinds)];
			const int Count = 3 + static_cast<int>(Rng.Next() * 4.0);
			for (int Index = 0; Index < Count; ++Index)
			{
				const double Lean = Rng.Range(0.0, 0.6);
				const double Toward = Rng.Range(0.0, 2.0 * Pi);
				const Vec3 Axis = Normalize(Vec3{ std::sin(Lean) * std::cos(Toward), std::sin(Lean) * std::sin(Toward), std::cos(Lean) });
				const double Long = Size * Rng.Range(0.45, 1.0) * (Index == 0 ? 1.3 : 1.0);
				const double Radius = Long * Rng.Range(0.14, 0.22);
				const Vec3 Foot = Base + Vec3{ Rng.Range(-0.3, 0.3), Rng.Range(-0.3, 0.3), 0.0 } * Size - Axis * Radius;
				const Vec3 Side = Normalize(Cross(Axis, std::fabs(Axis.Z) > 0.9 ? Vec3{ 1.0, 0.0, 0.0 } : Vec3{ 0.0, 0.0, 1.0 }));
				const Vec3 Other = Cross(Axis, Side);
				const Vec3 Top = Foot + Axis * (Long * 0.78);
				const Vec3 Tip = Foot + Axis * Long;
				const Color Shade = Scale(Tint, Rng.Range(0.85, 1.15));
				for (int Face = 0; Face < 6; ++Face)
				{
					const double A = Face * Pi / 3.0;
					const double B = (Face + 1) * Pi / 3.0;
					const Vec3 Out1 = Side * std::cos(A) + Other * std::sin(A);
					const Vec3 Out2 = Side * std::cos(B) + Other * std::sin(B);
					AddFacet(Out, Foot + Out1 * Radius, Foot + Out2 * Radius, Top + Out2 * Radius, Shade);
					AddFacet(Out, Foot + Out1 * Radius, Top + Out2 * Radius, Top + Out1 * Radius, Shade);
					AddFacet(Out, Top + Out1 * Radius, Top + Out2 * Radius, Tip, Scale(Shade, 1.25));
				}
			}
		}

		/**
		 * A boulder: a sphere with slices taken off it, as stone breaks along its planes, and roughened.
		 * It goes well below its middle so as to sit in a slope, and its normals are shared between
		 * triangles, so that only the edges of the cuts show and not those of the mesh.
		 */
		void AddRock(std::vector<TerrainVertex>& Out, const Vec3& Center, double Radius, int Sides, int Rings, std::uint32_t Stone, const Color& Tint)
		{
			constexpr int Cuts = 9;
			constexpr double Lowest = -0.85; // radians below the middle

			Random Cutter(Stone);
			Vec3 Planes[Cuts];
			double Depths[Cuts];
			for (int Cut = 0; Cut < Cuts; ++Cut)
			{
				const double Rise = Cutter.Range(-0.35, 1.0); // cracks open from above and the sides
				const double Angle = Cutter.Range(0.0, 2.0 * Pi);
				const double Across = std::sqrt(1.0 - Rise * Rise);
				Planes[Cut] = { Across * std::cos(Angle), Across * std::sin(Angle), Rise };
				Depths[Cut] = Cutter.Range(0.78, 1.0);
			}

			const auto Rough = [Stone](const Vec3& Way, double Size, std::uint32_t Salt)
			{
				return (Sim::GradientNoise(Way.X * Size, Way.Y * Size, Stone + Salt)
					+ Sim::GradientNoise(Way.Y * Size, Way.Z * Size, Stone + Salt + 1u)
					+ Sim::GradientNoise(Way.Z * Size, Way.X * Size, Stone + Salt + 2u)) / 3.0;
			};

			std::vector<Vec3> Points(static_cast<std::size_t>(Sides) * (Rings + 1));
			const auto PointAt = [&Points, Sides](int Ring, int Step) -> Vec3&
			{
				return Points[static_cast<std::size_t>(Ring) * Sides + (Step + Sides) % Sides];
			};

			for (int Ring = 0; Ring <= Rings; ++Ring)
			{
				const double Elevation = Lowest + (Pi * 0.5 - Lowest) * Ring / Rings;
				for (int Step = 0; Step < Sides; ++Step)
				{
					const double Angle = 2.0 * Pi * Step / Sides;
					const Vec3 Way{ std::cos(Elevation) * std::cos(Angle), std::cos(Elevation) * std::sin(Angle), std::sin(Elevation) };

					double Reach = 1.0;
					for (int Cut = 0; Cut < Cuts; ++Cut)
					{
						const double Along = Dot(Way, Planes[Cut]);
						if (Along > Depths[Cut])
						{
							Reach = std::min(Reach, Depths[Cut] / Along);
						}
					}
					PointAt(Ring, Step) = Center + Way * (Radius * Reach * (1.0 + 0.2 * Rough(Way, 2.3, 0u) + 0.08 * Rough(Way, 6.1, 5u)));
				}
			}

			const auto AddCorner = [&](int Ring, int Step)
			{
				const Vec3& Point = PointAt(Ring, Step);
				const Vec3 Round = PointAt(Ring, Step + 1) - PointAt(Ring, Step - 1);
				const Vec3 Upward = PointAt(std::min(Ring + 1, Rings), Step) - PointAt(std::max(Ring - 1, 0), Step);
				const Vec3 Facing = Cross(Round, Upward);
				const double Size = Length(Facing);
				Out.push_back(MakeVertex(Point, Size > 1e-9 ? Facing * (1.0 / Size) : Normalize(Point - Center), Tint, 0.0, TerrainStone));
			};

			for (int Ring = 0; Ring < Rings; ++Ring)
			{
				for (int Step = 0; Step < Sides; ++Step)
				{
					AddCorner(Ring, Step);
					AddCorner(Ring, Step + 1);
					AddCorner(Ring + 1, Step);
					if (Ring + 1 < Rings) // the last ring closes on a point
					{
						AddCorner(Ring, Step + 1);
						AddCorner(Ring + 1, Step + 1);
						AddCorner(Ring + 1, Step);
					}
				}
			}
		}

		/** Whether something hot lies over a point, which nothing grows on or lies in. */
		bool IsBurning(const std::vector<Sim::HotSpot>& Spots, double X, double Y)
		{
			for (const Sim::HotSpot& Spot : Spots)
			{
				const double AcrossX = X - Spot.Position.X;
				const double AcrossY = Y - Spot.Position.Y;
				const double Clear = Spot.Radius * (Spot.bLava ? 1.35 : 1.9); // the widest a pool's edge gets
				if (AcrossX * AcrossX + AcrossY * AcrossY < Clear * Clear)
				{
					return true;
				}
			}
			return false;
		}

		/** A pool of lava lying on the ground as it finds it: open in the middle, set black along its uneven edge. */
		void AddLava(std::vector<TerrainVertex>& Out, const Vec3& Origin, const Sim::HotSpot& Spot, int Rings, int Sides, double Lift)
		{
			const auto Stone = static_cast<std::uint32_t>(std::fabs(Spot.Position.X) * 7.0 + std::fabs(Spot.Position.Y) * 13.0);
			const auto AddCorner = [&](int Ring, int Step)
			{
				const double Share = static_cast<double>(Ring) / Rings;
				const double Angle = 2.0 * Pi * Step / Sides;
				const double Round = 0.8 + 0.34 * Sim::GradientNoise(std::cos(Angle) * 1.7, std::sin(Angle) * 1.7, Stone) + 0.13 * Sim::GradientNoise(std::cos(Angle) * 5.0, std::sin(Angle) * 5.0, Stone + 1u);
				const double Reach = Spot.Radius * Share * Round;
				const double WorldX = Spot.Position.X + std::cos(Angle) * Reach;
				const double WorldY = Spot.Position.Y + std::sin(Angle) * Reach;
				const double Height = Sim::TerrainHeight(WorldX, WorldY, Sim::WorldSeed) + Lift * (1.0 - Share * Share * Share);
				const double Setting = std::clamp((Share - 0.5) / 0.47, 0.0, 1.0);
				const double Heat = 1.0 - Setting * Setting * (3.0 - 2.0 * Setting);
				Out.push_back(MakeVertex(Vec3{ WorldX - Origin.X, WorldY - Origin.Y, Height }, Sim::TerrainNormal(WorldX, WorldY, Sim::WorldSeed), Color{ Heat, 0.0, 0.0 }, 0.0, TerrainMolten));
			};

			for (int Ring = 0; Ring < Rings; ++Ring)
			{
				for (int Step = 0; Step < Sides; ++Step)
				{
					AddCorner(Ring, Step);
					AddCorner(Ring + 1, Step);
					AddCorner(Ring + 1, Step + 1);
					if (Ring > 0) // the first ring starts from a point
					{
						AddCorner(Ring, Step);
						AddCorner(Ring + 1, Step + 1);
						AddCorner(Ring, Step + 1);
					}
				}
			}
		}

		/** A burnt length of wood, from one end to the other. */
		void AddLog(std::vector<TerrainVertex>& Out, const Vec3& From, const Vec3& To, double Radius, int Sides, const Color& Tint)
		{
			const Vec3 Along = Normalize(To - From);
			const Vec3 Side = Normalize(Cross(Along, Vec3{ 0.0, 0.0, 1.0 }));
			const Vec3 Other = Cross(Side, Along);
			for (int Step = 0; Step < Sides; ++Step)
			{
				const auto Facing = [&](int At)
				{
					const double Angle = 2.0 * Pi * At / Sides;
					return Side * std::cos(Angle) + Other * std::sin(Angle);
				};
				const Vec3 Left = Facing(Step);
				const Vec3 Right = Facing(Step + 1);
				const auto AddCorner = [&](const Vec3& End, const Vec3& Way)
				{
					Out.push_back(MakeVertex(End + Way * Radius, Way, Tint, 0.0, TerrainBark));
				};
				AddCorner(From, Left);
				AddCorner(To, Left);
				AddCorner(To, Right);
				AddCorner(From, Left);
				AddCorner(To, Right);
				AddCorner(From, Right);
			}
		}

		/** Stones in a ring, logs leaning together, and a bed of embers. The flames are drawn apart. */
		void AddCampfire(std::vector<TerrainVertex>& Out, const Vec3& Origin, const Sim::HotSpot& Spot)
		{
			Random Rng(static_cast<std::uint64_t>(std::fabs(Spot.Position.X) * 7.0 + std::fabs(Spot.Position.Y) * 13.0) + 1u);
			const auto GroundAt = [&](double Angle, double Reach)
			{
				const double WorldX = Spot.Position.X + std::cos(Angle) * Reach;
				const double WorldY = Spot.Position.Y + std::sin(Angle) * Reach;
				return Vec3{ WorldX - Origin.X, WorldY - Origin.Y, Sim::TerrainHeight(WorldX, WorldY, Sim::WorldSeed) };
			};

			constexpr int Stones = 9;
			for (int Index = 0; Index < Stones; ++Index)
			{
				const double Shade = Rng.Range(0.8, 1.1);
				const Vec3 Place = GroundAt(2.0 * Pi * Index / Stones + Rng.Range(-0.1, 0.1), Spot.Radius * Rng.Range(0.92, 1.05));
				AddRock(Out, Place, Rng.Range(0.14, 0.22), 7, 3, static_cast<std::uint32_t>(Rng.Next() * 4294967295.0), Color{ 0.2 * Shade, 0.19 * Shade, 0.18 * Shade });
			}

			const Vec3 Middle = GroundAt(0.0, 0.0);
			constexpr int Logs = 6;
			for (int Index = 0; Index < Logs; ++Index)
			{
				const double Angle = 2.0 * Pi * Index / Logs + Rng.Range(-0.2, 0.2);
				const Vec3 Foot = GroundAt(Angle, Spot.Radius * Rng.Range(0.6, 0.72)) + Vec3{ 0.0, 0.0, 0.03 };
				const Vec3 Tip = Middle + Vec3{ std::cos(Angle + 0.5) * 0.05, std::sin(Angle + 0.5) * 0.05, Rng.Range(0.42, 0.6) };
				const double Shade = Rng.Range(0.7, 1.2);
				AddLog(Out, Foot, Tip, Rng.Range(0.04, 0.065), 6, Color{ 0.05 * Shade, 0.04 * Shade, 0.035 * Shade });
			}

			constexpr int Sides = 14;
			const Vec3 Up{ 0.0, 0.0, 1.0 };
			for (int Step = 0; Step < Sides; ++Step)
			{
				const Vec3 Left = GroundAt(2.0 * Pi * Step / Sides, Spot.Radius * 0.72) + Vec3{ 0.0, 0.0, 0.03 };
				const Vec3 Right = GroundAt(2.0 * Pi * (Step + 1) / Sides, Spot.Radius * 0.72) + Vec3{ 0.0, 0.0, 0.03 };
				Out.push_back(MakeVertex(Middle + Vec3{ 0.0, 0.0, 0.05 }, Up, Color{ 1.0, 0.0, 0.0 }, 0.0, TerrainMolten));
				Out.push_back(MakeVertex(Left, Up, Color{ 0.45, 0.0, 0.0 }, 0.0, TerrainMolten));
				Out.push_back(MakeVertex(Right, Up, Color{ 0.45, 0.0, 0.0 }, 0.0, TerrainMolten));
			}
		}

		/** The ground's broad variation, what the textures, which repeat, cannot show, and where the water stands. */
		Color GroundTint(double WorldX, double WorldY, double Height, double Moisture)
		{
			const double Shade = 1.0 + 0.16 * Sim::GradientNoise(WorldX / 37.0, WorldY / 37.0, Sim::WorldSeed + 22u);
			const double Patch = 0.5 + 0.5 * Sim::GradientNoise(WorldX / 90.0, WorldY / 90.0, Sim::WorldSeed + 21u);
			const double Lush = std::clamp(Moisture * 1.15 - 0.1 + (Patch - 0.5) * 0.45, 0.0, 1.0);
			return { Shade * 0.5, Lush, EncodeGroundLevel(Height - Sim::GetShoreLevel(WorldX, WorldY, Sim::WorldSeed)) };
		}

		Sim::ESurface ClassifyFacet(const Vec3& A, const Vec3& B, const Vec3& C, double OriginX, double OriginY)
		{
			const double Level = Sim::GetShoreLevel(OriginX + (A.X + B.X + C.X) / 3.0, OriginY + (A.Y + B.Y + C.Y) / 3.0, Sim::WorldSeed);
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

	void GameWorld::ReceiveStrike(const Vec3& Point, double Time)
	{
		constexpr double Reach = 30.0; // metres: bodies asleep feel nothing, and fire magic would not wake them

		for (const Prop& Item : Props)
		{
			if (Length(Physics.GetBody(Item.Body).Position - Point) < Reach)
			{
				Physics.WakeBody(Item.Body);
			}
		}

		// Beyond the gas nobody works it out, but a bolt always brings far more fire than it takes.
		if (Magic == nullptr || !Magic->Contains(Point))
		{
			const bool bWater = Sim::TerrainHeight(Point.X, Point.Y, Sim::WorldSeed) < Sim::GetShoreLevel(Point.X, Point.Y, Sim::WorldSeed);
			ReceiveFire({ { Point, bWater ? Sim::MagicChannelAmount : 0.0 } }, Time);
		}
	}

	void GameWorld::ReceiveFire(const std::vector<FireContact>& Contacts, double Time)
	{
		constexpr double BurnRadius = 2.5;
		constexpr double FiresApart = 6.0;  // metres, or one patch of grass would light many times over
		constexpr double SteamApart = 12.0;
		constexpr double SteamJoins = 2.0;  // seconds in which more boiling adds to a cloud rather than starting another
		constexpr std::size_t MostSteam = 32;

		for (const FireContact& Contact : Contacts)
		{
			if (Contact.Boiled > 0.0)
			{
				const auto Joined = std::find_if(Steam.begin(), Steam.end(), [&Contact, Time](const SteamPuff& Item)
				{
					return Time < Item.Born + SteamJoins && Length(Item.Point - Contact.Position) < SteamApart;
				});
				if (Joined != Steam.end())
				{
					Joined->Amount += Contact.Boiled;
				}
				else if (Steam.size() < MostSteam)
				{
					Steam.push_back({ Contact.Position, AirDrift, Contact.Boiled, Time });
				}
				continue;
			}

			const bool bAlight = std::any_of(Burning.begin(), Burning.end(), [&Contact](const Sim::HotSpot& Fire)
			{
				return Length(Fire.Position - Contact.Position) < FiresApart;
			});
			if (!bAlight && Sim::CanBurn(Contact.Position.X, Contact.Position.Y, Sim::WorldSeed))
			{
				Sim::HotSpot Fire;
				Fire.Position = Contact.Position;
				Fire.Radius = BurnRadius;
				Fire.Depth = 1.0;
				Fire.Temperature = 850.0;
				Burning.push_back(Fire);
				BurningUntil.push_back(Time + BurnTime);
			}
		}
	}

	void GameWorld::UpdateBurning(double Time)
	{
		constexpr double Settling = 8.0;  // seconds for a new fire to settle before the wind can put it out
		constexpr double GoneOut = 0.15;  // of the fire magic its heat would gather, what a fire needs around it to stay alight

		std::erase_if(Steam, [Time](const SteamPuff& Item) { return Time > Item.Born + SteamLife; });
		std::erase_if(BurntOut, [Time](const Smouldering& Item) { return Time > Item.Out + SmokeLife; });

		for (std::size_t Index = Burning.size(); Index-- > 0;)
		{
			const Vec3 Air = Burning[Index].Position + Vec3{ 0.0, 0.0, 2.0 };
			const bool bBlownOut = Time > BurningUntil[Index] - BurnTime + Settling && Magic != nullptr && Magic->Contains(Air)
				&& Magic->GetMix(Air).GetAllFire() < GoneOut * Sim::MagicHotDraw(Air, Burning[Index]).Density;
			if (Time > BurningUntil[Index] || bBlownOut)
			{
				BurntOut.push_back({ Burning[Index], BurningUntil[Index] - BurnTime, Time });
				Burning.erase(Burning.begin() + static_cast<std::ptrdiff_t>(Index));
				BurningUntil.erase(BurningUntil.begin() + static_cast<std::ptrdiff_t>(Index));
			}
		}
	}

	void GameWorld::CollectPuffs(const Vec3& Eye, std::vector<PuffDraw>& Out, std::size_t Most)
	{
		Out.clear();

		// Steam goes up fast at first, and swells.
		constexpr double SteamClimb = 6.0;     // m/s as it leaves the water
		constexpr double SteamSlowing = 4.0;   // seconds for that to die away
		constexpr double SteamSwelling = 3.0;  // seconds to double in size
		constexpr double SizePerRoot = 0.9;    // metres for the cube root of each kg boiled
		for (const SteamPuff& Item : Steam)
		{
			const double Age = std::max(LastUpdateTime - Item.Born, 0.0);
			const double Left = std::max(1.0 - Age / SteamLife, 0.0);

			PuffDraw Draw;
			Draw.Radius = SizePerRoot * std::cbrt(Item.Amount) * (1.0 + Age / SteamSwelling);
			Draw.Point = Item.Point + (AirDrift - Item.Drift) + Vec3{ 0.0, 0.0, SteamClimb * SteamSlowing * (1.0 - std::exp(-Age / SteamSlowing)) + Draw.Radius * 0.5 };
			Draw.Thickness = Left * Left;
			Draw.Seed = std::fmod(Item.Born, 97.0);
			Draw.Floor = Item.Point.Z;
			Out.push_back(Draw);
		}

		// Smoke over each fire near enough: a few puffs that rise, swell and go with the wind, over and over.
		constexpr double SmokeSeen = 600.0;
		constexpr std::size_t SmokingFires = 8;
		constexpr int PuffsEach = 16;           // close enough together to make one column
		constexpr double SmokeSwelling = 1.6;   // seconds to double in size
		struct Smoking
		{
			const Sim::HotSpot* Spot;
			double Lit; // when it caught, before which it made no smoke
			double Out; // and when it went out, after which it made no more
		};
		RefreshHotSpots(Eye);
		std::vector<Smoking> Fires;
		for (const Sim::HotSpot& Spot : HotSpotsAround)
		{
			if (!Spot.bLava && Length(Spot.Position - Eye) < SmokeSeen)
			{
				Fires.push_back({ &Spot, -1e30, 1e30 }); // campfires have always burned, and always will
			}
		}
		for (std::size_t Index = 0; Index < Burning.size(); ++Index)
		{
			if (Length(Burning[Index].Position - Eye) < SmokeSeen)
			{
				Fires.push_back({ &Burning[Index], BurningUntil[Index] - BurnTime, 1e30 });
			}
		}
		for (const Smouldering& Item : BurntOut)
		{
			if (Length(Item.Spot.Position - Eye) < SmokeSeen)
			{
				Fires.push_back({ &Item.Spot, Item.Lit, Item.Out });
			}
		}
		std::sort(Fires.begin(), Fires.end(), [&Eye](const Smoking& A, const Smoking& B)
		{
			return Length(A.Spot->Position - Eye) < Length(B.Spot->Position - Eye);
		});
		if (Fires.size() > SmokingFires)
		{
			Fires.resize(SmokingFires);
		}

		const Vec3 Wind = Sim::PrevailingWind(LastUpdateTime, Sim::WorldSeed) * 0.7; // it is slower near the ground
		for (const Smoking& Each : Fires)
		{
			const Sim::HotSpot* Fire = Each.Spot;
			const bool bLarge = Fire->Radius >= 1.0;
			const double Climb = bLarge ? 1.8 : 1.2;   // m/s once it has cooled, twice that over the fire
			const double Peak = 0.7;                   // how thick it is at its thickest
			const double Size = bLarge ? 1.6 : 1.0;    // metres across at the start
			const double Shade = bLarge ? 0.3 : 0.6;   // wood smoke is a light grey, burning grass darker
			const double Strength = GetFlameStrength(*Fire, Fire->Position + Vec3{ 0.0, 0.0, 0.5 });
			const double Own = std::fmod(std::fabs(Fire->Position.X * 0.37 + Fire->Position.Y * 0.61), 1.0); // so that no two fires puff alike

			for (int Index = 0; Index < PuffsEach; ++Index)
			{
				const double Cycle = LastUpdateTime / SmokeLife + (Index + Own) / PuffsEach;
				const double Age = (Cycle - std::floor(Cycle)) * SmokeLife;
				const double Left = 1.0 - Age / SmokeLife;
				const double Born = LastUpdateTime - Age;
				if (Born < Each.Lit || Born > Each.Out)
				{
					continue;
				}

				PuffDraw Draw;
				Draw.Radius = Size * (1.0 + Age / SmokeSwelling);
				Draw.Point = Fire->Position + Wind * Age + Vec3{ 0.0, 0.0, Climb * (Age + 3.0 * (1.0 - std::exp(-Age / 3.0))) + Draw.Radius * 0.5 };
				Draw.Thickness = std::min(Peak * Strength, 1.0) * std::min(Age / 0.4, 1.0) * Left;
				Draw.Seed = std::fmod(std::floor(Cycle) * 7.31 + Index * 3.7 + Own * 97.0, 97.0);
				Draw.Floor = Fire->Position.Z;
				Draw.Shade = Shade;

				// Lit from below by its own fire, falling off as the surfaces' firelight does in Shaders/Lighting.hlsli.
				const double Square = LengthSquared(Draw.Point - Fire->Position);
				const double Fade = std::max(1.0 - Square / (FlameReach * FlameReach), 0.0);
				Draw.Firelight = LastUpdateTime > Each.Out ? 0.0 : Strength * Fade * Fade / (1.0 + Square / (FlameReach * FlameReach * 0.015));
				Out.push_back(Draw);
			}
		}

		// Fireflies, and the dust whirlwinds lift.
		Shows.CollectPuffs(LastUpdateTime, Out);

		// What has fallen apart for want of magic, as a cloud of dust that spreads, sinks and clears.
		for (const Crumbled& Item : Dust)
		{
			const double Age = std::max(LastUpdateTime - Item.Born, 0.0) / DustLife;
			for (int Index = 0; Index < 5; ++Index)
			{
				const double Angle = Index * 2.4 + Item.Born;
				PuffDraw Draw;
				Draw.Point = Item.Point + Vec3{ std::cos(Angle), std::sin(Angle), 0.3 } * (Item.Radius * (0.4 + 2.0 * Age)) - Vec3{ 0.0, 0.0, Age * 0.4 };
				Draw.Radius = Item.Radius * (0.8 + 1.6 * Age);
				Draw.Thickness = 0.7 * (1.0 - Age) * (1.0 - Age);
				Draw.Seed = Item.Born + Index * 3.1;
				Draw.Floor = Item.Point.Z - Item.Radius;
				Draw.Shade = 0.75;
				Out.push_back(Draw);
			}
		}

		std::sort(Out.begin(), Out.end(), [&Eye](const PuffDraw& A, const PuffDraw& B)
		{
			return Length(A.Point - Eye) < Length(B.Point - Eye);
		});
		if (Out.size() > Most)
		{
			Out.resize(Most);
		}
	}

	void GameWorld::RefreshHotSpots(const Vec3& Eye)
	{
		if (Length(Eye - HotSpotsCenter) > 100.0)
		{
			HotSpotsCenter = Eye;
			HotSpotsAround = Sim::FindHotSpots(Eye.X, Eye.Y, Sim::WorldSeed);
		}
	}

	double GameWorld::GetFlameStrength(const Sim::HotSpot& Spot, const Vec3& Point) const
	{
		// A flame is as strong as the fire magic around it says: the wind feeds it or wears it down.
		if (Spot.bLava || Magic == nullptr || !Magic->Contains(Point))
		{
			return 1.0;
		}
		// Its heat gathers fire magic around it, and wind takes some of that away again.
		const double Usual = std::max(Sim::MagicHotDraw(Point, Spot).Density * FireKeptInWind, 1e-3);
		return std::clamp(std::sqrt(Magic->GetMix(Point).GetAllFire() / Usual), 0.5, 1.7);
	}

	void GameWorld::CollectHotLights(const Vec3& Eye, std::vector<HotLight>& Out, std::size_t Most)
	{
		constexpr double Seen = 900.0;

		RefreshHotSpots(Eye);
		std::vector<Sim::HotSpot> Nearby = HotSpotsAround;
		Nearby.insert(Nearby.end(), Burning.begin(), Burning.end());
		std::sort(Nearby.begin(), Nearby.end(), [&Eye](const Sim::HotSpot& A, const Sim::HotSpot& B)
		{
			return Length(A.Position - Eye) < Length(B.Position - Eye);
		});

		Out.clear();
		for (const Sim::HotSpot& Spot : Nearby)
		{
			if (Out.size() == Most || Length(Spot.Position - Eye) > Seen)
			{
				break;
			}

			HotLight Light;
			Light.Point = Spot.Position + Vec3{ 0.0, 0.0, Spot.bLava ? 1.0 + Spot.Radius * 0.3 : 0.5 };
			Light.Color = Spot.bLava ? Vec3{ 3.0, 0.9, 0.16 } : Vec3{ FlameLightRed, FlameLightGreen, FlameLightBlue };
			Light.Reach = Spot.bLava ? 24.0 + Spot.Radius * 4.0 : FlameReach;
			// A campfire burns in one tall flame, a patch of grass in a broad low one torn into many tongues.
			const bool bGrass = Spot.Radius >= 1.0;
			Light.FlameHeight = Spot.bLava ? 0.0 : (bGrass ? 1.2 : 2.3) * Spot.Radius;
			Light.FlameBreadth = bGrass ? 0.9 : 0.36;

			const double Strength = GetFlameStrength(Spot, Light.Point);
			Light.FlameHeight *= Strength;
			Light.Color = Light.Color * Strength;
			Out.push_back(Light);
		}

		// Big fireflies light what is around them too, as far as there is room.
		std::vector<HotLight> Glowing;
		Shows.CollectLights(Glowing);
		std::sort(Glowing.begin(), Glowing.end(), [&Eye](const HotLight& A, const HotLight& B) { return Length(A.Point - Eye) < Length(B.Point - Eye); });
		for (const HotLight& Each : Glowing)
		{
			if (Out.size() == Most)
			{
				break;
			}
			Out.push_back(Each);
		}
	}

	void GameWorld::TakeMagicDeposits(std::vector<MagicDeposit>& Out)
	{
		Out.clear();
		Shows.TakeDeposits(Out);
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
			const double WaterLevel = Sim::GetShoreLevel(WorldX, WorldY, Sim::WorldSeed);
			if (Height < WaterLevel + 1.4 || Sim::ClassifySurface(Height, Sim::TerrainNormal(WorldX, WorldY, Sim::WorldSeed).Z, WaterLevel) != Sim::ESurface::Grass || IsBurning(Build.HotSpots, WorldX, WorldY))
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
			const double WaterLevel = Sim::GetShoreLevel(WorldX, WorldY, Sim::WorldSeed);
			if (Height < WaterLevel + 0.9 || Sim::ClassifySurface(Height, Sim::TerrainNormal(WorldX, WorldY, Sim::WorldSeed).Z, WaterLevel) != Sim::ESurface::Grass || IsBurning(Build.HotSpots, WorldX, WorldY))
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
			const auto Stone = static_cast<std::uint32_t>(Rng.Next() * 4294967295.0); // the same stone at every level of detail
			if (IsBurning(Build.HotSpots, WorldX, WorldY))
			{
				continue;
			}

			const double Height = Sim::TerrainHeight(WorldX, WorldY, Sim::WorldSeed);
			const Vec3 Center{ Local.X, Local.Y, Height - Radius * 0.2 };
			const Color Tint{ 0.30 * Shade, 0.29 * Shade, 0.28 * Shade };

			AddRock(Build.Vertices, Center, Radius, RockSides, Build.Lod == 0 ? Level.RockRings : std::max(Level.RockRings / 2, 2), Stone, Tint);

			if (Build.Lod == 0)
			{
				// What is left of the sphere after the cuts in AddRock is about this big.
				Build.Statics.push_back({ P::Shape::MakeSphere(Radius * 0.92), Vec3{ WorldX, WorldY, Center.Z } });
			}
		}

		// What dense magic has made, which stays: minerals out of earth magic, bushes out of nature magic.
		for (const Sim::MagicGrowth& Growth : Sim::FindMagicGrowths(Origin.X, Origin.Y, Origin.X + ChunkSize, Origin.Y + ChunkSize))
		{
			const Vec3 Local{ Growth.Position.X - Origin.X, Growth.Position.Y - Origin.Y, Growth.Position.Z };
			if (Growth.Kind == Sim::EMagicGrowth::Minerals)
			{
				AddCrystals(Build.Vertices, Local, Growth.Size, Growth.Seed);
				if (Build.Lod == 0)
				{
					Build.Statics.push_back({ P::Shape::MakeSphere(Growth.Size * 0.4), Growth.Position + Vec3{ 0.0, 0.0, Growth.Size * 0.2 } });
				}
			}
			else
			{
				GrowPlant(EPlantKind::Bush, Local, Origin, Growth.Size, Plants, Growth.Seed, Output);
			}
		}

		for (const Sim::HotSpot& Spot : Build.HotSpots)
		{
			// Each belongs to the chunk it stands in, which draws all of it.
			if (ToChunk(Spot.Position.X) != Build.X || ToChunk(Spot.Position.Y) != Build.Y)
			{
				continue;
			}

			if (Spot.bLava)
			{
				// A coarse mesh of the ground wanders further from the true height that this is laid on.
				const double Cell = ChunkSize / Level.Cells[Build.Lod];
				AddLava(Build.Vertices, Origin, Spot, Build.Lod == 0 ? 8 : 4, Build.Lod == 0 ? 40 : 20, 0.1 + 0.04 * Cell);
			}
			else if (Build.Lod == 0)
			{
				AddCampfire(Build.Vertices, Origin, Spot);
				Build.Statics.push_back({ P::Shape::MakeSphere(Spot.Radius), Spot.Position - Vec3{ 0.0, 0.0, Spot.Radius * 0.75 } });
			}
		}
	}

	void GameWorld::GrowGrass(ChunkBuild& Build)
	{
		const int Tufts = DetailLevels[Build.Detail].GrassTufts * (Build.bDenseGrass ? DenseGrass : 1);
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
			if ((!bReeds && Sim::ClassifySurface(Height, Normal.Z, Sim::GetShoreLevel(X + Build.X * ChunkSize, Y + Build.Y * ChunkSize, Sim::WorldSeed)) != Sim::ESurface::Grass)
				|| IsBurning(Build.HotSpots, X + Build.X * ChunkSize, Y + Build.Y * ChunkSize))
			{
				continue;
			}

			if (!bReeds)
			{
				// Two cards of grass blades across each other, so that it looks like a clump from any side.
				const double Wide = Rng.Range(0.3, 0.45);
				const double Tall = Rng.Range(0.3, 0.55);
				const Vec3 Foot{ X, Y, Height - 0.04 };
				for (int Card = 0; Card < 2; ++Card)
				{
					const double Angle = Facing + Card * Pi * 0.5;
					AddGrassCard(Build.Vertices, Foot, Vec3{ std::cos(Angle), std::sin(Angle), 0.0 } * Wide, Vec3{ 0.0, 0.0, Tall }, Normal, std::clamp(Shade - 0.4, 0.0, 1.0));
				}
				continue;
			}

			const Color Tint{ 0.10 * Shade, 0.15 * Shade, 0.05 * Shade };
			const double Width = 0.03;
			const double Lean = 0.22;
			for (int Blade = 0; Blade < 3; ++Blade)
			{
				const double Angle = Facing + Blade * Pi / 3.0;
				const Vec3 Across{ std::cos(Angle) * Width, std::sin(Angle) * Width, 0.0 };
				const Vec3 Leaning{ -std::sin(Angle) * Lean, std::cos(Angle) * Lean, 0.0 };
				const Vec3 Root{ X, Y, Height - 0.03 };
				const Vec3 Tip = Root + Leaning + Vec3{ 0.0, 0.0, std::max(WaterLevel + 0.6 - Height, 0.0) + Rng.Range(0.6, 1.4) };
				AddBlade(Build.Vertices, Root - Across, Root + Across, Tip, Normal, Tint);
			}
		}
	}

	std::unique_ptr<GameWorld::ChunkBuild> GameWorld::BuildChunk(int X, int Y, int Lod, int Detail, bool bDenseGrass)
	{
		auto Build = std::make_unique<ChunkBuild>();
		Build->bDenseGrass = bDenseGrass;
		Build->X = X;
		Build->Y = Y;
		Build->Lod = Lod;
		Build->Detail = Detail;
		for (const Sim::HotSpot& Spot : Sim::FindHotSpots((X + 0.5) * ChunkSize, (Y + 0.5) * ChunkSize, Sim::WorldSeed))
		{
			if (std::fabs(Spot.Position.X - (X + 0.5) * ChunkSize) < ChunkSize * 0.5 + 25.0 && std::fabs(Spot.Position.Y - (Y + 0.5) * ChunkSize) < ChunkSize * 0.5 + 25.0)
			{
				Build->HotSpots.push_back(Spot);
			}
		}
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

		const double Dt = std::clamp(Time - LastUpdateTime, 0.0, 0.25);
		AirDrift += Sim::PrevailingWind(Time, Sim::WorldSeed) * (Time - LastUpdateTime);
		LastUpdateTime = Time;

		Liquid.Follow(Focus);
		RequestChunks(CenterX, CenterY);
		ApplyFinishedChunks();
		CullProps(Time);
		UpdateBurning(Time);

		// What dense magic makes. Minerals and plants are built into the ground's chunks, so those are built again.
		if (Magic != nullptr)
		{
			Shows.Update(*Magic, Time, Dt);
			std::vector<Sim::MagicGrowth> Grown;
			Shows.TakeGrowths(Grown);
			for (const Sim::MagicGrowth& Growth : Grown)
			{
				Sim::AddMagicGrowth(Growth);
				for (int ChunkY = ToChunk(Growth.Position.Y - Growth.Size); ChunkY <= ToChunk(Growth.Position.Y + Growth.Size); ++ChunkY)
				{
					for (int ChunkX = ToChunk(Growth.Position.X - Growth.Size); ChunkX <= ToChunk(Growth.Position.X + Growth.Size); ++ChunkX)
					{
						Stale.insert(ChunkKey(ChunkX, ChunkY));
					}
				}
			}
		}

		if (Length(Focus - WellsCenter) > 200.0)
		{
			WellsCenter = Focus;
			WellsAround = Sim::FindMagicWells(Focus.X, Focus.Y, Sim::WorldSeed);
		}
	}

	void GameWorld::RequestChunks(int CenterX, int CenterY)
	{
		struct Request
		{
			int X;
			int Y;
			int Lod;
			int Ring;
			bool bDenseGrass;
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
				const bool bOutdated = Existing != Chunks.end() && (Existing->second.Detail != Detail || Stale.contains(Key));
				const int Ring = RingOf(X, Y);

				// One extra ring of hysteresis stops rebuilds when moving along a border.
				const int NearRadius = DetailLevels[Detail].NearRadius;
				const bool bNear = Ring <= NearRadius || (CurrentLod == 0 && Ring <= NearRadius + 1);
				const int WantedLod = bNear ? 0 : 1;

				// The chunks right around the eye grow their grass thicker, again with a ring of hysteresis.
				const bool bWasDense = Existing != Chunks.end() && Existing->second.bDenseGrass;
				const bool bDense = WantedLod == 0 && (Ring <= 1 || (bWasDense && Ring <= 2));
				if (CurrentLod != WantedLod || bOutdated || bDense != bWasDense)
				{
					Requests.push_back({ X, Y, WantedLod, Ring, bDense });
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
			Stale.erase(ChunkKey(Item.X, Item.Y));
			Workers.Enqueue([Results = Finished, X = Item.X, Y = Item.Y, Lod = Item.Lod, Level = Detail, bDense = Item.bDenseGrass]
			{
				std::unique_ptr<ChunkBuild> Build = BuildChunk(X, Y, Lod, Level, bDense);
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
		Target.bDenseGrass = Build.bDenseGrass;

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

	void GameWorld::TakeMagicPushes(std::vector<MagicPush>& Out)
	{
		Out.swap(MagicPushes);
		MagicPushes.clear();
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
			// The aim allows for the magic around the thrower as well as the air, as a thrower would by feel.
			const double Thick = Magic != nullptr && Magic->Contains(Start) ? Magic->GetMix(Start).GetActing().GetAspected() * Sim::MagicDragShare : 0.0;
			const double DragPerMass = 0.5 * (Sim::AirDensity + Thick) * Spec.DragCoefficient * Spec.FrontalArea / Spec.GetMass();
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

	bool GameWorld::PlaceCampfire(const Vec3& Eye, const Vec3& Gaze)
	{
		constexpr double Reach = 80.0;
		constexpr double Footprint = 2.0; // metres around it that its stones and logs cover

		P::RayHit Hit;
		if (!Physics.Raycast(Eye, Gaze, Reach, Hit) || Hit.Normal.Z < 0.8)
		{
			return false;
		}
		const double X = Hit.Point.X;
		const double Y = Hit.Point.Y;
		const double Ground = Sim::TerrainHeight(X, Y, Sim::WorldSeed);
		if (Ground < Sim::GetShoreLevel(X, Y, Sim::WorldSeed) + 0.3 || std::fabs(Ground - Hit.Point.Z) > 1.0)
		{
			return false; // in water, or on something other than the ground
		}

		Sim::PlaceCampfire({ X, Y, Ground });
		HotSpotsCenter = { 1e12, 0.0, 0.0 }; // look again at what burns

		// Its stones and logs are built into the ground's chunks, so those are built again.
		for (int ChunkY = ToChunk(Y - Footprint); ChunkY <= ToChunk(Y + Footprint); ++ChunkY)
		{
			for (int ChunkX = ToChunk(X - Footprint); ChunkX <= ToChunk(X + Footprint); ++ChunkX)
			{
				Stale.insert(ChunkKey(ChunkX, ChunkY));
			}
		}
		return true;
	}

	void GameWorld::PlaceProp(EPropKind Kind, const Vec3& Eye, const Vec3& Gaze)
	{
		constexpr double Reach = 80.0;
		constexpr double Ahead = 6.0; // where it goes when the eye rests on nothing near

		const PropSpec& Spec = GetPropSpec(Kind);
		P::RayHit Hit;
		const Vec3 Spot = Physics.Raycast(Eye, Gaze, Reach, Hit) ? Hit.Point + Hit.Normal * (Spec.HalfHeight + 0.3) : Eye + Gaze * Ahead;
		SpawnProp(Kind, Spot, {});
	}

	void GameWorld::CullProps(double Time)
	{
		const double Dt = std::clamp(Time - LastCullTime, 0.0, 1.0);
		LastCullTime = Time;
		std::erase_if(Dust, [Time](const Crumbled& Item) { return Time > Item.Born + DustLife; });

		for (std::size_t Index = Props.size(); Index > 0; --Index)
		{
			P::Body& Item = Physics.GetBody(Props[Index - 1].Body);

			// A body must not outlive the ground under it, or it falls for ever.
			if (!HasGroundAt(Item.Position.X, Item.Position.Y))
			{
				RemoveProp(Index - 1);
				continue;
			}

			// Rule 17: it holds magic inside itself, kept up by the magic around it, and without it falls apart.
			const Sim::MagicMix Around = Magic != nullptr && Magic->Contains(Item.Position)
				? Magic->GetMix(Item.Position)
				: Sim::MagicBaseline(Item.Position, WellsAround, Sim::ReadMagicGround(Item.Position.X, Item.Position.Y, Sim::WorldSeed), Sim::WorldSeed);
			Prop& Thing = Props[Index - 1];
			Thing.Held = Sim::UpdateHeldMagic(Thing.Held, Around.GetTotal(), Around.GetAllFire(), Dt);
			if (!Sim::HoldsTogether(Thing.Held))
			{
				Dust.push_back({ Item.Position, GetPropSpec(Thing.Kind).HalfHeight, Time });
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

		// Magic with an aspect acts on what is in it, and is acted on as hard in return.
		const double Radius = std::cbrt(Spec.GetVolume() * 3.0 / (4.0 * Pi));
		MagicReading* Felt = nullptr;
		if (Magic != nullptr)
		{
			if (Index >= MagicByBody.size())
			{
				MagicByBody.resize(Index + 1);
			}
			Felt = &MagicByBody[Index];
			if (Time < Felt->Time || Time - Felt->Time >= AirReadingAge)
			{
				if (Magic->Contains(Item.Position))
				{
					if (LengthSquared(Felt->Impulse) > 0.0)
					{
						MagicPushes.push_back({ Item.Position, Felt->Impulse });
					}
					Felt->Mix = Magic->GetMix(Item.Position);
					Felt->Flow = Magic->GetFlow(Item.Position);
					Felt->AirSlope = Magic->GetSlope(Sim::EMagicType::Air, Item.Position);
					Felt->Rising = Sim::FireUpdraft(Felt->Mix.GetAllFire());
					Felt->FireSlope = Magic->GetSlope(Sim::EMagicType::Fire, Item.Position) + Magic->GetSlope(Sim::EMagicType::Lightning, Item.Position) + Magic->GetSlope(Sim::EMagicType::Light, Item.Position);
				}
				else
				{
					// Magic is everywhere, but only around the camera is it worked out as a gas. Elsewhere it lies at rest.
					Felt->Mix = Sim::MagicBaseline(Item.Position, WellsAround, Sim::ReadMagicGround(Item.Position.X, Item.Position.Y, Sim::WorldSeed), Sim::WorldSeed);
					Felt->Flow = {};
					Felt->AirSlope = {};
					Felt->Rising = 0.0;
					Felt->FireSlope = {};
				}
				Felt->Impulse = {};
				Felt->Time = Time;
			}

			// Only what an aspect has over Sim::MagicActsOnMatter pushes on anything. The swelling of air that
			// fire magic warms is the air's push, not the magic's.
			const Sim::MagicMix Acting = Felt->Mix.GetActing();
			const Vec3 Past = Felt->Flow - Item.Velocity;
			const bool bAirActs = Acting.Get(Sim::EMagicType::Air) > 0.0;
			const Vec3 Pushed = Sim::MagicDrag(Acting, Past, Spec.DragCoefficient, Spec.FrontalArea)
				+ Sim::MagicViscosity(Acting.Get(Sim::EMagicType::Water), Past, Radius)
				+ (bAirActs ? Sim::MagicPressure(Felt->AirSlope, Spec.GetVolume()) : Vec3{})
				+ Sim::FireBlast(Felt->FireSlope, Spec.GetVolume());
			Item.Force += Pushed;
			Felt->Impulse -= Pushed * Dt;
		}

		// Fire magic warms the air, and warm air rises and takes things with it. Streams of air magic take the air
		// along, and where it is dense enough it spins it into whirlwinds.
		const Vec3 Stirred = Felt != nullptr ? Sim::MagicStirredWind(Felt->Mix.Get(Sim::EMagicType::Air), Felt->Flow) : Vec3{};
		const Vec3 Moving = Air.Flow.Velocity + Stirred + Shows.GetWhirl(Item.Position) + Vec3{ 0.0, 0.0, Felt != nullptr ? Felt->Rising : 0.0 };
		Item.Force += (Sim::AeroForce(Moving - Item.Velocity, Spec.DragCoefficient, Spec.FrontalArea) + Sim::AirLift(Spec.GetVolume())) * Exposed;

		// Spinning air turns what is in it, until the body keeps pace at half the vorticity.
		const Vec3 Torque = Sim::AeroTorque(Air.Flow.Vorticity * 0.5 - Item.AngularVelocity, Radius);
		Item.AngularAcceleration += Torque * (Exposed / (0.4 * Item.GetMass() * Radius * Radius));

		if (Submerged > 0.0)
		{
			// Water carries along what floats in it, and is shoved aside by what comes in. Dense water magic drives it in a current.
			const Vec3 Flowing = Liquid.GetFlow(Item.Position.X, Item.Position.Y) + Shows.GetCurrent(Item.Position);
			Item.Force += Sim::WaterForce(Submerged, Spec.GetVolume(), Item.Velocity - Flowing, 1.2, Item.GetMass());
			Item.AngularAcceleration += Item.AngularVelocity * (-2.4 * Submerged);
		}
		DisplaceWater(Index, Item.Position, Submerged * Spec.GetVolume(), Spec.HalfHeight);

		const double Speed = Length(Item.Velocity);
		if (Speed > 0.05)
		{
			const double Stiffening = Felt != nullptr ? Sim::MagicStiffening(Felt->Mix.GetActing().Get(Sim::EMagicType::Nature)) : 1.0;
			Item.Force += Plants.Touch(Item.Position, Spec.HalfHeight, Item.Velocity, Item.GetMass(), Dt, Stiffening);
		}

		GroundPoint Ground;
		if (!ReadGround(Item.Position.X, Item.Position.Y, Ground))
		{
			return;
		}

		const double Clearance = Item.Position.Z - Spec.HalfHeight - Ground.Height;
		const double EarthMagic = Felt != nullptr ? Felt->Mix.GetActing().Get(Sim::EMagicType::Earth) : 0.0;
		if (Felt != nullptr)
		{
			const Vec3 Clung = Sim::MagicCling(EarthMagic, Clearance, Ground.Normal, Item.GetMass());
			Item.Force += Clung;
			Felt->Impulse -= Clung * Dt;
		}

		if (Clearance < GroundContact && Spec.bRolls)
		{
			// A round thing rolling on soft ground pushes a small bow wave of it along, which coulomb friction knows nothing of.
			const double Rolling = Sim::GetSurfaceGive(Ground.Surface).Rolling * Sim::Gravity * Ground.Normal.Z * Sim::MagicGrip(EarthMagic);
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
			Draw.Magic = static_cast<float>(Spec.Aspect) + 0.99f * static_cast<float>(std::clamp(Item.Held / Sim::MatterHolds, 0.0, 1.0));
			Out.push_back(Draw);
		}
	}
}
