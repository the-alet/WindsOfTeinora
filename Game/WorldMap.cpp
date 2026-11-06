#include "WorldMap.h"

#include "TeinoraSim.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace Teinora
{
	namespace
	{
		struct Rgb
		{
			double R;
			double G;
			double B;
		};

		Rgb Mix(const Rgb& A, const Rgb& B, double Share)
		{
			const double T = std::clamp(Share, 0.0, 1.0);
			return { A.R + (B.R - A.R) * T, A.G + (B.G - A.G) * T, A.B + (B.B - A.B) * T };
		}

		std::uint8_t ToByte(double Value)
		{
			return static_cast<std::uint8_t>(std::clamp(Value, 0.0, 1.0) * 255.0 + 0.5);
		}

		Rgb ColorLand(double Height, double NormalZ, double WaterLevel, const Sim::Biome& Biome)
		{
			switch (Sim::ClassifySurface(Height, NormalZ, WaterLevel))
			{
			case Sim::ESurface::Sand: return { 0.80, 0.72, 0.50 };
			case Sim::ESurface::Rock: return { 0.47, 0.44, 0.41 };
			case Sim::ESurface::Snow: return { 0.93, 0.95, 0.98 };
			default: break;
			}

			const Rgb Dry{ 0.68, 0.64, 0.36 };
			const Rgb Meadow{ 0.45, 0.60, 0.28 };
			const Rgb Wood = Mix(Rgb{ 0.20, 0.42, 0.18 }, Rgb{ 0.13, 0.30, 0.22 }, Biome.Conifers);
			return Mix(Mix(Dry, Meadow, Biome.Moisture * 1.6), Wood, Biome.Forest);
		}
	}

	std::vector<std::uint8_t> DrawWorldMap(int Size, double Span, double CenterX, double CenterY, std::uint32_t Seed)
	{
		std::vector<std::uint8_t> Pixels(static_cast<std::size_t>(Size) * Size * 4);
		const double Cell = Span / Size;

		const auto DrawRows = [&](int First, int Last)
		{
			for (int Row = First; Row < Last; ++Row)
			{
				for (int Column = 0; Column < Size; ++Column)
				{
					const double X = CenterX + (Column + 0.5 - Size * 0.5) * Cell;
					const double Y = CenterY - (Row + 0.5 - Size * 0.5) * Cell;
					const double Height = Sim::TerrainHeight(X, Y, Seed);
					const Sim::WaterBody Water = Sim::FindWater(X, Y, Seed);

					Rgb Color;
					if (Height < Water.Level)
					{
						const double Depth = Water.Level - Height;
						Color = Water.bLake
							? Mix(Rgb{ 0.28, 0.58, 0.68 }, Rgb{ 0.08, 0.26, 0.42 }, Depth / 10.0)
							: Mix(Rgb{ 0.34, 0.66, 0.70 }, Rgb{ 0.05, 0.13, 0.30 }, Depth / 32.0);
					}
					else
					{
						// Light from the north-west, as maps have it.
						const Vec3 Normal = Sim::TerrainNormal(X, Y, Seed);
						const double Shade = 0.62 + 0.75 * std::max(Dot(Normal, Normalize(Vec3{ -0.5, 0.5, 0.7 })), 0.0);
						const Rgb Land = ColorLand(Height, Normal.Z, Water.Level, Sim::ReadBiome(X, Y, Height, Seed));
						Color = { Land.R * Shade, Land.G * Shade, Land.B * Shade };
					}

					std::uint8_t* Pixel = &Pixels[(static_cast<std::size_t>(Row) * Size + Column) * 4];
					Pixel[0] = ToByte(Color.R);
					Pixel[1] = ToByte(Color.G);
					Pixel[2] = ToByte(Color.B);
					Pixel[3] = 255;
				}
			}
		};

		const int Workers = static_cast<int>(std::max(std::thread::hardware_concurrency(), 2u));
		std::vector<std::thread> Threads;
		for (int Worker = 0; Worker < Workers; ++Worker)
		{
			Threads.emplace_back(DrawRows, Size * Worker / Workers, Size * (Worker + 1) / Workers);
		}
		for (std::thread& Thread : Threads)
		{
			Thread.join();
		}
		return Pixels;
	}
}
