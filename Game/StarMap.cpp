#include "StarMap.h"

#include <algorithm>
#include <cmath>

namespace Teinora
{
	namespace
	{
		constexpr double TwoPi = Pi * 2.0;

		// The real sky holds about 10^(0.5 m + 0.7) stars brighter than magnitude m.
		constexpr double CountSlope = 0.5;
		constexpr double CountBase = 0.7;

		constexpr int ClusterCount = 46;
		constexpr double ClusterShare = 0.14; // of all stars, most of them the fainter ones

		/** The same numbers on every compiler, which the standard's distributions do not promise. */
		struct Random
		{
			std::uint64_t State;

			double Next()
			{
				State += 0x9E3779B97F4A7C15ull;
				std::uint64_t Value = State;
				Value = (Value ^ (Value >> 30)) * 0xBF58476D1CE4E5B9ull;
				Value = (Value ^ (Value >> 27)) * 0x94D049BB133111EBull;
				Value ^= Value >> 31;
				return static_cast<double>(Value >> 11) / 9007199254740992.0;
			}

			double NextNormal()
			{
				const double Radius = std::sqrt(-2.0 * std::log(1.0 - Next()));
				return Radius * std::cos(TwoPi * Next());
			}
		};

		Vec3 PickDirection(Random& Dice)
		{
			const double Z = 2.0 * Dice.Next() - 1.0;
			const double Around = TwoPi * Dice.Next();
			const double Ring = std::sqrt(std::max(1.0 - Z * Z, 0.0));
			return { Ring * std::cos(Around), Ring * std::sin(Around), Z };
		}

		struct SpectralClass
		{
			double Share;
			Float3 Tint;
		};

		// As seen by eye: hot blue-white stars, a lot of white, yellow and orange giants, few red ones.
		constexpr SpectralClass Classes[] = {
			{ 0.10, { 0.62f, 0.72f, 1.00f } },
			{ 0.22, { 0.80f, 0.86f, 1.00f } },
			{ 0.19, { 0.98f, 0.97f, 1.00f } },
			{ 0.14, { 1.00f, 0.93f, 0.82f } },
			{ 0.31, { 1.00f, 0.78f, 0.55f } },
			{ 0.04, { 1.00f, 0.60f, 0.36f } } };

		Float3 PickTint(Random& Dice)
		{
			double Left = Dice.Next();
			for (const SpectralClass& Class : Classes)
			{
				Left -= Class.Share;
				if (Left <= 0.0)
				{
					return Class.Tint;
				}
			}
			return Classes[0].Tint;
		}

		std::uint8_t ToByte(double Value)
		{
			return static_cast<std::uint8_t>(std::clamp(Value, 0.0, 1.0) * 255.0 + 0.5);
		}
	}

	double GetStarMagnitude(double Flux)
	{
		return FaintestMagnitude - 2.5 * std::log10(Flux);
	}

	std::vector<SkyStar> MakeStarMap(std::uint32_t Seed)
	{
		Random Dice{ 0x5354415253ull ^ (static_cast<std::uint64_t>(Seed) << 20) };

		struct Cluster
		{
			Vec3 Center;
			double Spread;
			double Share;
		};
		Cluster Clusters[ClusterCount];
		double Shares = 0.0;
		for (Cluster& Item : Clusters)
		{
			Item.Center = PickDirection(Dice);
			Item.Spread = 0.006 + 0.03 * Dice.Next() * Dice.Next(); // about a third of a degree to two
			Item.Share = 0.2 + Dice.Next() * Dice.Next();
			Shares += Item.Share;
		}

		std::vector<SkyStar> Stars(StarMapCount);
		const double Total = std::pow(10.0, CountSlope * FaintestMagnitude + CountBase);
		const double Rescale = Total / StarMapCount;
		for (int Index = 0; Index < StarMapCount; ++Index)
		{
			SkyStar& Star = Stars[static_cast<std::size_t>(Index)];

			// The magnitude that this star's rank among the brightest calls for.
			const double Rank = (Index + 0.2 + 0.6 * Dice.Next()) * Rescale;
			const double Magnitude = (std::log10(Rank) - CountBase) / CountSlope;
			const double Flux = std::pow(10.0, 0.4 * (FaintestMagnitude - Magnitude));
			Star.Flux = static_cast<float>(Flux);

			Vec3 Direction = PickDirection(Dice);
			const double Faintness = static_cast<double>(Index) / StarMapCount;
			if (Dice.Next() < ClusterShare * Faintness * 2.0)
			{
				double Pick = Dice.Next() * Shares;
				const Cluster* Home = &Clusters[0];
				for (const Cluster& Item : Clusters)
				{
					Pick -= Item.Share;
					Home = &Item;
					if (Pick <= 0.0)
					{
						break;
					}
				}
				const Vec3 Scatter{ Dice.NextNormal(), Dice.NextNormal(), Dice.NextNormal() };
				Direction = Normalize(Home->Center + Scatter * Home->Spread);
			}
			Star.Direction = { static_cast<float>(Direction.X), static_cast<float>(Direction.Y), static_cast<float>(Direction.Z) };

			// The eye only sees colour in the brighter stars.
			const Float3 Tint = PickTint(Dice);
			const double Colourful = 0.2 + 0.8 * std::clamp(std::log2(Flux) / 7.0, 0.0, 1.0);
			Star.Color[0] = ToByte(1.0 + (Tint.X - 1.0) * Colourful);
			Star.Color[1] = ToByte(1.0 + (Tint.Y - 1.0) * Colourful);
			Star.Color[2] = ToByte(1.0 + (Tint.Z - 1.0) * Colourful);
			Star.Color[3] = ToByte(Dice.Next());
		}
		return Stars;
	}

