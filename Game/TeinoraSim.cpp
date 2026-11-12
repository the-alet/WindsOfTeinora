#include "TeinoraSim.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <random>
#include <vector>

namespace Teinora::Sim
{
	namespace
	{
		constexpr double TwoPi = 6.283185307179586;

		/** One wave of the turbulence. Its velocity is at right angles to Wave, so it carries no divergence. */
		struct EddyMode
		{
			Vec3 Wave;         // radians per metre
			double WaveNumber; // the length of Wave
			Vec3 AlongCosine;  // velocity amplitudes of the two phases
			Vec3 AlongSine;
			Vec3 SpinOfCosine; // Wave crossed with each, which is what the curl comes to
			Vec3 SpinOfSine;
			double Churn;      // radians per second: small eddies turn over faster
			double Offset;
		};

		std::vector<EddyMode> MakeEddyModes()
		{
			constexpr int Count = 64;
			constexpr double Largest = 160.0;
			constexpr double Smallest = 2.0;
			constexpr double TypicalSpeed = 1.5; // m/s, of the largest eddies

			std::minstd_rand Random(0x7E1Au);
			const auto Next = [&Random] { return (static_cast<double>(Random()) - 1.0) / 2147483645.0; };
			const auto NextDirection = [&Next]
			{
				const double Z = 2.0 * Next() - 1.0;
				const double Angle = TwoPi * Next();
				const double Flat = std::sqrt(1.0 - Z * Z);
				return Vec3{ Flat * std::cos(Angle), Flat * std::sin(Angle), Z };
			};

			std::vector<EddyMode> Modes(Count);
			double Energy = 0.0;
			for (int Index = 0; Index < Count; ++Index)
			{
				// Evenly spread in log k. E(k) ~ k^-5/3 then gives each band an energy of k^-2/3.
				const double Size = Smallest * std::pow(Largest / Smallest, (Index + Next()) / Count);
				const double Number = TwoPi / Size;
				const double Amplitude = std::pow(Number, -1.0 / 3.0);
				const Vec3 Direction = NextDirection();

				EddyMode& Mode = Modes[static_cast<std::size_t>(Index)];
				Mode.Wave = Direction * Number;
				Mode.WaveNumber = Number;
				Mode.AlongCosine = Normalize(Cross(Direction, NextDirection())) * Amplitude;
				Mode.AlongSine = Normalize(Cross(Direction, NextDirection())) * Amplitude;
				Mode.Churn = TypicalSpeed * std::pow(Number, 2.0 / 3.0) * std::pow(Largest / TwoPi, -1.0 / 3.0);
				Mode.Offset = TwoPi * Next();
				Energy += Amplitude * Amplitude;
			}

			// Averaged over phase, each mode holds its amplitude squared; three axes share the total.
			const double Scale = std::sqrt(3.0 / Energy);
			for (EddyMode& Mode : Modes)
			{
				Mode.AlongCosine = Mode.AlongCosine * Scale;
				Mode.AlongSine = Mode.AlongSine * Scale;
				Mode.SpinOfCosine = Cross(Mode.Wave, Mode.AlongCosine);
				Mode.SpinOfSine = Cross(Mode.Wave, Mode.AlongSine);
			}
			return Modes;
		}

		std::uint32_t HashLattice(std::int64_t X, std::int64_t Y, std::uint32_t Seed)
		{
			std::uint32_t H = Seed ^ 0x811C9DC5u;
			const auto Mix = [&H](std::uint32_t Value)
			{
				H ^= Value;
				H *= 0x01000193u;
				H ^= H >> 15;
			};
			Mix(static_cast<std::uint32_t>(X));
			Mix(static_cast<std::uint32_t>(X >> 32));
			Mix(static_cast<std::uint32_t>(Y));
			Mix(static_cast<std::uint32_t>(Y >> 32));
			H *= 0x5BD1E995u;
			return H ^ (H >> 13);
		}

		double Fade(double T)
		{
			return T * T * T * (T * (T * 6.0 - 15.0) + 10.0);
		}

		double Mix(double A, double B, double T)
		{
			return A + (B - A) * T;
		}

		double SmoothStep(double Edge0, double Edge1, double Value)
		{
			const double T = std::clamp((Value - Edge0) / (Edge1 - Edge0), 0.0, 1.0);
			return T * T * (3.0 - 2.0 * T);
		}

		double LatticeGradient(std::int64_t CellX, std::int64_t CellY, double DX, double DY, std::uint32_t Seed)
		{
			const double Angle = HashLattice(CellX, CellY, Seed) / 4294967296.0 * TwoPi;
			return std::cos(Angle) * DX + std::sin(Angle) * DY;
		}

		/** Noise with its slope, which tells the terrain where the ground is already steep. */
		struct SlopedNoise
		{
			double Value;
			double DX;
			double DY;
		};

		SlopedNoise GradientNoiseSloped(double X, double Y, std::uint32_t Seed)
		{
			const double FloorX = std::floor(X);
			const double FloorY = std::floor(Y);
			const auto CellX = static_cast<std::int64_t>(FloorX);
			const auto CellY = static_cast<std::int64_t>(FloorY);
			const double PartX = X - FloorX;
			const double PartY = Y - FloorY;

			struct Pull
			{
				double X;
				double Y;
			};
			const auto PullAt = [Seed](std::int64_t Column, std::int64_t Row)
			{
				const double Angle = HashLattice(Column, Row, Seed) / 4294967296.0 * TwoPi;
				return Pull{ std::cos(Angle), std::sin(Angle) };
			};
			const Pull A = PullAt(CellX, CellY);
			const Pull B = PullAt(CellX + 1, CellY);
			const Pull C = PullAt(CellX, CellY + 1);
			const Pull D = PullAt(CellX + 1, CellY + 1);

			const double VA = A.X * PartX + A.Y * PartY;
			const double VB = B.X * (PartX - 1.0) + B.Y * PartY;
			const double VC = C.X * PartX + C.Y * (PartY - 1.0);
			const double VD = D.X * (PartX - 1.0) + D.Y * (PartY - 1.0);

			const double U = Fade(PartX);
			const double V = Fade(PartY);
			const double DU = 30.0 * PartX * PartX * (PartX - 1.0) * (PartX - 1.0);
			const double DV = 30.0 * PartY * PartY * (PartY - 1.0) * (PartY - 1.0);

			const double K1 = VB - VA;
			const double K2 = VC - VA;
			const double K3 = VA - VB - VC + VD;

			constexpr double Scale = 1.41421356;
			SlopedNoise Result;
			Result.Value = (VA + K1 * U + K2 * V + K3 * U * V) * Scale;
			Result.DX = (A.X + U * (B.X - A.X) + V * (C.X - A.X) + U * V * (A.X - B.X - C.X + D.X) + DU * (K1 + K3 * V)) * Scale;
			Result.DY = (A.Y + U * (B.Y - A.Y) + V * (C.Y - A.Y) + U * V * (A.Y - B.Y - C.Y + D.Y) + DV * (K2 + K3 * U)) * Scale;
			return Result;
		}

		/**
		 * Fractal noise that goes quiet where earlier octaves are already steep. Detail then gathers on
		 * crests and valley floors while the slopes between stay smooth, which is how worn ground looks.
		 */
		double WornNoise(double X, double Y, std::uint32_t Seed, int Octaves, double Wear)
		{
			double Sum = 0.0;
			double Amplitude = 1.0;
			double Total = 0.0;
			double SlopeX = 0.0;
			double SlopeY = 0.0;
			for (int Octave = 0; Octave < Octaves; ++Octave)
			{
				const SlopedNoise Layer = GradientNoiseSloped(X, Y, Seed + static_cast<std::uint32_t>(Octave) * 131u);
				SlopeX += Layer.DX;
				SlopeY += Layer.DY;
				Sum += Amplitude * Layer.Value / (1.0 + Wear * (SlopeX * SlopeX + SlopeY * SlopeY));
				Total += Amplitude;
				Amplitude *= 0.5;
				X = X * 2.03 + 17.3;
				Y = Y * 2.03 - 9.1;
			}
			return Sum / Total;
		}

		/** The same with sharp crests, from the folds of the noise: 1 along a crest and falling away from it. */
		double WornRidges(double X, double Y, std::uint32_t Seed, int Octaves, double Wear)
		{
			double Sum = 0.0;
			double Amplitude = 1.0;
			double Total = 0.0;
			double SlopeX = 0.0;
			double SlopeY = 0.0;
			double Weight = 1.0;
			for (int Octave = 0; Octave < Octaves; ++Octave)
			{
				const SlopedNoise Layer = GradientNoiseSloped(X, Y, Seed + static_cast<std::uint32_t>(Octave) * 131u);
				// A softened absolute value: a true one would flip the slope, and with it the wear, at every crest.
				const double Folded = std::sqrt(Layer.Value * Layer.Value + 0.012);
				const double Crest = 1.1 - Folded;
				const double Side = -Layer.Value / Folded;
				SlopeX += Layer.DX * Side;
				SlopeY += Layer.DY * Side;
				const double Layered = Crest * Crest * Weight;
				Sum += Amplitude * Layered / (1.0 + Wear * (SlopeX * SlopeX + SlopeY * SlopeY));
				Total += Amplitude;
				Weight = std::clamp(Layered * 1.6, 0.0, 1.0); // fine ridges keep to the big ones
				Amplitude *= 0.5;
				X = X * 2.03 + 17.3;
				Y = Y * 2.03 - 9.1;
			}
			return Sum / Total;
		}

