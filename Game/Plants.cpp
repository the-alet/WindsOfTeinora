#include "Plants.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Teinora
{
	namespace
	{
		constexpr double TwoPi = 2.0 * Pi;
		constexpr double GoldenTurn = 2.399963;

		struct Color
		{
			double R;
			double G;
			double B;
		};

		/** Small and reproducible, so a plant grows back the same when its chunk reloads. */
		class Dice
		{
		public:
			explicit Dice(std::uint64_t Seed) : State(Seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}

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

		TerrainVertex MakeVertex(const Vec3& Position, const Vec3& Normal, std::uint8_t R, std::uint8_t G, std::uint8_t B, double Sway, ETerrainMaterial Material)
		{
			TerrainVertex Vertex;
			Vertex.Position[0] = static_cast<float>(Position.X);
			Vertex.Position[1] = static_cast<float>(Position.Y);
			Vertex.Position[2] = static_cast<float>(Position.Z);
			Vertex.Normal[0] = static_cast<std::int8_t>(Normal.X * 127.0);
			Vertex.Normal[1] = static_cast<std::int8_t>(Normal.Y * 127.0);
			Vertex.Normal[2] = static_cast<std::int8_t>(Normal.Z * 127.0);
			Vertex.Normal[3] = Material;
			Vertex.Color[0] = R;
			Vertex.Color[1] = G;
			Vertex.Color[2] = B;
			Vertex.Color[3] = ToByte(Sway);
			return Vertex;
		}

		Vec3 FindSide(const Vec3& Axis)
		{
			return Normalize(Cross(Axis, std::fabs(Axis.Z) < 0.9 ? Vec3{ 0.0, 0.0, 1.0 } : Vec3{ 1.0, 0.0, 0.0 }));
		}

		/** A slice through a tube. */
		struct Ring
		{
			Vec3 Center;
			double Radius;
			double Sway;
		};

		/** A smooth tube through its rings, bark side out. */
		void AddTube(std::vector<TerrainVertex>& Out, const std::vector<Ring>& Rings, int Sides, const Color& Tint)
		{
			const std::uint8_t R = ToByte(Tint.R);
			const std::uint8_t G = ToByte(Tint.G);
			const std::uint8_t B = ToByte(Tint.B);

			Vec3 Side = FindSide(Normalize(Rings[1].Center - Rings[0].Center));
			for (std::size_t Index = 0; Index + 1 < Rings.size(); ++Index)
			{
				const Ring& Lower = Rings[Index];
				const Ring& Upper = Rings[Index + 1];
				const Vec3 Along = Normalize(Upper.Center - Lower.Center);
				const Vec3 SideLow = Normalize(Side - Along * Dot(Side, Along));
				const Vec3 SideHigh = SideLow; // a slice this short hardly turns
				const Vec3 OtherLow = Cross(Along, SideLow);
				Side = SideLow;

				for (int Facet = 0; Facet < Sides; ++Facet)
				{
					const double Angle0 = TwoPi * Facet / Sides;
					const double Angle1 = TwoPi * (Facet + 1) / Sides;
					const Vec3 Out0 = SideLow * std::cos(Angle0) + OtherLow * std::sin(Angle0);
					const Vec3 Out1 = SideHigh * std::cos(Angle1) + OtherLow * std::sin(Angle1);

					const TerrainVertex A = MakeVertex(Lower.Center + Out0 * Lower.Radius, Out0, R, G, B, Lower.Sway, TerrainBark);
					const TerrainVertex C = MakeVertex(Lower.Center + Out1 * Lower.Radius, Out1, R, G, B, Lower.Sway, TerrainBark);
					const TerrainVertex D = MakeVertex(Upper.Center + Out1 * Upper.Radius, Out1, R, G, B, Upper.Sway, TerrainBark);
					const TerrainVertex E = MakeVertex(Upper.Center + Out0 * Upper.Radius, Out0, R, G, B, Upper.Sway, TerrainBark);
					Out.push_back(A);
					Out.push_back(C);
					Out.push_back(E);
					if (Upper.Radius > 0.0)
					{
						Out.push_back(C);
						Out.push_back(D);
						Out.push_back(E);
					}
				}
			}
		}

		/** A flat card that the alpha of its texture cuts into leaves. It shades as part of the rounded mass it belongs to. */
		void AddCard(std::vector<TerrainVertex>& Out, const Vec3& Center, const Vec3& AlongU, const Vec3& AlongV, const Vec3& Normal, double Shade, double Sway, ETerrainMaterial Material)
		{
			const std::uint8_t Tone = ToByte(Shade);
			const auto Corner = [&](double U, double V)
			{
				return MakeVertex(Center + AlongU * (U * 2.0 - 1.0) + AlongV * (V * 2.0 - 1.0), Normal, ToByte(U), ToByte(V), Tone, Sway, Material);
			};
			const TerrainVertex A = Corner(0.0, 0.0);
			const TerrainVertex B = Corner(1.0, 0.0);
			const TerrainVertex C = Corner(1.0, 1.0);
			const TerrainVertex D = Corner(0.0, 1.0);
			Out.push_back(A);
			Out.push_back(B);
			Out.push_back(C);
			Out.push_back(A);
			Out.push_back(C);
			Out.push_back(D);
		}

		class Grower
		{
		public:
			Grower(const Vec3& InBase, const Vec3& InOrigin, double InHeight, const PlantDetail& InDetail, std::uint64_t Seed, PlantOutput& InOut)
				: Base(InBase), Origin(InOrigin), Height(InHeight), Detail(InDetail), Rng(Seed), Out(InOut)
			{
			}

			void GrowFir();
			void GrowPine();
			void GrowBroadleaf(bool bBirch);
			void GrowBush();

		private:
			struct BranchStyle
			{
				Color Tint;
				double Rise;     // how much a branch bends upwards as it goes; below zero it droops
				double Spread;   // how wide its children part from it
				double Children; // how many of them
				double LeafShade;
				double LeafSize;
				ETerrainMaterial LeafMaterial;
			};

			Vec3 GetWorld(const Vec3& Local) const { return Local + Origin; }

			double GetSway(const Vec3& Point) const
			{
				const double Up = std::clamp((Point.Z - Base.Z) / Height, 0.0, 1.0);
				return 0.05 + 0.22 * Up * Up;
			}

			void AddLimb(const Vec3& Root, const Vec3& Tip, double Radius)
			{
				if (Out.Limbs == nullptr)
				{
					return;
				}

				// Bending a rod takes E pi r^4 / (4 L^3) at its end. Green wood is about 8 GPa, which
				// would make any branch rigid to a ball; these are springier, as a whole bough is.
				const double Length = std::max(Dot(Tip - Root, Normalize(Tip - Root)), 0.05);
				constexpr double Modulus = 7.0e7;
				const double Stiffness = 3.0 * Modulus * Pi * std::pow(Radius, 4.0) / (4.0 * Length * Length * Length);

				PlantLimb Limb;
				Limb.Root = GetWorld(Root);
				Limb.Tip = GetWorld(Tip);
				Limb.Radius = static_cast<float>(Radius);
				Limb.Stiffness = static_cast<float>(std::clamp(Stiffness, 25.0, 40000.0));
				Limb.Mass = static_cast<float>(std::clamp(700.0 * Pi * Radius * Radius * Length * 0.33 + 0.3, 0.3, 120.0));
				Out.Limbs->push_back(Limb);
			}

			void AddMass(const Vec3& Center, double Radius, double Density)
			{
				if (Out.Foliage != nullptr)
				{
					PlantFoliage Mass;
					Mass.Center = GetWorld(Center);
					Mass.Radius = static_cast<float>(Radius);
					Mass.Density = static_cast<float>(Density);
					Out.Foliage->push_back(Mass);
				}
			}

			/** A tuft of leaf cards around a point, lit as part of the crown around Heart. */
			void AddTuft(const Vec3& Where, const Vec3& Heart, double Radius, int Count, const BranchStyle& Style)
			{
				for (int Card = 0; Card < Count; ++Card)
				{
					const double Spin = Rng.Range(0.0, TwoPi);
					const double Tilt = Rng.Range(-0.7, 1.0);
					const Vec3 Face = Normalize(Vec3{ std::cos(Spin) * std::cos(Tilt), std::sin(Spin) * std::cos(Tilt), std::sin(Tilt) });
					const Vec3 AlongU = FindSide(Face);
					const Vec3 AlongV = Cross(Face, AlongU);
					const double Roll = Rng.Range(0.0, TwoPi);
					const double Size = Radius * Style.LeafSize * Detail.CardSize * Rng.Range(0.8, 1.2);

					const Vec3 Center = Where + Vec3{ Rng.Range(-1.0, 1.0), Rng.Range(-1.0, 1.0), Rng.Range(-0.6, 0.8) } * (Radius * 0.55);
					const Vec3 Outward = Normalize(Center - Heart + Vec3{ 0.0, 0.0, 0.2 });
					const Vec3 Normal = Normalize(Outward * 0.8 + Face * 0.2 + Vec3{ 0.0, 0.0, 0.25 });
					const double Shade = std::clamp(Style.LeafShade + Rng.Range(-0.12, 0.12), 0.0, 1.0);

					AddCard(*Out.Leaves, Center, (AlongU * std::cos(Roll) + AlongV * std::sin(Roll)) * Size, (AlongV * std::cos(Roll) - AlongU * std::sin(Roll)) * Size,
						Normal, Shade, GetSway(Center) + 0.25, Style.LeafMaterial);
				}
			}

			/** A branch and all that grows from it. */
			void GrowBranch(const Vec3& Start, const Vec3& Heading, double Length, double Radius, int Level, const Vec3& Heart, const BranchStyle& Style)
			{
				const int Slices = Level <= 1 ? 4 : 3;
				std::vector<Ring> Rings;
				Vec3 Point = Start;
				Vec3 Way = Heading;
				const double Step = Length / Slices;
				const double TipRadius = Radius * 0.3;

				for (int Slice = 0; Slice <= Slices; ++Slice)
				{
					const double Along = static_cast<double>(Slice) / Slices;
					Rings.push_back({ Point, Radius + (TipRadius - Radius) * Along, GetSway(Point) + 0.35 * Along * Level / std::max(Detail.BranchLevels, 1) });
					Way = Normalize(Way + Vec3{ Rng.Range(-0.22, 0.22), Rng.Range(-0.22, 0.22), Rng.Range(-0.22, 0.22) + Style.Rise });
					Point += Way * Step;
				}
				const Vec3 Tip = Rings.back().Center;

				if (Detail.BranchSides >= 3 && Radius > 0.012)
				{
					AddTube(*Out.Solid, Rings, Detail.BranchSides, Style.Tint);
				}
				AddLimb(Start, Tip, Radius);

				if (Level >= Detail.BranchLevels)
				{
					const double Reach = std::max(Length * 0.8, Height * 0.07);
					AddTuft(Tip, Heart, Reach, std::max(static_cast<int>(std::ceil(3.0 * Detail.Cards)), 1), Style);
					AddMass(Tip, Reach * 1.1, 2.5);
					return;
				}

				const int Children = static_cast<int>(std::round(Style.Children + Rng.Range(-0.5, 0.5)));
				const double Turn = Rng.Range(0.0, TwoPi);
				for (int Child = 0; Child < Children; ++Child)
				{
					const double Along = Rng.Range(0.45, 1.0);
					const std::size_t Slice = std::min(static_cast<std::size_t>(Along * Slices), static_cast<std::size_t>(Slices));
					const Vec3 From = Rings[Slice].Center;
					const Vec3 Side = FindSide(Way);
					const Vec3 Other = Cross(Way, Side);
					const double Around = Turn + Child * GoldenTurn;
					const Vec3 Part = Side * std::cos(Around) + Other * std::sin(Around);
					const Vec3 Ahead = Normalize(Way * std::cos(Style.Spread) + Part * std::sin(Style.Spread));
					GrowBranch(From, Ahead, Length * Rng.Range(0.5, 0.72), Rings[Slice].Radius * 0.62, Level + 1, Heart, Style);
				}
			}

			Vec3 Base;
			Vec3 Origin;
			double Height;
			const PlantDetail& Detail;
			Dice Rng;
			PlantOutput& Out;
		};

		void Grower::GrowFir()
		{
			const double Girth = Height * Rng.Range(0.014, 0.019);
			const Color Tint{ 0.26, 0.17, 0.11 };

			// A straight trunk that runs right up to the tip.
			const Vec3 Lean{ Rng.Range(-0.012, 0.012), Rng.Range(-0.012, 0.012), 1.0 };
			std::vector<Ring> Rings;
			for (int Slice = 0; Slice <= 5; ++Slice)
			{
				const double Along = Slice / 5.0;
				const Vec3 Point = Base + Lean * (Height * Along) - Vec3{ 0.0, 0.0, Slice == 0 ? 0.3 : 0.0 };
				Rings.push_back({ Point, Girth * (1.0 - Along) * (Slice == 0 ? 1.35 : 1.0), GetSway(Point) });
			}
			AddTube(*Out.Solid, Rings, Detail.TrunkSides, Tint);
			Out.TrunkRadius = Girth;
			Out.TrunkHeight = Height * 0.9;

			const double Reach = Height * Rng.Range(0.2, 0.27);
			const double Shade = Rng.Range(0.1, 0.45);
			const int Boughs = Detail.Whorls >= 8 ? 6 : 4;
			double Turn = Rng.Range(0.0, TwoPi);

			// Tiers of boughs: long and drooping below, short and lifted towards the top.
			for (int Whorl = 0; Whorl < Detail.Whorls; ++Whorl)
			{
				const double Share = (Whorl + Rng.Range(0.0, 0.5)) / Detail.Whorls;
				const double Up = 0.14 + 0.85 * Share;
				const Vec3 Joint = Base + Lean * (Height * Up);
				const double Length = (Reach * std::pow(1.0 - Up, 0.8) + Height * 0.02) * Rng.Range(0.85, 1.15);
				const double Droop = 0.55 - 1.2 * Up;
				Turn += GoldenTurn * 0.6;

				for (int Bough = 0; Bough < Boughs; ++Bough)
				{
					const double Around = Turn + TwoPi * (Bough + Rng.Range(-0.18, 0.18)) / Boughs;
					const Vec3 Ray{ std::cos(Around), std::sin(Around), 0.0 };
					const Vec3 Side{ -Ray.Y, Ray.X, 0.0 };

					// Out from the trunk, then down at the end under its own weight.
					const Vec3 Mid = Joint + Ray * (Length * 0.5) - Vec3{ 0.0, 0.0, Length * Droop * 0.22 };
					const Vec3 Tip = Joint + Ray * Length - Vec3{ 0.0, 0.0, Length * Droop * 0.62 };
					const double Width = Length * 0.5;
					const Vec3 Normal = Normalize(Vec3{ Ray.X * 0.55, Ray.Y * 0.55, 0.75 });
					const double Sway = GetSway(Tip) + 0.3;

					const auto AddBoughCard = [&](const Vec3& Across)
					{
						// Two lengths of one card, so that it can bend. Its stem runs along U.
						const auto Vertex = [&](const Vec3& Point, double U, double V, double Weight)
						{
							return MakeVertex(Point, Normal, ToByte(U), ToByte(V), ToByte(Shade), Weight, TerrainNeedle);
						};
						const TerrainVertex A = Vertex(Joint - Across, 0.0, 0.0, GetSway(Joint));
						const TerrainVertex B = Vertex(Mid - Across, 0.5, 0.0, Sway * 0.6);
						const TerrainVertex C = Vertex(Mid + Across, 0.5, 1.0, Sway * 0.6);
						const TerrainVertex D = Vertex(Joint + Across, 0.0, 1.0, GetSway(Joint));
						const TerrainVertex E = Vertex(Tip - Across * 0.7, 1.0, 0.0, Sway);
						const TerrainVertex F = Vertex(Tip + Across * 0.7, 1.0, 1.0, Sway);
						for (const TerrainVertex& Corner : { A, B, C, A, C, D, B, E, F, B, F, C })
						{
							Out.Leaves->push_back(Corner);
						}
					};
					AddBoughCard(Side * Width);
					if (Detail.Whorls >= 8)
					{
						AddBoughCard((Side * 0.5 + Vec3{ 0.0, 0.0, 0.8 }) * (Width * 0.75)); // a second, standing on edge, so that it is not a flat sheet from the side
					}

					AddLimb(Joint, Tip, Girth * (1.0 - Up) * 0.32 + 0.012);
				}

				if (Whorl % 4 == 1)
				{
					AddMass(Joint - Vec3{ 0.0, 0.0, Height * 0.04 }, Length * 0.95, 3.5);
				}
			}
		}

		void Grower::GrowPine()
		{
			const double Girth = Height * Rng.Range(0.016, 0.022);
			const BranchStyle Style{ { 0.40, 0.24, 0.14 }, 0.05, 0.75, 2.4, Rng.Range(0.25, 0.5), 0.8, TerrainNeedle };

			// A long clean trunk, a little crooked, with its crown high up.
			const double Crown = Rng.Range(0.55, 0.68);
			std::vector<Ring> Rings;
			Vec3 Point = Base - Vec3{ 0.0, 0.0, 0.3 };
			Vec3 Way{ Rng.Range(-0.05, 0.05), Rng.Range(-0.05, 0.05), 1.0 };
			constexpr int Slices = 6;
			for (int Slice = 0; Slice <= Slices; ++Slice)
			{
				const double Along = static_cast<double>(Slice) / Slices;
				Rings.push_back({ Point, Girth * (1.0 - 0.82 * Along) * (Slice == 0 ? 1.3 : 1.0), GetSway(Point) });
				Way = Normalize(Way + Vec3{ Rng.Range(-0.1, 0.1), Rng.Range(-0.1, 0.1), 0.0 });
				Point += Way * (Height / Slices);
			}
			AddTube(*Out.Solid, Rings, Detail.TrunkSides, { 0.36, 0.21, 0.12 });
			Out.TrunkRadius = Girth;
			Out.TrunkHeight = Height * 0.92;

			const Vec3 Heart = Base + Vec3{ 0.0, 0.0, Height * (Crown + 1.0) * 0.5 };
			const int Branches = Detail.BranchLevels >= 2 ? 11 : 7;
			double Turn = Rng.Range(0.0, TwoPi);
			for (int Branch = 0; Branch < Branches; ++Branch)
			{
				const double Share = (Branch + 0.5) / Branches;
				const double Up = Crown + (1.0 - Crown) * Share;
				const std::size_t Slice = std::min(static_cast<std::size_t>(Up * Slices), static_cast<std::size_t>(Slices - 1));
				const double Part = Up * Slices - static_cast<double>(Slice);
				const Vec3 From = Rings[Slice].Center + (Rings[Slice + 1].Center - Rings[Slice].Center) * Part;
				Turn += GoldenTurn;
				const Vec3 Ahead = Normalize(Vec3{ std::cos(Turn), std::sin(Turn), Rng.Range(0.35, 0.8) });
				const double Length = Height * Rng.Range(0.2, 0.3) * (1.25 - 0.75 * Share);
				GrowBranch(From, Ahead, Length, Girth * 0.32 * (1.2 - 0.6 * Share), 1, Heart, Style);
			}
		}

		void Grower::GrowBroadleaf(bool bBirch)
		{
			const double Girth = Height * (bBirch ? Rng.Range(0.011, 0.015) : Rng.Range(0.024, 0.034));
			const Color Bark = bBirch ? Color{ 0.82, 0.80, 0.74 } : Color{ 0.27, 0.21, 0.16 };
			const BranchStyle Style{ bBirch ? Color{ 0.42, 0.33, 0.28 } : Color{ 0.25, 0.19, 0.14 }, bBirch ? -0.12 : 0.1, bBirch ? 0.5 : 0.8, bBirch ? 2.4 : 2.8,
				bBirch ? Rng.Range(0.62, 0.88) : Rng.Range(0.2, 0.55), bBirch ? 0.62 : 0.72, TerrainLeaf };

			// The trunk thickens at the ground and gives way to its limbs at the fork.
			const double Fork = bBirch ? Rng.Range(0.2, 0.3) : Rng.Range(0.28, 0.4);
			std::vector<Ring> Rings;
			Vec3 Point = Base - Vec3{ 0.0, 0.0, 0.3 };
			Vec3 Way{ Rng.Range(-0.08, 0.08), Rng.Range(-0.08, 0.08), 1.0 };
			constexpr int Slices = 6;
			const double Stem = bBirch ? 0.9 : 0.72;
			for (int Slice = 0; Slice <= Slices; ++Slice)
			{
				const double Along = static_cast<double>(Slice) / Slices;
				const double Flare = 1.0 + 0.5 * std::exp(-Along * 12.0);
				Rings.push_back({ Point, Girth * (1.0 - 0.8 * Along) * Flare, GetSway(Point) });
				Way = Normalize(Way + Vec3{ Rng.Range(-0.12, 0.12), Rng.Range(-0.12, 0.12), 0.0 });
				Point += Way * (Height * Stem / Slices);
			}
			AddTube(*Out.Solid, Rings, Detail.TrunkSides, Bark);
			Out.TrunkRadius = Girth;
			Out.TrunkHeight = Height * Fork;

			const Vec3 Heart = Base + Vec3{ 0.0, 0.0, Height * (Fork + 1.0) * 0.5 };
			const int Limbs = bBirch ? 7 : 8;
			double Turn = Rng.Range(0.0, TwoPi);
			for (int Limb = 0; Limb < Limbs; ++Limb)
			{
				const double Share = (Limb + Rng.Range(0.2, 0.8)) / Limbs;
				const double Up = Fork / Stem + (1.0 - Fork / Stem) * Share;
				const std::size_t Slice = std::min(static_cast<std::size_t>(Up * Slices), static_cast<std::size_t>(Slices - 1));
				const double Part = Up * Slices - static_cast<double>(Slice);
				const Vec3 From = Rings[Slice].Center + (Rings[Slice + 1].Center - Rings[Slice].Center) * Part;
				const double FromRadius = Rings[Slice].Radius + (Rings[Slice + 1].Radius - Rings[Slice].Radius) * Part;
				Turn += GoldenTurn;

				const double Rise = bBirch ? Rng.Range(0.7, 1.4) - Share * 0.3 : Rng.Range(0.5, 1.1);
				const Vec3 Ahead = Normalize(Vec3{ std::cos(Turn), std::sin(Turn), Rise });
				const double Length = Height * (bBirch ? 0.26 : 0.34) * Rng.Range(0.85, 1.15) * (1.15 - 0.5 * Share);
				GrowBranch(From, Ahead, Length, FromRadius * (bBirch ? 0.42 : 0.55), 1, Heart, Style);
			}
		}

		void Grower::GrowBush()
		{
			const BranchStyle Style{ { 0.22, 0.17, 0.12 }, 0.12, 0.7, 2.0, Rng.Range(0.3, 0.75), 0.75, TerrainLeaf };
			const Vec3 Heart = Base + Vec3{ 0.0, 0.0, Height * 0.45 };
			const int Stems = 4 + static_cast<int>(Rng.Next() * 4.0);
			double Turn = Rng.Range(0.0, TwoPi);
			for (int Stem = 0; Stem < Stems; ++Stem)
			{
				Turn += GoldenTurn;
				const Vec3 Ahead = Normalize(Vec3{ std::cos(Turn) * 0.7, std::sin(Turn) * 0.7, 1.0 });
				GrowBranch(Base + Vec3{ std::cos(Turn), std::sin(Turn), -0.1 } * 0.12, Ahead, Height * Rng.Range(0.6, 0.85), Height * 0.012 + 0.006, 1, Heart, Style);
			}
			AddMass(Heart, Height * 0.7, 2.0);
		}
	}

	void GrowPlant(EPlantKind Kind, const Vec3& Base, const Vec3& Origin, double Height, const PlantDetail& Detail, std::uint64_t Seed, PlantOutput& Out)
	{
		Grower Plant(Base, Origin, Height, Detail, Seed, Out);
		switch (Kind)
		{
		case EPlantKind::Fir:   Plant.GrowFir(); break;
		case EPlantKind::Pine:  Plant.GrowPine(); break;
		case EPlantKind::Oak:   Plant.GrowBroadleaf(false); break;
		case EPlantKind::Birch: Plant.GrowBroadleaf(true); break;
		default:                Plant.GrowBush(); break;
		}
	}

	void PlantField::AddChunk(std::uint64_t Key, std::vector<PlantLimb>&& Limbs, std::vector<PlantFoliage>&& Foliage, const Vec3& Origin, double Size)
	{
		RemoveChunk(Key);
		Chunk& Target = Chunks[Key];
		Target.Origin = Origin;
		Target.Size = Size;
		Target.Limbs = std::move(Limbs);
		Target.Foliage = std::move(Foliage);
		Target.LimbStamps.assign(Target.Limbs.size(), 0u);
		Target.FoliageStamps.assign(Target.Foliage.size(), 0u);
		LimbCount += Target.Limbs.size();

		// Filed under every cell that a thing's bounds touch, a little beyond the chunk at the edge cells.
		const double Cell = Size / Cells;
		const auto ToCell = [&](double Value, double Start) { return std::clamp(static_cast<int>(std::floor((Value - Start) / Cell)), 0, Cells - 1); };
		const auto File = [&](std::vector<std::uint32_t>* Lists, std::uint32_t Index, double MinX, double MinY, double MaxX, double MaxY)
		{
			for (int Y = ToCell(MinY, Origin.Y); Y <= ToCell(MaxY, Origin.Y); ++Y)
			{
				for (int X = ToCell(MinX, Origin.X); X <= ToCell(MaxX, Origin.X); ++X)
				{
					Lists[X + Y * Cells].push_back(Index);
				}
			}
		};

		for (std::uint32_t Index = 0; Index < Target.Limbs.size(); ++Index)
		{
			const PlantLimb& Limb = Target.Limbs[Index];
			constexpr double Reach = 2.0; // what touches a limb can be this far off its line, bend included
			File(Target.LimbCells, Index, std::min(Limb.Root.X, Limb.Tip.X) - Reach, std::min(Limb.Root.Y, Limb.Tip.Y) - Reach,
				std::max(Limb.Root.X, Limb.Tip.X) + Reach, std::max(Limb.Root.Y, Limb.Tip.Y) + Reach);
		}
		for (std::uint32_t Index = 0; Index < Target.Foliage.size(); ++Index)
		{
			const PlantFoliage& Mass = Target.Foliage[Index];
			const double Reach = Mass.Radius + 1.0;
			File(Target.FoliageCells, Index, Mass.Center.X - Reach, Mass.Center.Y - Reach, Mass.Center.X + Reach, Mass.Center.Y + Reach);
		}
	}

	void PlantField::RemoveChunk(std::uint64_t Key)
	{
		const auto Found = Chunks.find(Key);
		if (Found != Chunks.end())
		{
			LimbCount -= Found->second.Limbs.size();
			Chunks.erase(Found);
		}
	}

	void PlantField::TouchChunk(Chunk& Target, const Vec3& Position, double Radius, const Vec3& Velocity, double Mass, double Dt, double Stiffening, Vec3& Force)
	{
		const double Cell = Target.Size / Cells;
		const int X = std::clamp(static_cast<int>((Position.X - Target.Origin.X) / Cell), 0, Cells - 1);
		const int Y = std::clamp(static_cast<int>((Position.Y - Target.Origin.Y) / Cell), 0, Cells - 1);

		// A spring pushing with explicit steps holds only while it is soft enough for the body it pushes.
		const double FirmestContact = Mass * 0.2 / (Dt * Dt);

		for (const std::uint32_t Index : Target.LimbCells[X + Y * Cells])
		{
			if (Target.LimbStamps[Index] == Stamp)
			{
				continue;
			}
			Target.LimbStamps[Index] = Stamp;

			const PlantLimb& Limb = Target.Limbs[Index];
			const auto Moved = Target.Moving.find(Index);
			const Vec3 Bend = Moved != Target.Moving.end() ? Moved->second.Bend : Vec3{};
			const Vec3 Line = Limb.Tip + Bend - Limb.Root;
			const double Along = std::clamp(Dot(Position - Limb.Root, Line) / std::max(LengthSquared(Line), 1e-6), 0.0, 1.0);

			const double Thick = static_cast<double>(Limb.Radius) * (1.0 - 0.7 * Along) + 0.04; // twigs and needles stand out from the wood
			const Vec3 Apart = Position - (Limb.Root + Line * Along);
			const double Distance = Length(Apart);
			const double Sunk = Radius + Thick - Distance;
			if (Sunk <= 0.0 || Distance < 1e-5)
			{
				continue;
			}

			// Nearer the tip a limb has more length to bend with, and so gives more easily.
			const double Lever = std::max(Along, 0.12);
			const double Springiness = std::min(static_cast<double>(Limb.Stiffness) * Stiffening / (Lever * Lever), FirmestContact);
			const Vec3 Normal = Apart * (1.0 / Distance);
			const Vec3 LimbSpeed = Moved != Target.Moving.end() ? Moved->second.Speed * Along : Vec3{};
			const double Closing = Dot(Velocity - LimbSpeed, Normal);
			const double Damping = 2.0 * 0.12 * std::sqrt(Springiness * Mass);
			const Vec3 Push = Normal * std::max(Springiness * Sunk - Damping * Closing, 0.0);
			Force += Push;

			// What pushes the body pushes the limb the other way, at the tip through its lever.
			Motion& Swing = Moved != Target.Moving.end() ? Moved->second : Target.Moving[Index];
			Swing.Speed -= Push * (Lever * Dt / static_cast<double>(Limb.Mass));
		}

		const double Speed = Length(Velocity);
		for (const std::uint32_t Index : Target.FoliageCells[X + Y * Cells])
		{
			if (Target.FoliageStamps[Index] == Stamp)
			{
				continue;
			}
			Target.FoliageStamps[Index] = Stamp;

			const PlantFoliage& Leaves = Target.Foliage[Index];
			const Vec3 Apart = Position - Leaves.Center;
			const double Distance = Length(Apart);
			if (Distance >= static_cast<double>(Leaves.Radius) || Speed < 0.1)
			{
				continue;
			}

			// Drag from what it brushes past, in a mass thickest at its heart, and a sideways knock from each spray of leaves.
			const double Thickness = 1.0 - Distance / static_cast<double>(Leaves.Radius);
			const double Area = Pi * Radius * Radius;
			const double Pull = 0.5 * static_cast<double>(Leaves.Density) * Area * Speed * Thickness;
			Force -= Velocity * Pull;
			const double Spin = Position.X * 1.7 + Position.Y * 2.3 + Position.Z * 2.9;
			Force += Vec3{ std::sin(Spin), std::cos(Spin * 1.3), std::sin(Spin * 0.7) } * (Pull * Speed * 0.22);

			Motion& Swing = Target.MovingLeaves[Index];
			Swing.Speed += Velocity * (Pull * Dt * 0.6 / std::max(static_cast<double>(Leaves.Density) * static_cast<double>(Leaves.Radius), 0.5));
		}
	}

	Vec3 PlantField::Touch(const Vec3& Position, double Radius, const Vec3& Velocity, double Mass, double Dt, double Stiffening)
	{
		Vec3 Force;
		++Stamp;

		// What grows near the edge of a chunk reaches over it, so its neighbours get asked as well.
		constexpr double Overhang = 12.0;
		for (auto& [Key, Target] : Chunks)
		{
			if (Position.X >= Target.Origin.X - Overhang && Position.X < Target.Origin.X + Target.Size + Overhang
				&& Position.Y >= Target.Origin.Y - Overhang && Position.Y < Target.Origin.Y + Target.Size + Overhang)
			{
				TouchChunk(Target, Position, Radius, Velocity, Mass, Dt, Stiffening, Force);
			}
		}
		return Force;
	}

	void PlantField::Advance(double Dt)
	{
		constexpr double Settled = 0.002;

		const auto Swing = [Dt](std::unordered_map<std::uint32_t, Motion>& Moving, const auto& GetStiffness)
		{
			for (auto It = Moving.begin(); It != Moving.end();)
			{
				Motion& Item = It->second;
				const double Rate = GetStiffness(It->first); // of the swing, squared
				const double Damping = 2.0 * 0.09 * std::sqrt(Rate);
				Item.Speed -= (Item.Bend * Rate + Item.Speed * Damping) * Dt;
				Item.Bend += Item.Speed * Dt;

				const double Size = LengthSquared(Item.Bend) + LengthSquared(Item.Speed) * Dt * Dt;
				It = Size < Settled * Settled ? Moving.erase(It) : std::next(It);
			}
		};

		for (auto& [Key, Target] : Chunks)
		{
			Swing(Target.Moving, [&Target](std::uint32_t Index)
			{
				const PlantLimb& Limb = Target.Limbs[Index];
				return static_cast<double>(Limb.Stiffness) / static_cast<double>(Limb.Mass);
			});
			Swing(Target.MovingLeaves, [](std::uint32_t) { return 60.0; });
		}
	}

	void PlantField::CollectMoves(const Vec3& Eye, std::vector<PlantMove>& Out, std::size_t Most) const
	{
		Out.clear();
		struct Candidate
		{
			double Rank;
			PlantMove Move;
		};
		std::vector<Candidate> Found;

		for (const auto& [Key, Target] : Chunks)
		{
			for (const auto& [Index, Item] : Target.Moving)
			{
				const PlantLimb& Limb = Target.Limbs[Index];
				const Vec3 Middle = (Limb.Root + Limb.Tip) * 0.5 + Item.Bend * 0.5;
				Found.push_back({ LengthSquared(Middle - Eye) / std::max(LengthSquared(Item.Bend), 1e-4), { Limb.Tip + Item.Bend * 0.5, Length(Limb.Tip - Limb.Root) * 0.9, Item.Bend } });
			}
			for (const auto& [Index, Item] : Target.MovingLeaves)
			{
				const PlantFoliage& Mass = Target.Foliage[Index];
				Found.push_back({ LengthSquared(Mass.Center - Eye) / std::max(LengthSquared(Item.Bend), 1e-4), { Mass.Center, static_cast<double>(Mass.Radius) * 1.3, Item.Bend } });
			}
		}

		std::sort(Found.begin(), Found.end(), [](const Candidate& A, const Candidate& B) { return A.Rank < B.Rank; });
		for (std::size_t Index = 0; Index < std::min(Found.size(), Most); ++Index)
		{
			Out.push_back(Found[Index].Move);
		}
	}
}