	std::vector<std::uint8_t> DrawStarChart(const std::vector<SkyStar>& Stars, int Height)
	{
		const int Width = Height * 2;
		std::vector<float> Light(static_cast<std::size_t>(Width) * Height * 3, 0.0f);

		const auto Add = [&Light, Width, Height](int X, int Y, float R, float G, float B)
		{
			if (X < 0 || X >= Width || Y < 0 || Y >= Height)
			{
				return;
			}
			float* Pixel = &Light[(static_cast<std::size_t>(Y) * Width + X) * 3];
			Pixel[0] += R;
			Pixel[1] += G;
			Pixel[2] += B;
		};

		// Two discs, one for each half of the sphere around the poles it turns on. Distance from
		// the middle of a disc is the angle from its pole, so that nothing is stretched much.
		const double Radius = Height * 0.48;
		const auto AddGridPoint = [&Add, Height, Radius](int Disc, double Reach, double Around)
		{
			const double X = Height * (Disc + 0.5) + Reach * Radius * std::cos(Around);
			const double Y = Height * 0.5 + Reach * Radius * std::sin(Around);
			Add(static_cast<int>(X), static_cast<int>(Y), 0.06f, 0.09f, 0.14f);
		};
		for (int Disc = 0; Disc < 2; ++Disc)
		{
			for (int Ring = 1; Ring <= 3; ++Ring)
			{
				for (int Step = 0; Step < Height * 4; ++Step)
				{
					AddGridPoint(Disc, Ring / 3.0, TwoPi * Step / (Height * 4));
				}
			}
			for (int Spoke = 0; Spoke < 12; ++Spoke)
			{
				for (int Step = 0; Step < Height; ++Step)
				{
					AddGridPoint(Disc, static_cast<double>(Step) / Height, TwoPi * Spoke / 12.0);
				}
			}
		}

		const Vec3 Pole = GetSkyPole();
		const Vec3 Along = GetSkyAlong();
		const Vec3 Side = Cross(Pole, Along);
		for (const SkyStar& Star : Stars)
		{
			const Vec3 Direction{ Star.Direction.X, Star.Direction.Y, Star.Direction.Z };
			const double Height2 = std::clamp(Dot(Direction, Pole), -1.0, 1.0);
			const int Disc = Height2 >= 0.0 ? 0 : 1;
			const double FromPole = std::acos(std::fabs(Height2)) / (Pi * 0.5);
			const double Around = std::atan2(Dot(Direction, Side), Dot(Direction, Along));
			const double Flip = Disc == 0 ? 1.0 : -1.0; // the far half is seen from inside the sphere as well
			const double CenterX = Height * (Disc + 0.5) + Flip * FromPole * Radius * std::cos(Around);
			const double CenterY = Height * 0.5 + FromPole * Radius * std::sin(Around);

			// Brighter stars as bigger dots, the way charts mark them.
			const double Rise = FaintestMagnitude - GetStarMagnitude(Star.Flux);
			const double Size = (0.42 + 0.028 * Rise * Rise) * Height / 1024.0;
			const double Strength = 0.5 + 0.2 * Rise;
			const int Reach = static_cast<int>(std::ceil(Size * 4.0));
			for (int Y = -Reach; Y <= Reach; ++Y)
			{
				for (int X = -Reach; X <= Reach; ++X)
				{
					const int PixelX = static_cast<int>(CenterX) + X;
					const int PixelY = static_cast<int>(CenterY) + Y;
					const double OffX = (PixelX + 0.5 - CenterX) / Size;
					const double OffY = (PixelY + 0.5 - CenterY) / Size;
					const float Spot = static_cast<float>(std::exp(-0.5 * (OffX * OffX + OffY * OffY)) * Strength);
					Add(PixelX, PixelY, Spot * Star.Color[0] / 255.0f, Spot * Star.Color[1] / 255.0f, Spot * Star.Color[2] / 255.0f);
				}
			}
		}

		std::vector<std::uint8_t> Pixels(static_cast<std::size_t>(Width) * Height * 4);
		for (std::size_t Index = 0; Index < static_cast<std::size_t>(Width) * Height; ++Index)
		{
			for (std::size_t Channel = 0; Channel < 3; ++Channel)
			{
				const float Value = Light[Index * 3 + Channel];
				Pixels[Index * 4 + Channel] = ToByte(std::sqrt(static_cast<double>(1.0f - std::exp(-Value * 1.6f))));
			}
			Pixels[Index * 4 + 3] = 255;
		}
		return Pixels;
	}