		double SoftMax(double A, double B, double Softness)
		{
			const double Mixed = std::clamp(0.5 + 0.5 * (A - B) / Softness, 0.0, 1.0);
			return Mix(B, A, Mixed) + Softness * Mixed * (1.0 - Mixed);
		}

		/** The land and sea floor before any lakes are let into them. */
		double ShapeGround(double X, double Y, std::uint32_t Seed)
		{
			// Domain warping breaks up the axis-aligned look of lattice noise.
			const double WarpX = X + 180.0 * FractalNoise(X / 1100.0, Y / 1100.0, Seed + 1u, 3);
			const double WarpY = Y + 180.0 * FractalNoise(X / 1100.0 + 40.0, Y / 1100.0 - 25.0, Seed + 2u, 3);

			// Continents, whose edge is broken into bays and headlands by a finer layer.
			const double Continent = FractalNoise(WarpX / 6400.0 + 0.37, WarpY / 6400.0 + 0.71, Seed + 3u, 5) + 0.05;
			const double Coast = Continent + 0.055 * FractalNoise(WarpX / 520.0, WarpY / 520.0, Seed + 14u, 3);

			if (Coast < 0.0)
			{
				// A shelf that slopes off the beach, drops at its edge, and levels out into the deep.
				const double Shallows = SmoothStep(0.0, -0.014, Coast) * 1.6;
				const double Shelf = SmoothStep(-0.004, -0.075, Coast) * 8.0;
				const double Deep = SmoothStep(-0.07, -0.26, Coast);
				const double Floor = -(Shallows + Shelf + Deep * 30.0);

				// Bars of sand near the shore, reefs along the shelf, and broad valleys in the deep.
				const double Bars = (FractalNoise(WarpX / 110.0, WarpY / 74.0, Seed + 15u, 2) * 0.9) * SmoothStep(0.0, -0.02, Coast);
				const double Reefs = std::max(0.0, FractalNoise(WarpX / 70.0, WarpY / 70.0, Seed + 16u, 3) - 0.18) * 7.0 * SmoothStep(-0.012, -0.05, Coast);
				const double Valleys = WornNoise(WarpX / 1500.0, WarpY / 1500.0, Seed + 17u, 3, 0.4) * 11.0 * Deep;
				return std::min(Floor + Bars + Reefs + Valleys, -0.05);
			}

			const double Beach = std::min(Coast / 0.02, 1.0) * 2.4;
			const double Inland = SmoothStep(0.012, 0.11, Coast);

			// Plains lie between hills, and the mountains stand in ranges with long worn flanks.
			const double Rolling = SmoothStep(-0.25, 0.3, FractalNoise(WarpX / 2600.0, WarpY / 2600.0, Seed + 18u, 2));
			const double Hills = WornNoise(WarpX / 900.0, WarpY / 900.0, Seed + 4u, 5, 0.55) * (10.0 + 40.0 * Rolling);
			const double MountainMask = SmoothStep(-0.04, 0.3, FractalNoise(WarpX / 4600.0, WarpY / 4600.0, Seed + 6u, 3));
			const double Ranges = MountainMask > 0.0 ? WornRidges(WarpX / 2300.0, WarpY / 2300.0, Seed + 5u, 6, 0.09) : 0.0;
			const double Mountains = 620.0 * Ranges * Ranges * MountainMask;

			// Long valleys wind through the high ground, flat along the bottom.
			const double Course = std::abs(FractalNoise(WarpX / 2900.0 + 3.1, WarpY / 2900.0 - 1.7, Seed + 19u, 3));
			const double Valley = 1.0 - SmoothStep(0.02, 0.2, Course);

			// Tablelands in parts of the middle ground, cut into steps that break into low cliffs.
			const double Tableland = SmoothStep(0.2, 0.42, FractalNoise(WarpX / 3300.0 - 9.0, WarpY / 3300.0 + 5.0, Seed + 20u, 2)) * (1.0 - MountainMask);
			const double Raised = FractalNoise(WarpX / 1300.0, WarpY / 1300.0, Seed + 21u, 3) * 60.0 + 36.0;
			const double Stepped = std::floor(Raised / 14.0) + SmoothStep(0.45, 1.0, Raised / 14.0 - std::floor(Raised / 14.0));

			// Rolling ground at walking scale, so that no field is a plane.
			const double Mounds = FractalNoise(WarpX / 170.0, WarpY / 170.0, Seed + 8u, 4) * (4.0 + 7.0 * Rolling);

			// Fine relief that only the denser terrain meshes resolve; rougher on the mountains.
			const double Fine = FractalNoise(WarpX / 46.0, WarpY / 46.0, Seed + 7u, 3) * (1.6 + 3.6 * MountainMask);
			const double Bumps = FractalNoise(X / 13.0, Y / 13.0, Seed + 9u, 2) * 0.6;

			const double Low = 8.0 + Hills + Mix(0.0, Stepped * 14.0, Tableland);
			const double High = Low + Mountains;
			const double Carved = Mix(High, Low * 0.55 + Mountains * 0.2 + 3.0, Valley * 0.85);
			const double Dry = SoftMax(Carved + Mounds + Fine + Bumps, 4.4, 3.0); // no hollow inland sinks to the level of the shore
			return Mix(Beach, Dry, Inland);
		}

		// One lake at most in each cell of a grid. Each keeps to the middle of its cell and is small enough
		// that what it does to the ground, out to LakeInfluence radii, cannot reach the next lake's.
		constexpr double LakeSpacing = 1800.0;
		constexpr double LakeChance = 0.9;
		constexpr double LakeInfluence = 1.9;
		constexpr double LakeWander = 0.15;  // of a cell: how far from the middle of its cell a lake can lie
		constexpr double LargestLake = 330.0; // the radius times the stretch, which keeps it clear of the next

		struct LakeCell
		{
			std::uint64_t Key = ~0ull;
			bool bLake = false;
			Lake Value;
		};

		bool MakeLake(std::int64_t CellX, std::int64_t CellY, std::uint32_t Seed, Lake& Out)
		{
			const auto Share = [=](std::uint32_t Salt) { return HashLattice(CellX, CellY, Seed + 400u + Salt) / 4294967296.0; };
			if (Share(0u) > LakeChance)
			{
				return false;
			}

			const double Size = Share(3u);
			Out.X = (static_cast<double>(CellX) + 0.5 + LakeWander * (2.0 * Share(1u) - 1.0)) * LakeSpacing;
			Out.Y = (static_cast<double>(CellY) + 0.5 + LakeWander * (2.0 * Share(2u) - 1.0)) * LakeSpacing;
			Out.Stretch = 1.0 + 1.5 * Share(5u) * Share(6u);
			Out.Radius = std::min(60.0 + 220.0 * Size * Size, LargestLake / Out.Stretch);
			Out.Angle = TwoPi * Share(7u);
			Out.Id = HashLattice(CellX, CellY, Seed + 409u);

			// The water can rise no higher than the lowest ground around it.
			double Lowest = ShapeGround(Out.X, Out.Y, Seed);
			double Highest = Lowest;
			constexpr int Samples = 14;
			const double AlongX = std::cos(Out.Angle);
			const double AlongY = std::sin(Out.Angle);
			for (int Sample = 0; Sample < Samples; ++Sample)
			{
				const double Angle = TwoPi * Sample / Samples;
				const double Long = std::cos(Angle) * Out.Radius * Out.Stretch * 1.3;
				const double Wide = std::sin(Angle) * Out.Radius * 1.3;
				const double Ground = ShapeGround(Out.X + AlongX * Long - AlongY * Wide, Out.Y + AlongY * Long + AlongX * Wide, Seed);
				Lowest = std::min(Lowest, Ground);
				Highest = std::max(Highest, Ground);
			}

			// Ground that falls away too steeply cannot hold a lake, and near the sea it would be the sea.
			if (Lowest < 7.0 || Highest - Lowest > 55.0 + Out.Radius * Out.Stretch * 0.22)
			{
				return false;
			}

			Out.Level = Lowest - 0.6;
			Out.Depth = std::min(3.0 + Out.Radius * 0.03 + 6.0 * Share(4u), Out.Level - SeaLevel - 1.0); // or the sea's surface would show through its bed
			return Out.Depth >= 2.0;
		}