	void FoldDirection(const Vec3& Direction, double& U, double& V)
	{
		const double Sum = std::fabs(Direction.X) + std::fabs(Direction.Y) + std::fabs(Direction.Z);
		double X = Direction.X / Sum;
		double Y = Direction.Y / Sum;
		if (Direction.Z < 0.0)
		{
			const double FoldedX = (1.0 - std::fabs(Y)) * (X >= 0.0 ? 1.0 : -1.0);
			Y = (1.0 - std::fabs(X)) * (Y >= 0.0 ? 1.0 : -1.0);
			X = FoldedX;
		}
		U = X * 0.5 + 0.5;
		V = Y * 0.5 + 0.5;
	}

	Vec3 UnfoldDirection(double U, double V)
	{
		double X = U * 2.0 - 1.0;
		double Y = V * 2.0 - 1.0;
		const double Z = 1.0 - std::fabs(X) - std::fabs(Y);
		if (Z < 0.0)
		{
			const double FoldedX = (1.0 - std::fabs(Y)) * (X >= 0.0 ? 1.0 : -1.0);
			Y = (1.0 - std::fabs(X)) * (Y >= 0.0 ? 1.0 : -1.0);
			X = FoldedX;
		}
		return Normalize(Vec3{ X, Y, Z });
	}

	StarSheet MakeStarSheet(const std::vector<SkyStar>& Stars)
	{
		StarSheet Sheet;
		constexpr std::size_t BytesPerStar = 8;
		constexpr std::size_t RowBytes = static_cast<std::size_t>(StarCellSide) * StarsPerCell * BytesPerStar;
		Sheet.Pixels.assign(RowBytes * StarCellSide, 0);
		for (const SkyStar& Star : Stars) // brightest first, so what is taken stays taken
		{
			double U = 0.0;
			double V = 0.0;
			FoldDirection({ Star.Direction.X, Star.Direction.Y, Star.Direction.Z }, U, V);

			const int X = std::min(static_cast<int>(U * StarCellSide), StarCellSide - 1);
			const int Y = std::min(static_cast<int>(V * StarCellSide), StarCellSide - 1);
			std::uint8_t* Cell = &Sheet.Pixels[Y * RowBytes + X * StarsPerCell * BytesPerStar];
			const std::uint8_t* const End = Cell + StarsPerCell * BytesPerStar;
			while (Cell != End && Cell[2] != 0)
			{
				Cell += BytesPerStar;
			}
			if (Cell == End)
			{
				continue;
			}

			Cell[0] = ToByte(U * StarCellSide - X);
			Cell[1] = ToByte(V * StarCellSide - Y);
			Cell[2] = static_cast<std::uint8_t>(1.0 + 254.0 * std::clamp(std::log2(static_cast<double>(Star.Flux)) / StarFluxStops, 0.0, 1.0) + 0.5);
			Cell[4] = Star.Color[0];
			Cell[5] = Star.Color[1];
			Cell[6] = Star.Color[2];
			Cell[7] = Star.Color[3];
			++Sheet.Placed;
		}
		return Sheet;
	}
}