		/** Lakes are only worked out once per thread, or every height near one would pay for it. */
		const LakeCell& GetLakeCell(std::int64_t CellX, std::int64_t CellY, std::uint32_t Seed)
		{
			constexpr std::size_t CacheSize = 256;
			thread_local std::array<LakeCell, CacheSize> Cache;

			const std::uint64_t Key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(CellX)) << 32) ^ static_cast<std::uint32_t>(CellY) ^ (static_cast<std::uint64_t>(Seed) << 13);
			LakeCell& Slot = Cache[(static_cast<std::size_t>(CellX) * 31u + static_cast<std::size_t>(CellY)) % CacheSize];
			if (Slot.Key != Key)
			{
				Slot.Key = Key;
				Slot.bLake = MakeLake(CellX, CellY, Seed, Slot.Value);
			}
			return Slot;
		}

		/** The lake whose influence takes in a point, if any. There can only be one. */
		const Lake* FindNearbyLake(double X, double Y, std::uint32_t Seed)
		{
			const auto CellX = static_cast<std::int64_t>(std::floor(X / LakeSpacing));
			const auto CellY = static_cast<std::int64_t>(std::floor(Y / LakeSpacing));
			for (std::int64_t Row = CellY - 1; Row <= CellY + 1; ++Row)
			{
				for (std::int64_t Column = CellX - 1; Column <= CellX + 1; ++Column)
				{
					const LakeCell& Cell = GetLakeCell(Column, Row, Seed);
					if (Cell.bLake)
					{
						const double Reach = Cell.Value.Radius * Cell.Value.Stretch * LakeInfluence;
						const double OffX = X - Cell.Value.X;
						const double OffY = Y - Cell.Value.Y;
						if (OffX * OffX + OffY * OffY < Reach * Reach)
						{
							return &Cell.Value;
						}
					}
				}
			}
			return nullptr;
		}

		/** How far out a point lies in a lake's basin: 1 about at the shore, which wanders in and out. */
		double GetLakeReach(const Lake& Item, double X, double Y, std::uint32_t Seed)
		{
			const double OffX = X - Item.X;
			const double OffY = Y - Item.Y;
			const double Long = (OffX * std::cos(Item.Angle) + OffY * std::sin(Item.Angle)) / Item.Stretch;
			const double Wide = OffY * std::cos(Item.Angle) - OffX * std::sin(Item.Angle);
			const double Wander = FractalNoise(X / (Item.Radius * 0.75), Y / (Item.Radius * 0.75), Seed + 410u + (Item.Id & 15u), 3);
			return std::max(std::hypot(Long, Wide) / Item.Radius + Wander * 0.3, 0.0);
		}

		constexpr double BasinHeld = 1.45; // how far the ground is held above the water, in radii
		constexpr double BasinEnd = 1.75;  // and where it has gone back to what it was
		constexpr double BeachFall = 0.25; // metres of height a shore counts for each metre from the water

		double LetLakeIn(const Lake& Item, double X, double Y, double Ground, std::uint32_t Seed)
		{
			constexpr double ShoreRise = 0.35; // how far the ground stands above the water where the basin ends
			constexpr double BankSlope = 0.12; // and how it climbs from there, for the width of a beach
			constexpr double BankRise = 1.7;   // but not so far that the lake stands in a ring of its own

			const double Reach = GetLakeReach(Item, X, Y, Seed);
			if (Reach >= BasinEnd)
			{
				return Ground;
			}

			const double Bank = Item.Level + ShoreRise + std::min(std::max(Reach - 1.0, 0.0) * Item.Radius * BankSlope, BankRise);
			if (Reach >= 1.0)
			{
				// Everywhere the water might reach, the ground holds it in, however low the land beyond.
				const double Back = SmoothStep(1.0, BasinEnd, Reach);
				return std::max(Mix(Bank, Ground, Back), Mix(Item.Level + ShoreRise, Ground, SmoothStep(BasinHeld, BasinEnd, Reach)));
			}

			// The bed shelves off the shore into a hollow, with ledges and banks of its own.
			const double Hollow = 1.0 - std::pow(Reach, 2.2);
			const double Relief = (WornNoise(X / 64.0, Y / 64.0, Seed + 430u, 3, 0.3) * 1.5 + FractalNoise(X / 17.0, Y / 17.0, Seed + 431u, 2) * 0.3) * SmoothStep(0.0, 0.3, Hollow);
			return Bank - (Item.Depth + ShoreRise) * Hollow + Relief;
		}

		std::atomic<double> WindSpeedSetting{ WorldConditions{}.WindSpeed };
		std::atomic<double> WindHeadingSetting{ WorldConditions{}.WindHeading };
		std::atomic<double> WaveHeightSetting{ WorldConditions{}.WaveHeight };
	}

	WorldConditions GetConditions()
	{
		WorldConditions Result;
		Result.WindSpeed = WindSpeedSetting.load(std::memory_order_relaxed);
		Result.WindHeading = WindHeadingSetting.load(std::memory_order_relaxed);
		Result.WaveHeight = WaveHeightSetting.load(std::memory_order_relaxed);
		return Result;
	}

	void SetConditions(const WorldConditions& Conditions)
	{
		WindSpeedSetting.store(Conditions.WindSpeed, std::memory_order_relaxed);
		WindHeadingSetting.store(Conditions.WindHeading, std::memory_order_relaxed);
		WaveHeightSetting.store(Conditions.WaveHeight, std::memory_order_relaxed);
	}

	double GradientNoise(double X, double Y, std::uint32_t Seed)
	{
		const double FloorX = std::floor(X);
		const double FloorY = std::floor(Y);
		const auto CellX = static_cast<std::int64_t>(FloorX);
		const auto CellY = static_cast<std::int64_t>(FloorY);
		const double DX = X - FloorX;
		const double DY = Y - FloorY;

		const double N00 = LatticeGradient(CellX, CellY, DX, DY, Seed);
		const double N10 = LatticeGradient(CellX + 1, CellY, DX - 1.0, DY, Seed);
		const double N01 = LatticeGradient(CellX, CellY + 1, DX, DY - 1.0, Seed);
		const double N11 = LatticeGradient(CellX + 1, CellY + 1, DX - 1.0, DY - 1.0, Seed);

		const double U = Fade(DX);
		// sqrt(2) rescales the unit-gradient range of about +-0.71 to +-1.
		return Mix(Mix(N00, N10, U), Mix(N01, N11, U), Fade(DY)) * 1.41421356;
	}

	double TiledNoise(double X, double Y, int PeriodX, int PeriodY, std::uint32_t Seed)
	{
		const double FloorX = std::floor(X);
		const double FloorY = std::floor(Y);
		const double DX = X - FloorX;
		const double DY = Y - FloorY;
		const std::int64_t CellX = (static_cast<std::int64_t>(FloorX) % PeriodX + PeriodX) % PeriodX;
		const std::int64_t CellY = (static_cast<std::int64_t>(FloorY) % PeriodY + PeriodY) % PeriodY;
		const std::int64_t NextX = (CellX + 1) % PeriodX;
		const std::int64_t NextY = (CellY + 1) % PeriodY;

		const double N00 = LatticeGradient(CellX, CellY, DX, DY, Seed);
		const double N10 = LatticeGradient(NextX, CellY, DX - 1.0, DY, Seed);
		const double N01 = LatticeGradient(CellX, NextY, DX, DY - 1.0, Seed);
		const double N11 = LatticeGradient(NextX, NextY, DX - 1.0, DY - 1.0, Seed);

		const double U = Fade(DX);
		return Mix(Mix(N00, N10, U), Mix(N01, N11, U), Fade(DY)) * 1.41421356;
	}

	double FractalNoise(double X, double Y, std::uint32_t Seed, int Octaves)
	{
		double Sum = 0.0;
		double Amplitude = 1.0;
		double Total = 0.0;
		for (int Octave = 0; Octave < Octaves; ++Octave)
		{
			Sum += GradientNoise(X, Y, Seed + static_cast<std::uint32_t>(Octave) * 131u) * Amplitude;
			Total += Amplitude;
			Amplitude *= 0.5;
			X = X * 2.03 + 17.3;
			Y = Y * 2.03 - 9.1;
		}
		return Sum / Total;
	}

	double TerrainHeight(double X, double Y, std::uint32_t Seed)
	{
		const double Ground = ShapeGround(X, Y, Seed);
		const Lake* Nearby = FindNearbyLake(X, Y, Seed);
		return Nearby != nullptr ? LetLakeIn(*Nearby, X, Y, Ground, Seed) : Ground;
	}

	std::vector<Lake> FindLakes(double X, double Y, double Distance, std::uint32_t Seed)
	{
		std::vector<Lake> Result;
		const auto FirstX = static_cast<std::int64_t>(std::floor((X - Distance) / LakeSpacing)) - 1;
		const auto LastX = static_cast<std::int64_t>(std::floor((X + Distance) / LakeSpacing)) + 1;
		const auto FirstY = static_cast<std::int64_t>(std::floor((Y - Distance) / LakeSpacing)) - 1;
		const auto LastY = static_cast<std::int64_t>(std::floor((Y + Distance) / LakeSpacing)) + 1;
		for (std::int64_t Row = FirstY; Row <= LastY; ++Row)
		{
			for (std::int64_t Column = FirstX; Column <= LastX; ++Column)
			{
				const LakeCell& Cell = GetLakeCell(Column, Row, Seed);
				if (Cell.bLake && std::hypot(Cell.Value.X - X, Cell.Value.Y - Y) < Distance + Cell.Value.GetReach())
				{
					Result.push_back(Cell.Value);
				}
			}
		}
		return Result;
	}

	WaterBody FindWater(double X, double Y, std::uint32_t Seed)
	{
		WaterBody Result;
		const Lake* Nearby = FindNearbyLake(X, Y, Seed);
		if (Nearby != nullptr && Nearby->Holds(X, Y))
		{
			Result.Level = Nearby->Level;
			Result.bLake = true;
		}
		return Result;
	}

	double GetShoreLevel(double X, double Y, std::uint32_t Seed)
	{
		const Lake* Nearby = FindNearbyLake(X, Y, Seed);
		if (Nearby == nullptr)
		{
			return SeaLevel;
		}

		// Falling away from the water, or on flat land a low meadow far from the lake would count as its beach.
		const double Reach = GetLakeReach(*Nearby, X, Y, Seed);
		const double Beach = Nearby->Level - std::max(Reach - 1.0, 0.0) * Nearby->Radius * BeachFall;
		return Mix(Beach, SeaLevel, SmoothStep(BasinHeld, BasinEnd, Reach));
	}

	double WaterSurfaceAt(double X, double Y, double Time, std::uint32_t Seed)
	{
		const WaterBody Water = FindWater(X, Y, Seed);
		return Water.bLake ? Water.Level : WaterHeight(X, Y, Time);
	}

	Vec3 TerrainNormal(double X, double Y, std::uint32_t Seed)
	{
		constexpr double Step = 1.0;
		const double DX = TerrainHeight(X + Step, Y, Seed) - TerrainHeight(X - Step, Y, Seed);
		const double DY = TerrainHeight(X, Y + Step, Seed) - TerrainHeight(X, Y - Step, Seed);
		const Vec3 Raw{ -DX, -DY, 2.0 * Step };
		return Raw * (1.0 / Length(Raw));
	}

	ESurface ClassifySurface(double Height, double NormalZ, double WaterLevel)
	{
		if (NormalZ < 0.72)
		{
			return ESurface::Rock;
		}
		if (Height < WaterLevel + 1.5)
		{
			return ESurface::Sand;
		}
		return Height > 170.0 ? ESurface::Snow : ESurface::Grass;
	}

	Biome ReadBiome(double X, double Y, double Height, std::uint32_t Seed)
	{
		// Cold comes with height, damp with the broad weather of a region and the nearness of water.
		const double Climate = FractalNoise(X / 5200.0 + 7.0, Y / 5200.0 - 3.0, Seed + 40u, 3);
		const double Rain = FractalNoise(X / 3400.0 - 2.0, Y / 3400.0 + 6.0, Seed + 41u, 3);

		Biome Result;
		Result.Warmth = std::clamp(0.62 + 0.45 * Climate - std::max(Height, 0.0) / 260.0, 0.0, 1.0);
		Result.Moisture = std::clamp(0.52 + 1.1 * Rain - std::max(Height - 60.0, 0.0) / 900.0, 0.0, 1.0);

		// Woods come in stands with clearings between, thicker where it is damp, and give out on the cold heights.
		const double Stands = FractalNoise(X / 1500.0, Y / 1500.0, Seed + 42u, 5) + 0.5 * (Result.Moisture - 0.5);
		const double Glades = SmoothStep(-0.16, 0.14, FractalNoise(X / 160.0, Y / 160.0, Seed + 43u, 2));
		Result.Forest = SmoothStep(-0.06, 0.22, Stands) * (0.4 + 0.6 * Glades) * SmoothStep(0.03, 0.2, Result.Warmth);

		// Conifers take the cold and dry ground, broadleaf trees the warm and damp.
		const double Mixture = FractalNoise(X / 410.0 + 11.0, Y / 410.0 - 13.0, Seed + 44u, 2);
		Result.Conifers = std::clamp(1.05 - 1.25 * Result.Warmth + 0.3 * (0.5 - Result.Moisture) + 0.5 * Mixture, 0.0, 1.0);
		return Result;
	}

	double SurfaceFriction(ESurface Surface)
	{
		switch (Surface)
		{
		case ESurface::Sand:  return 0.55;
		case ESurface::Grass: return 0.8;
		case ESurface::Rock:  return 1.0;
		case ESurface::Snow:  return 0.25;
		default:              return 0.8;
		}
	}

	SurfaceGive GetSurfaceGive(ESurface Surface)
	{
		// Measured rather than worked out: PhysX solves these springs implicitly, which at 120 steps a
		// second takes out far more energy than they would lose by themselves, the stiffer the more so.
		// That is why rock is rigid with a bounce of its own instead of a very stiff spring. A ball of
		// 8 kg dropped 3 m comes back 0.44 m on grass, 0.05 m on sand and nothing to speak of in snow.
		switch (Surface)
		{
		case ESurface::Sand:  return { 2000.0, 40.0, 0.0, 0.20 };
		case ESurface::Rock:  return { 0.0, 0.0, 0.45, 0.015 };
		case ESurface::Snow:  return { 1500.0, 48.0, 0.0, 0.14 };
		case ESurface::Grass:
		default:              return { 3000.0, 8.0, 0.0, 0.09 };
		}
	}

	Vec3 FindLandSpawn(std::uint32_t Seed)
	{
		// Best of all beside a lake, with its shore in view.
		const auto IsGood = [Seed](double X, double Y)
		{
			const double Height = TerrainHeight(X, Y, Seed);
			return Height > SeaLevel + 3.0 && Height < 120.0 && TerrainNormal(X, Y, Seed).Z > 0.95;
		};
		std::vector<Lake> Lakes = FindLakes(0.0, 0.0, 12000.0, Seed);
		std::sort(Lakes.begin(), Lakes.end(), [](const Lake& A, const Lake& B) { return std::hypot(A.X, A.Y) < std::hypot(B.X, B.Y); });
		for (const Lake& Item : Lakes)
		{
			for (int Step = 0; Step < 16; ++Step)
			{
				const double Angle = Step * 2.399963;
				const double Reach = Item.Radius * Item.Stretch * 1.62;
				const double X = Item.X + std::cos(Angle) * Reach;
				const double Y = Item.Y + std::sin(Angle) * Reach;
				if (IsGood(X, Y))
				{
					return { X, Y, TerrainHeight(X, Y, Seed) };
				}
			}
		}

		for (int Step = 0; Step < 4096; ++Step)
		{
			const double Angle = Step * 2.399963;
			const double Radius = 64.0 * std::sqrt(static_cast<double>(Step));
			const double X = std::cos(Angle) * Radius;
			const double Y = std::sin(Angle) * Radius;
			if (IsGood(X, Y))
			{
				return { X, Y, TerrainHeight(X, Y, Seed) };
			}
		}
		return { 0.0, 0.0, std::max(TerrainHeight(0.0, 0.0, Seed), SeaLevel) };
	}

	double FindInlandYaw(const Vec3& From, std::uint32_t Seed)
	{
		// With a lake close by, that is the view.
		std::vector<Lake> Lakes = FindLakes(From.X, From.Y, 500.0, Seed);
		if (!Lakes.empty())
		{
			return std::atan2(Lakes.front().Y - From.Y, Lakes.front().X - From.X);
		}

		double BestYaw = 0.0;
		double BestHeight = -1e9;
		for (int Step = 0; Step < 24; ++Step)
		{
			const double Yaw = 2.0 * Pi * Step / 24;
			double Height = 0.0;
			for (const double Distance : { 300.0, 700.0, 1200.0 })
			{
				Height += TerrainHeight(From.X + std::cos(Yaw) * Distance, From.Y + std::sin(Yaw) * Distance, Seed);
			}
			if (Height > BestHeight)
			{
				BestHeight = Height;
				BestYaw = Yaw;
			}
		}
		return BestYaw;
	}

	Vec3 PrevailingWind(double Time, std::uint32_t Seed)
	{
		const WorldConditions Conditions = GetConditions();
		const double Heading = Conditions.WindHeading >= 0.0 ? Conditions.WindHeading : 1.1 + 2.4 * GradientNoise(Time / 900.0, 3.7, Seed + 11u);
		const double Mean = Conditions.WindSpeed * (1.0 + 0.4 * GradientNoise(Time / 140.0, 8.1, Seed + 12u));
		return { std::cos(Heading) * Mean, std::sin(Heading) * Mean, 0.0 };
	}

	Vec3 LargeWindAt(double X, double Y, double Time, std::uint32_t Seed)
	{
		constexpr double SwirlSize = 7000.0;    // metres across the great slow eddies
		constexpr double MeetingSize = 4500.0;  // and between the lines where the air runs together
		constexpr double Swirling = 0.75;       // of the mean wind, how hard they blow
		constexpr double Meeting = 0.45;
		constexpr double Changing = 2400.0;     // seconds for their pattern to change

		const Vec3 Prevailing = PrevailingWind(Time, Seed);
		const double Mean = Length(Prevailing);
		if (Mean < 1e-6)
		{
			return {};
		}

		// Swirls turn around the highs and lows of one smooth field and do not gather the air. The other
		// field's slope drives the air together along lines, and that is where clouds run into each other.
		const double Drift = Time / Changing;
		const auto Slope = [&](double Size, std::uint32_t Salt)
		{
			const double Step = Size / 60.0;
			const double U = X / Size + Drift;
			const double V = Y / Size - Drift * 0.6;
			const double DX = (GradientNoise(U + Step / Size, V, Seed + Salt) - GradientNoise(U - Step / Size, V, Seed + Salt)) / (2.0 * Step);
			const double DY = (GradientNoise(U, V + Step / Size, Seed + Salt) - GradientNoise(U, V - Step / Size, Seed + Salt)) / (2.0 * Step);
			return Vec3{ DX, DY, 0.0 } * Size; // about 1 at the steepest
		};
		const Vec3 Swirl = Slope(SwirlSize, 41u);
		const Vec3 Together = Slope(MeetingSize, 42u);
		Vec3 Large = Prevailing + (Vec3{ Swirl.Y, -Swirl.X, 0.0 } * Swirling + Together * Meeting) * Mean;

		// Never quite still: where the eddies would cancel the wind, it still blows a little their way.
		const double Speed = Length(Large);
		if (Speed < 0.3 * Mean)
		{
			Large = Speed > 1e-6 ? Large * (0.3 * Mean / Speed) : Prevailing * 0.3;
		}
		return Large;
	}

	Vec3 WindAt(const Vec3& Position, double Time, std::uint32_t Seed)
	{
		const Vec3 Large = LargeWindAt(Position.X, Position.Y, Time, Seed);
		const double Mean = Length(Large);
		if (Mean < 1e-6)
		{
			return {};
		}

		const double DirX = Large.X / Mean;
		const double DirY = Large.Y / Mean;

		// Gust cells are carried downwind, so a front visibly crosses the world.
		const double FrontX = (Position.X - DirX * 8.0 * Time) / 380.0;
		const double FrontY = (Position.Y - DirY * 8.0 * Time) / 380.0;
		const double Gust = std::max(0.0, FractalNoise(FrontX, FrontY + Time / 45.0, Seed + 13u, 2)) * Mean * 2.6;

		const double AltitudeBoost = 1.0 + std::clamp(Position.Z / 200.0, 0.0, 1.0);
		const double Speed = (Mean + Gust) * AltitudeBoost;
		return { DirX * Speed, DirY * Speed, 0.0 };
	}

	AirFlow EddiesAt(const Vec3& Position, const Vec3& Drift, double Time, double SmallestEddy)
	{
		static const std::vector<EddyMode> Modes = MakeEddyModes();

		const Vec3 Carried = Position - Drift;
		const double Finest = SmallestEddy > 0.0 ? TwoPi / SmallestEddy : std::numeric_limits<double>::max();

		AirFlow Result;
		for (const EddyMode& Mode : Modes)
		{
			if (Mode.WaveNumber <= Finest)
			{
				const double Phase = Dot(Mode.Wave, Carried) + Mode.Churn * Time + Mode.Offset;
				const double Cosine = std::cos(Phase);
				const double Sine = std::sin(Phase);
				Result.Velocity += Mode.AlongCosine * Cosine + Mode.AlongSine * Sine;
				Result.Vorticity += Mode.SpinOfSine * Cosine - Mode.SpinOfCosine * Sine;
			}
		}
		return Result;
	}

	double TurbulenceIntensity(double HeightAboveGround, double Roughness)
	{
		return 1.0 / std::log(std::max(HeightAboveGround, Roughness * 3.0) / Roughness);
	}

	AirFlow AirFlowAt(const Vec3& Position, const Vec3& Drift, double Time, std::uint32_t Seed)
	{
		constexpr double GrassRoughness = 0.03;
		constexpr double WaterRoughness = 0.0005;
		constexpr double SquashedBelow = 15.0; // the ground stops eddies moving into it

		const double Ground = TerrainHeight(Position.X, Position.Y, Seed);
		const double WaterLevel = FindWater(Position.X, Position.Y, Seed).Level;
		const double Height = std::max(Position.Z - std::max(Ground, WaterLevel), 1.0);
		const double Roughness = Ground > WaterLevel ? GrassRoughness : WaterRoughness;

		const Vec3 Wind = WindAt(Position, Time, Seed);
		const double Strength = Length(Wind) * TurbulenceIntensity(Height, Roughness);

		AirFlow Result = EddiesAt(Position, Drift, Time);
		Result.Velocity.Z *= std::min(Height / SquashedBelow, 1.0);
		Result.Velocity = Wind + Result.Velocity * Strength;
		Result.Vorticity = Result.Vorticity * Strength;
		return Result;
	}

	double WaterHeight(double X, double Y, double Time)
	{
		// Whole cycles per 4096 m, so the pattern repeats exactly and shaders can
		// wrap the camera position to keep single precision far from the origin.
		struct Wave
		{
			double CyclesX;
			double CyclesY;
			double Amplitude;
		};
		constexpr Wave Waves[] = {
			{ 150.0, 110.0, 0.30 }, { -90.0, 210.0, 0.20 }, { 330.0, -240.0, 0.09 },
			{ 61.0, -173.0, 0.14 }, { -437.0, -129.0, 0.05 }, { 241.0, 563.0, 0.035 } };
		constexpr double Scale = TwoPi / 4096.0;

		double Height = 0.0;
		for (const Wave& Item : Waves)
		{
			// Deep-water dispersion: omega^2 = g k.
			const double WaveNumber = Scale * std::hypot(Item.CyclesX, Item.CyclesY);
			const double Phase = Scale * (Item.CyclesX * X + Item.CyclesY * Y) - std::sqrt(Gravity * WaveNumber) * Time;
			Height += Item.Amplitude * std::sin(Phase);
		}
		return SeaLevel + Height * GetConditions().WaveHeight;
	}

	Vec3 AeroForce(const Vec3& RelativeAir, double DragCoefficient, double FrontalArea, double Density)
	{
		return RelativeAir * (0.5 * Density * DragCoefficient * FrontalArea * Length(RelativeAir));
	}

	Vec3 AeroTorque(const Vec3& RelativeSpin, double Radius, double Density)
	{
		constexpr double MomentCoefficient = 1.0;
		return RelativeSpin * (0.5 * Density * MomentCoefficient * std::pow(Radius, 5.0) * Length(RelativeSpin));
	}

	double CloudDepthFor(double WaterMagic)
	{
		constexpr double StormDepth = 1100.0; // metres, at MagicStormWater: tall enough to churn, see CloudUpdraft
		return StormDepth * std::clamp((WaterMagic - MagicCloudWater) / (MagicStormWater - MagicCloudWater), 0.0, 1.3);
	}

	double ShownCloudCover(double Gathered)
	{
		// None where the water magic makes no cloud, scattered ones as it begins to, the sky closing over as it
		// nears MagicRainWater, and overcast in rain. See GetCover in Shaders/Clouds.hlsli.
		return -0.5 + 4.2 * std::clamp(Gathered, 0.0, 1.3);
	}

	double GetOvercast(double Gathered)
	{
		return SmoothStep(0.35, 0.55, Gathered); // rain begins at half a storm's depth of cloud
	}

	Weather GetWeather(double WaterMagic, double Lightning)
	{
		// Only a cloud dense enough to rain is a storm cloud, however much charge has drifted into it.
		Weather Result;
		if (WaterMagic < MagicRainWater)
		{
			return Result;
		}
		Result.Storm = std::clamp((WaterMagic - MagicStormWater) / 10.0 + Lightning / MagicBreakdown * 0.6, 0.0, 1.0);
		Result.Rain = std::max(std::clamp((WaterMagic - MagicRainWater) / 12.0, 0.0, 1.0), Result.Storm);
		return Result;
	}

	MagicMix MagicMix::GetActing() const
	{
		MagicMix Result;
		for (int Type = 0; Type < static_cast<int>(EMagicType::Neutral); ++Type)
		{
			Result.Amounts[Type] = std::max(Amounts[Type] - MagicActsOnMatter, 0.0);
		}
		return Result;
	}

	double MagicManifestDensity(EMagicType Type)
	{
		switch (Type)
		{
		case EMagicType::Light:  return 40.0; // fireflies: not insects, only light showing itself
		case EMagicType::Air:    return 35.0; // a whirlwind
		case EMagicType::Nature: return 38.0; // plants
		case EMagicType::Water:  return 40.0; // a current
		case EMagicType::Earth:  return 50.0; // minerals
		default:                 return MagicFierceFire; // fire, in any of its forms
		}
	}

	double MagicInertness(EMagicType Type)
	{
		switch (Type)
		{
		case EMagicType::Earth:     return 4.0;
		case EMagicType::Water:     return 2.5;
		case EMagicType::Nature:    return 1.8;
		case EMagicType::Fire:      return 1.0;
		case EMagicType::Lightning: return 0.6;
		case EMagicType::Air:       return 0.5;
		case EMagicType::Light:     return 0.25;
		default:                    return 1.0; // neutral
		}
	}

	double MagicFadeTime(EMagicType Type)
	{
		switch (Type)
		{
		case EMagicType::Earth:
		case EMagicType::Water:
			return 420.0; // long enough to lie in a layer some 45 m deep, and to trail away downstream
		case EMagicType::Air:
			return 70.0;
		case EMagicType::Nature:
			return 45.0;
		case EMagicType::Fire:
			return 30.0;
		case EMagicType::Lightning:
			return 10.0;
		case EMagicType::Light:
			return 2.0;
		default:
			return 1e30; // neutral has nothing to lose
		}
	}

	namespace
	{
		std::mutex PlacedGuard;
		std::vector<HotSpot> PlacedFires;
		std::atomic<std::uint32_t> PlacedVersion{ 0 };

		HotSpot MakeCampfire(const Vec3& Position)
		{
			HotSpot Spot;
			Spot.Position = Position;
			Spot.Radius = 0.7;
			Spot.Depth = 0.8;
			Spot.Temperature = 900.0;
			return Spot;
		}

		/** A campfire a little way off where the camera starts, in the way it looks. False if none fits. */
		bool FindSpawnCampfire(std::uint32_t Seed, HotSpot& Out)
		{
			static std::mutex Guard;
			static std::uint32_t KnownSeed = 0;
			static bool bKnown = false;
			static bool bFound = false;
			static HotSpot Known;

			std::lock_guard Lock(Guard);
			if (!bKnown || KnownSeed != Seed)
			{
				bKnown = true;
				KnownSeed = Seed;
				bFound = false;
				const Vec3 Spawn = FindLandSpawn(Seed);
				const double Yaw = FindInlandYaw(Spawn + Vec3{ 0.0, 0.0, 14.0 }, Seed);
				for (double Away = 30.0; Away < 60.0 && !bFound; Away += 2.0) // far enough below the eye to be in view
				{
					for (const double Turn : { 0.0, 0.25, -0.25, 0.5, -0.5 })
					{
						const double X = Spawn.X + std::cos(Yaw + Turn) * Away;
						const double Y = Spawn.Y + std::sin(Yaw + Turn) * Away;
						const double Ground = TerrainHeight(X, Y, Seed);
						if (Ground > GetShoreLevel(X, Y, Seed) + 1.0 && TerrainNormal(X, Y, Seed).Z > 0.95)
						{
							Known = MakeCampfire({ X, Y, Ground });
							bFound = true;
							break;
						}
					}
				}
			}
			Out = Known;
			return bFound;
		}
	}

	void PlaceCampfire(const Vec3& Position)
	{
		std::lock_guard Lock(PlacedGuard);
		PlacedFires.push_back(MakeCampfire(Position));
		++PlacedVersion;
	}

	std::uint32_t GetPlacedVersion()
	{
		return PlacedVersion.load();
	}

	namespace
	{
		std::mutex GrowthGuard;
		std::vector<MagicGrowth> Growths;
	}

	void AddMagicGrowth(const MagicGrowth& Growth)
	{
		std::lock_guard Lock(GrowthGuard);
		Growths.push_back(Growth);
	}

	std::vector<MagicGrowth> FindMagicGrowths(double MinX, double MinY, double MaxX, double MaxY)
	{
		std::lock_guard Lock(GrowthGuard);
		std::vector<MagicGrowth> Found;
		for (const MagicGrowth& Each : Growths)
		{
			if (Each.Position.X >= MinX && Each.Position.X < MaxX && Each.Position.Y >= MinY && Each.Position.Y < MaxY)
			{
				Found.push_back(Each);
			}
		}
		return Found;
	}

	std::vector<HotSpot> FindHotSpots(double X, double Y, std::uint32_t Seed)
	{
		constexpr double Spacing = 700.0;
		constexpr int Reach = 2;

		const auto CellX = static_cast<std::int64_t>(std::floor(X / Spacing));
		const auto CellY = static_cast<std::int64_t>(std::floor(Y / Spacing));
		const auto Share = [Seed](std::int64_t Column, std::int64_t Row, std::uint32_t Salt)
		{
			return HashLattice(Column, Row, Seed + 500u + Salt) / 4294967296.0;
		};

		std::vector<HotSpot> Spots;
		for (std::int64_t Row = CellY - Reach; Row <= CellY + Reach; ++Row)
		{
			for (std::int64_t Column = CellX - Reach; Column <= CellX + Reach; ++Column)
			{
				const double Kind = Share(Column, Row, 0u);
				if (Kind >= 0.55)
				{
					continue;
				}

				const double SpotX = (static_cast<double>(Column) + Share(Column, Row, 1u)) * Spacing;
				const double SpotY = (static_cast<double>(Row) + Share(Column, Row, 2u)) * Spacing;
				const double Ground = TerrainHeight(SpotX, SpotY, Seed);

				// Only on dry, level ground, or it would hang in the air or sit in a lake.
				if (Ground < GetShoreLevel(SpotX, SpotY, Seed) + 2.0 || TerrainNormal(SpotX, SpotY, Seed).Z < 0.96)
				{
					continue;
				}

				HotSpot Spot;
				Spot.bLava = Kind < 0.15;
				Spot.Position = { SpotX, SpotY, Ground };
				Spot.Radius = Spot.bLava ? 6.0 + 9.0 * Share(Column, Row, 3u) : 0.7;
				Spot.Depth = Spot.bLava ? 3.0 : 0.8;
				Spot.Temperature = Spot.bLava ? 1050.0 + 150.0 * Share(Column, Row, 4u) : 900.0;
				Spots.push_back(Spot);
			}
		}

		// And those not of the world's own making, as far off as the others reach.
		const double Covered = Spacing * (Reach + 1);
		const auto IsNear = [X, Y, Covered](const HotSpot& Spot)
		{
			return std::fabs(Spot.Position.X - X) < Covered && std::fabs(Spot.Position.Y - Y) < Covered;
		};
		HotSpot ByTheSpawn;
		if (FindSpawnCampfire(Seed, ByTheSpawn) && IsNear(ByTheSpawn))
		{
			Spots.push_back(ByTheSpawn);
		}
		std::lock_guard Lock(PlacedGuard);
		for (const HotSpot& Spot : PlacedFires)
		{
			if (IsNear(Spot))
			{
				Spots.push_back(Spot);
			}
		}
		return Spots;
	}

	double GetGroundTemperature(double Warmth, ESurface Surface, double SunHeat)
	{
		const double Cool = -5.0 + 35.0 * Warmth;
		switch (Surface)
		{
		case ESurface::Sand:  return Cool + 45.0 * SunHeat;
		case ESurface::Rock:  return Cool + 30.0 * SunHeat;
		case ESurface::Grass: return Cool + 12.0 * SunHeat;
		default:              return Cool;
		}
	}

	double MagicHeatImprint(double Temperature, double VolumeShare)
	{
		constexpr double Contact = 0.35;      // per second, as for any matter, in something at FullHeat
		constexpr double Lowest = 45.0;       // degrees: below this nothing counts as hot
		constexpr double FullHeat = 1000.0;

		// As a root, so that ground warm enough to raise thermals counts for something beside lava.
		return Contact * std::clamp(VolumeShare, 0.0, 1.0) * std::sqrt(std::max(Temperature - Lowest, 0.0) / (FullHeat - Lowest));
	}

	namespace
	{
		/** How far around a hot spot its heat reaches, as the scale of a Gaussian, in metres. */
		double HotSpread(const HotSpot& Spot)
		{
			constexpr double Least = 5.0;
			return Least + 0.5 * Spot.Radius;
		}
	}

	HotDraw MagicHotDraw(const Vec3& Position, const HotSpot& Spot)
	{
		constexpr double Gathered = 18.0; // kg/m3 in a fire at FullHeat: well short of where fire turns to lightning
		constexpr double Taking = 1.0;    // per second there
		constexpr double Lowest = 45.0;   // degrees, as in MagicHeatImprint
		constexpr double FullHeat = 1000.0;

		// How far outside what is hot: across from its rim, and up from its top or down from its foot.
		const double Across = std::max(std::hypot(Position.X - Spot.Position.X, Position.Y - Spot.Position.Y) - Spot.Radius, 0.0);
		const double Up = Position.Z - Spot.Position.Z;
		const double Over = Up > Spot.Depth ? Up - Spot.Depth : (Up < -Spot.Depth ? -Spot.Depth - Up : 0.0);
		const double Spread = HotSpread(Spot);
		const double Near = std::exp(-(Across * Across + Over * Over) / (Spread * Spread));

		const double Heat = std::sqrt(std::max(Spot.Temperature - Lowest, 0.0) / (FullHeat - Lowest));
		return { Gathered * Heat * Near, Taking * Heat * Near };
	}

	double MagicHotReach(const HotSpot& Spot)
	{
		return Spot.Radius + Spot.Depth + 2.5 * HotSpread(Spot); // the draw is under 0.2% of its peak beyond
	}

	void UpdateFireForms(MagicMix& Mix, double Pressure, double Dt)
	{
		constexpr double ChangeTime = 0.5; // seconds
		constexpr EMagicType Forms[] = { EMagicType::Fire, EMagicType::Lightning, EMagicType::Light };

		const EMagicType Held = Pressure >= MagicLightPressure ? EMagicType::Light : (Pressure >= MagicLightningPressure ? EMagicType::Lightning : EMagicType::Fire);
		const double Share = 1.0 - std::exp(-Dt / ChangeTime);
		for (const EMagicType Form : Forms)
		{
			if (Form != Held)
			{
				const double Moved = Mix.Amounts[static_cast<int>(Form)] * Share;
				Mix.Amounts[static_cast<int>(Form)] -= Moved;
				Mix.Amounts[static_cast<int>(Held)] += Moved;
			}
		}
	}

	double MagicRubPressure(double WaterMagic, double AirMagic, double Speed)
	{
		// The squeeze of rubbing counts for as much more than the energy of the motion as the dew point is over
		// the density, 5.5 kg/m3, that was once enough to show. Storms make lightning as they always have.
		constexpr double Squeeze = MagicDewPoint / 5.5;
		const double Both = WaterMagic + AirMagic;
		return Both > 0.0 ? Squeeze * 0.5 * WaterMagic * AirMagic / Both * Speed * Speed : 0.0;
	}

	double MagicRubImprint(double RubPressure)
	{
		constexpr double AtLightning = 0.03; // per second, once it rubs hard enough to make lightning
		return AtLightning * RubPressure / MagicLightningPressure;
	}

	double MagicVapourImprint(double Vapour)
	{
		constexpr double Contact = 0.006; // per second: cloud is mostly air, and touches far less than the sea
		return Contact * Vapour;
	}

	double MagicHeldByVapour(double Vapour)
	{
		constexpr double Held = 1.6;
		return Held * Vapour;
	}

	double CloudUpdraft(double Depth)
	{
		constexpr double Fastest = 42.0;  // m/s, as in the tallest storms there are
		constexpr double Lowest = 900.0;  // metres of cloud: only those piled up to near MagicStormWater, see CloudDepthFor
		constexpr double Tallest = 1100.0; // at MagicStormWater
		return Fastest * std::clamp((Depth - Lowest) / (Tallest - Lowest), 0.0, 1.0);
	}

	void UpdateFireInWind(MagicMix& Mix, double WindSpeed, double Draft, double Dt)
	{
		constexpr double FreshWind = 4.0;  // m/s, at which the air magic in a place counts for just what it is
		constexpr double Burning = 0.6;    // per second, the share of the air magic that a great fire takes
		constexpr double Heating = 3.0;    // kg of neutral magic that each kg of it burnt gives the aspect of fire
		constexpr double Blowing = 0.08;   // per second, the share of a little fire that fresh wind blows away

		const int Fire = static_cast<int>(EMagicType::Fire);
		const int Air = static_cast<int>(EMagicType::Air);
		const int Neutral = static_cast<int>(EMagicType::Neutral);

		// The wind brings air magic, and so does the draft of the fire's own heat drawing it in from around.
		const double Wind = WindSpeed / FreshWind;
		const double Drawn = (WindSpeed + std::max(Draft, 0.0)) / FreshWind;
		const double Brought = Mix.Amounts[Air] * Drawn;
		const double Sum = Mix.Amounts[Fire] + Brought;
		if (Mix.Amounts[Fire] <= 0.0 || Brought <= 0.0)
		{
			return;
		}

		const double Balance = (Mix.Amounts[Fire] - Brought) / Sum; // 1 for all fire, -1 for all wind
		if (Balance > 0.0)
		{
			const double Burnt = Mix.Amounts[Air] * (1.0 - std::exp(-Burning * Balance * std::min(Drawn, 2.0) * Dt));
			Mix.Amounts[Air] -= Burnt;
			Mix.Amounts[Neutral] += Burnt;

			const double Taken = std::min(Burnt * Heating, Mix.Amounts[Neutral]);
			Mix.Amounts[Neutral] -= Taken;
			Mix.Amounts[Fire] += Taken;
		}
		else
		{
			const double Lost = Mix.Amounts[Fire] * (1.0 - std::exp(Blowing * Balance * Wind * Dt));
			Mix.Amounts[Fire] -= Lost;
			Mix.Amounts[Neutral] += Lost;
		}
	}

	bool CanBurn(double X, double Y, std::uint32_t Seed)
	{
		const double Height = TerrainHeight(X, Y, Seed);
		const double Level = GetShoreLevel(X, Y, Seed);
		return Height > Level + 0.5 && ClassifySurface(Height, TerrainNormal(X, Y, Seed).Z, Level) == ESurface::Grass;
	}

	Vec3 FireBlast(const Vec3& FireSlope, double Volume)
	{
		constexpr double SwellPerAmount = 2000.0; // Pa for each kg/m3, far less than air shut in would give
		return FireSlope * (-SwellPerAmount * Volume);
	}

	double BoilWater(MagicMix& Mix, double Dt)
	{
		constexpr double Boiling = 2.0; // per second, the share of the fire that goes on it

		const double Fire = Mix.GetAllFire();
		if (Fire < MagicFierceFire)
		{
			return 0.0;
		}

		const double Left = std::exp(-Boiling * Dt);
		for (const EMagicType Form : { EMagicType::Fire, EMagicType::Lightning, EMagicType::Light })
		{
			Mix.Amounts[static_cast<int>(Form)] *= Left;
		}
		const double Spent = Fire * (1.0 - Left);
		Mix.Amounts[static_cast<int>(EMagicType::Neutral)] += Spent;
		return Spent;
	}

	double FireUpdraft(double FireMagic)
	{
		constexpr double Fastest = 6.0;
		constexpr double PerRoot = 12.0; // hot air rises as the root of how much warmer it is
		return std::min(PerRoot * std::sqrt(std::max(FireMagic, 0.0)), Fastest);
	}

	MagicMix MagicImprint(double HeightAboveSurface, const MagicGround& Ground)
	{
		constexpr double Contact = 0.35;    // per second, inside land or water
		constexpr double Reach = 4.0;       // metres over them that still counts as touching
		constexpr double Growing = 0.04;    // per second, among plants
		constexpr double Canopy = 14.0;     // how high grass and bushes give it
		constexpr double TreeTops = 28.0;   // and a wood, up through its crowns
		constexpr double InWoods = 3.0;     // how much more a thick wood gives than grass

		const double Touching = HeightAboveSurface <= 0.0 ? 1.0 : std::max(1.0 - HeightAboveSurface / Reach, 0.0);

		MagicMix Rates;
		Rates.Amounts[static_cast<int>(EMagicType::Earth)] = Contact * Touching * Ground.Land;
		Rates.Amounts[static_cast<int>(EMagicType::Water)] = Contact * Touching * (1.0 - Ground.Land);
		Rates.Amounts[static_cast<int>(EMagicType::Nature)] = (HeightAboveSurface < Canopy ? Growing * Ground.Plants : 0.0)
			+ (HeightAboveSurface < TreeTops ? Growing * InWoods * Ground.Trees : 0.0);
		Rates.Amounts[static_cast<int>(EMagicType::Air)] = HeightAboveSurface >= 0.0 ? 1.0 / MagicFadeTime(EMagicType::Air) : 0.0;
		return Rates;
	}

	double MagicSlowing(double HeightAboveSurface)
	{
		constexpr double Slowest = 0.1;
		constexpr double Depth = 26.0; // metres over which the ground holds the stream back
		return HeightAboveSurface <= 0.0 ? Slowest : 1.0 - (1.0 - Slowest) * std::exp(-HeightAboveSurface / Depth);
	}

	void UpdateMagicAspects(MagicMix& Mix, const MagicMix& Imprint, double Dt)
	{
		const int Neutral = static_cast<int>(EMagicType::Neutral);

		double Rate = 0.0;
		for (int Type = 0; Type < Neutral; ++Type)
		{
			Rate += Imprint.Amounts[Type];
		}

		// Implicit, which never takes more neutral magic than there is, however long the step.
		const double Taken = Mix.Amounts[Neutral] * Rate * Dt / (1.0 + Rate * Dt);
		double Returned = 0.0;
		for (int Type = 0; Type < Neutral; ++Type)
		{
			const double Lost = Mix.Amounts[Type] * (1.0 - std::exp(-Dt / MagicFadeTime(static_cast<EMagicType>(Type))));
			const double Given = Rate > 0.0 ? Taken * Imprint.Amounts[Type] / Rate : 0.0;
			Mix.Amounts[Type] += Given - Lost;
			Returned += Lost;
		}
		Mix.Amounts[Neutral] += Returned - Taken;
	}

	double UpdateHeldMagic(double Held, double Around, double FireAround, double Dt)
	{
		constexpr double Enough = 0.1;      // kg/m3 of magic around that keeps all of it up: far less than there is anywhere near the ground
		constexpr double Exchanging = 20.0; // seconds for what it holds to come to that
		constexpr double Burning = 0.5;     // per second, of what it holds, in fierce fire

		// Fierce fire burns it out faster than anything around can keep it up.
		if (FireAround >= MagicFierceFire)
		{
			return Held * std::exp(-Burning * Dt);
		}
		const double Kept = MatterHolds * std::clamp(Around / Enough, 0.0, 1.0);
		return Held + (Kept - Held) * (1.0 - std::exp(-Dt / Exchanging));
	}

	Vec3 MagicStirredWind(double AirMagic, const Vec3& Flow)
	{
		constexpr double HalfWay = 5.0; // kg/m3 of air magic that takes the air along at half its own speed
		return Flow * (std::max(AirMagic, 0.0) / (std::max(AirMagic, 0.0) + HalfWay));
	}

	void UpdateNatureFeeding(MagicMix& Mix, double Dt)
	{
		constexpr double TakingLight = 2.0;  // per second, for each kg/m3 of nature magic
		constexpr double TakingWater = 0.05; // the same for water magic, far slower
		constexpr double FromWater = 0.4;    // of the water magic taken up, the share nature grows by

		const double Nature = Mix.Get(EMagicType::Nature);
		if (Nature <= 0.0)
		{
			return;
		}
		const double Light = Mix.Get(EMagicType::Light) * (1.0 - std::exp(-TakingLight * Nature * Dt));
		const double Water = Mix.Get(EMagicType::Water) * (1.0 - std::exp(-TakingWater * Nature * Dt));
		Mix.Amounts[static_cast<int>(EMagicType::Light)] -= Light;
		Mix.Amounts[static_cast<int>(EMagicType::Water)] -= Water;
		Mix.Amounts[static_cast<int>(EMagicType::Nature)] += Light + Water * FromWater;
		Mix.Amounts[static_cast<int>(EMagicType::Neutral)] += Water * (1.0 - FromWater);
	}

	Vec3 MagicDrag(const MagicMix& Mix, const Vec3& Relative, double DragCoefficient, double FrontalArea)
	{
		return Relative * (0.5 * MagicDragShare * Mix.GetAspected() * DragCoefficient * FrontalArea * Length(Relative));
	}

	Vec3 MagicViscosity(double WaterMagic, const Vec3& Relative, double Radius)
	{
		constexpr double ThicknessPerAmount = 0.1; // Pa s for each kg/m3
		return Relative * (6.0 * Pi * ThicknessPerAmount * WaterMagic * Radius);
	}

	Vec3 MagicPressure(const Vec3& AirSlope, double Volume)
	{
		constexpr double Grip = 10.0; // how much harder matter feels this pressure than magic itself does
		return AirSlope * (-Grip * MagicSoundSpeed * MagicSoundSpeed * Volume);
	}

	Vec3 MagicCling(double EarthMagic, double Clearance, const Vec3& GroundNormal, double Mass)
	{
		constexpr double Strongest = 2.5;     // m/s2, in 3 kg/m3 of it, right on the ground
		constexpr double FullAmount = 3.0;
		constexpr double Range = 1.5;         // metres
		return GroundNormal * (-Mass * Strongest * EarthMagic / FullAmount * std::exp(-std::max(Clearance, 0.0) / Range));
	}

	double MagicStiffening(double NatureMagic)
	{
		constexpr double Doubling = 0.6; // kg/m3 that would double it, a little more than the thickest wood holds
		return 1.0 + NatureMagic / Doubling;
	}

	double MagicGrip(double EarthMagic)
	{
		constexpr double Doubling = 3.0; // kg/m3
		return 1.0 + EarthMagic / Doubling;
	}

	std::vector<MagicWell> FindMagicWells(double X, double Y, std::uint32_t Seed)
	{
		constexpr double Spacing = 900.0;
		constexpr int Reach = 2; // far enough for what the distant haze shows

		const auto CellX = static_cast<std::int64_t>(std::floor(X / Spacing));
		const auto CellY = static_cast<std::int64_t>(std::floor(Y / Spacing));
		const auto Share = [Seed](std::int64_t Column, std::int64_t Row, std::uint32_t Salt)
		{
			return HashLattice(Column, Row, Seed + 300u + Salt) / 4294967296.0;
		};

		std::vector<MagicWell> Wells;
		for (std::int64_t Row = CellY - Reach; Row <= CellY + Reach; ++Row)
		{
			for (std::int64_t Column = CellX - Reach; Column <= CellX + Reach; ++Column)
			{
				if (Share(Column, Row, 0u) < 0.8)
				{
					const double WellX = (static_cast<double>(Column) + Share(Column, Row, 1u)) * Spacing;
					const double WellY = (static_cast<double>(Row) + Share(Column, Row, 2u)) * Spacing;
					const double Ground = std::max(TerrainHeight(WellX, WellY, Seed), SeaLevel);

					MagicWell Well;
					Well.Position = { WellX, WellY, Ground + 10.0 + 26.0 * Share(Column, Row, 3u) };
					Well.Radius = 28.0 + 44.0 * Share(Column, Row, 4u);
					Well.Peak = 44.0 + 11.0 * Share(Column, Row, 5u); // over the dew point, so a well shows
					Wells.push_back(Well);
				}
			}
		}
		return Wells;
	}

	double MagicLandShare(double GroundHeight)
	{
		return SmoothStep(SeaLevel - 6.0, SeaLevel + 8.0, GroundHeight);
	}

	MagicGround ReadMagicGround(double X, double Y, std::uint32_t Seed)
	{
		const double Height = TerrainHeight(X, Y, Seed);
		const double WaterLevel = GetShoreLevel(X, Y, Seed);

		MagicGround Ground;
		Ground.Surface = std::max(Height, WaterLevel);
		Ground.bWater = Height < WaterLevel;
		Ground.Land = MagicLandShare(Height - WaterLevel);
		const ESurface Surface = ClassifySurface(Height, TerrainNormal(X, Y, Seed).Z, WaterLevel);
		Ground.Plants = Surface == ESurface::Grass ? 1.0 : 0.0;

		// Water only gets as warm as the air, and the suns are no source of anything by themselves: only what they heat is.
		const Biome Here = ReadBiome(X, Y, Height, Seed);
		Ground.Cool = GetGroundTemperature(Here.Warmth, Surface, 0.0);
		Ground.Sunned = Height > WaterLevel ? GetGroundTemperature(Here.Warmth, Surface, 1.0) - Ground.Cool : 0.0;
		Ground.Trees = Height > WaterLevel ? Here.Forest : 0.0;
		return Ground;
	}

	double MagicLimit(EMagicType Type, double HeightAboveSurface, const MagicGround& Ground)
	{
		constexpr double Layer = 45.0;      // metres over which earth and water magic thin out
		constexpr double Canopy = 14.0;     // and natural magic over grass
		constexpr double Blades = 1.5;      // and what the blades themselves hold, right at the ground
		constexpr double TreeTops = 28.0;   // and in a wood
		constexpr double DenseMagic = 10.0; // the most there is, in the wells

		const double Height = std::max(HeightAboveSurface, 0.0);
		switch (Type)
		{
		case EMagicType::Air:
			return DenseMagic;
		case EMagicType::Nature:
			// Every blade of grass holds nature magic, rule 17, so it is thickest right among them. Grass gives a
			// little more around, and a wood a good deal more, up through its crowns.
			return 0.6 * Ground.Plants * std::exp(-Height / Blades) + 0.14 * Ground.Plants * std::exp(-Height / Canopy) + 0.45 * Ground.Trees * std::exp(-Height / TreeTops) + 0.002;
		case EMagicType::Earth:
			return (0.3 + 2.7 * Ground.Land) * std::exp(-Height / Layer) + 0.05;
		default:
			return (0.3 + 2.7 * (1.0 - Ground.Land)) * std::exp(-Height / Layer) + 0.05;
		}
	}

	MagicMix MagicBaseline(const Vec3& Position, const std::vector<MagicWell>& Wells, const MagicGround& Ground, std::uint32_t Seed)
	{
		// Broad banks of thicker and thinner magic, lying about in no particular order.
		const double Broad = FractalNoise(Position.X / 520.0 + Position.Z / 900.0, Position.Y / 520.0 - Position.Z / 700.0, Seed + 320u, 3);
		const double Bank = SmoothStep(-0.15, 0.7, Broad);
		const double Height = Position.Z - Ground.Surface;

		MagicMix Mix;
		Mix.Amounts[static_cast<int>(EMagicType::Earth)] = MagicLimit(EMagicType::Earth, Height, Ground) * (0.25 + 0.7 * Bank);
		Mix.Amounts[static_cast<int>(EMagicType::Water)] = MagicLimit(EMagicType::Water, Height, Ground) * (0.25 + 0.7 * Bank);
		Mix.Amounts[static_cast<int>(EMagicType::Nature)] = MagicLimit(EMagicType::Nature, Height, Ground) * (0.4 + 0.5 * Bank);

		double Air = 0.03 + 0.25 * Bank;
		for (const MagicWell& Well : Wells)
		{
			const double Reach = LengthSquared(Position - Well.Position) / (Well.Radius * Well.Radius);
			if (Reach < 16.0)
			{
				Air += Well.Peak * std::exp(-Reach);
			}
		}

		// Up in the air half of it has taken on the air's aspect, see MagicImprint.
		Mix.Amounts[static_cast<int>(EMagicType::Air)] = Air * 0.5;
		Mix.Amounts[static_cast<int>(EMagicType::Neutral)] = Air * 0.5;

		// It goes up about 2 km, and thins out to nothing over the last few hundred metres.
		const double Left = 1.0 - SmoothStep(MagicCeiling - 300.0, MagicCeiling + 300.0, Height);
		for (double& Amount : Mix.Amounts)
		{
			Amount *= Left;
		}
		return Mix;
	}

	Vec3 AirLift(double Volume)
	{
		return { 0.0, 0.0, AirDensity * Volume * Gravity };
	}

	double ThrowSpeedAfterHold(double Seconds)
	{
		constexpr double Tap = 0.12;  // anything shorter just drops the ball
		constexpr double Full = 1.5;  // and this long gives all the strength there is
		return MaxThrowSpeed * std::clamp((Seconds - Tap) / (Full - Tap), 0.0, 1.0);
	}

	bool SolveThrowAngle(double Distance, double Rise, double Speed, double DragPerMass, double NetGravity, double& OutAngle)
	{
		constexpr double Step = 1.0 / 120.0; // the rate of the physics, and integrated the same way
		constexpr double MaxTime = 20.0;
		constexpr double MaxAngle = 75.0 * Pi / 180.0;
		constexpr double AngleStep = 1.0 * Pi / 180.0;

		// How far above the target the arc passes. Negative when it falls short.
		const auto Clearance = [=](double Angle)
		{
			double X = 0.0;
			double Z = 0.0;
			double SpeedX = Speed * std::cos(Angle);
			double SpeedZ = Speed * std::sin(Angle);
			for (double Time = 0.0; Time < MaxTime && SpeedX > 1e-3; Time += Step)
			{
				const double Drag = DragPerMass * std::hypot(SpeedX, SpeedZ);
				SpeedX -= Drag * SpeedX * Step;
				SpeedZ -= (Drag * SpeedZ + NetGravity) * Step;

				const double NextX = X + SpeedX * Step;
				const double NextZ = Z + SpeedZ * Step;
				if (NextX >= Distance)
				{
					return Z + (NextZ - Z) * (Distance - X) / (NextX - X) - Rise;
				}
				X = NextX;
				Z = NextZ;
			}
			return -1e9;
		};

		// Straight at the target it always falls short, so raising the aim brackets the arc.
		double Low = std::atan2(Rise, Distance);
		for (double High = Low + AngleStep; High <= MaxAngle; Low = High, High += AngleStep)
		{
			if (Clearance(High) >= 0.0)
			{
				for (int Iteration = 0; Iteration < 24; ++Iteration)
				{
					const double Middle = (Low + High) * 0.5;
					(Clearance(Middle) >= 0.0 ? High : Low) = Middle;
				}
				OutAngle = High;
				return true;
			}
		}
		return false;
	}

	double SubmergedFraction(double CenterZ, double HalfHeight, double WaterZ)
	{
		return std::clamp((WaterZ - (CenterZ - HalfHeight)) / (2.0 * HalfHeight), 0.0, 1.0);
	}

	Vec3 WaterForce(double SubmergedShare, double Volume, const Vec3& Velocity, double DragPerSecond, double Mass)
	{
		const Vec3 Lift{ 0.0, 0.0, WaterDensity * Volume * SubmergedShare * Gravity };
		return Lift - Velocity * (DragPerSecond * Mass * SubmergedShare);
	}
}
