#include "SurfaceTextures.h"

#include "TeinoraSim.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <thread>

namespace Teinora
{
	namespace
	{
		constexpr int LayerCount = NeedleLayer + 1;
		constexpr std::uint32_t Seed = 0x51C7A9E3u;

		struct Rgb
		{
			double R;
			double G;
			double B;
		};

		/** What a surface looks like at one texel. Coordinates run from 0 to 1 across the tile. */
		struct Texel
		{
			Rgb Color;
			double Height;
		};

		double Lerp(double A, double B, double T)
		{
			return A + (B - A) * T;
		}

		Rgb Lerp(const Rgb& A, const Rgb& B, double T)
		{
			const double Clamped = std::clamp(T, 0.0, 1.0);
			return { Lerp(A.R, B.R, Clamped), Lerp(A.G, B.G, Clamped), Lerp(A.B, B.B, Clamped) };
		}

		Rgb Scale(const Rgb& Color, double Amount)
		{
			return { Color.R * Amount, Color.G * Amount, Color.B * Amount };
		}

		/** Octaves of noise over Cells lattice cells per tile, each twice as fine and half as strong. */
		double Fractal(double U, double V, int Cells, std::uint32_t Offset, int Octaves)
		{
			double Sum = 0.0;
			double Amplitude = 1.0;
			double Total = 0.0;
			for (int Octave = 0; Octave < Octaves; ++Octave)
			{
				Sum += Sim::TiledNoise(U * Cells, V * Cells, Cells, Cells, Seed + Offset + static_cast<std::uint32_t>(Octave) * 131u) * Amplitude;
				Total += Amplitude;
				Amplitude *= 0.5;
				Cells *= 2;
			}
			return Sum / Total;
		}

		/** The tallest blade over a texel: how high it stands there, and how yellow that blade is. */
		struct BladeTexel
		{
			float Height = 0.0f;
			float Tone = 0.0f;
		};

		/** A tile of grass, drawn stroke by stroke. Blades lean every way, and wrap at the edges. */
		std::vector<BladeTexel> DrawBlades(int Size)
		{
			std::vector<BladeTexel> Field(static_cast<std::size_t>(Size) * Size);
			std::minstd_rand Random(Seed);
			const auto Next = [&Random] { return (Random() - 1.0) / 2147483645.0; };

			const int Blades = Size * Size / 40;
			for (int Blade = 0; Blade < Blades; ++Blade)
			{
				const double StartX = Next() * Size;
				const double StartY = Next() * Size;
				const double Angle = Next() * 6.283185307;
				const double Curl = (Next() - 0.5) * 1.6;
				const double Length = Size * (0.022 + 0.03 * Next());
				const double Width = Size * (0.0016 + 0.0016 * Next());
				const float Tall = static_cast<float>(0.45 + 0.55 * Next());
				const float Tone = static_cast<float>(Next());

				const int Steps = static_cast<int>(Length);
				for (int Step = 0; Step < Steps; ++Step)
				{
					const double Along = static_cast<double>(Step) / Steps;
					const double Heading = Angle + Curl * Along;
					const double X = StartX + std::cos(Heading) * Length * Along;
					const double Y = StartY + std::sin(Heading) * Length * Along;
					const double Radius = Width * (1.0 - Along * 0.8);
					const int Reach = static_cast<int>(Radius) + 1;

					for (int OffsetY = -Reach; OffsetY <= Reach; ++OffsetY)
					{
						for (int OffsetX = -Reach; OffsetX <= Reach; ++OffsetX)
						{
							const int PixelX = static_cast<int>(X) + OffsetX;
							const int PixelY = static_cast<int>(Y) + OffsetY;
							const double Distance = std::hypot(PixelX + 0.5 - X, PixelY + 0.5 - Y) / Radius;
							if (Distance >= 1.0)
							{
								continue;
							}

							// The rounded back of the blade, rising to its tip.
							const float Height = Tall * static_cast<float>((1.0 - Distance * Distance) * (0.5 + 0.5 * Along));
							BladeTexel& Texel = Field[static_cast<std::size_t>((PixelX + Size) % Size) + static_cast<std::size_t>((PixelY + Size) % Size) * Size];
							if (Height > Texel.Height)
							{
								Texel.Height = Height;
								Texel.Tone = Tone;
							}
						}
					}
				}
			}
			return Field;
		}

		Texel MakeGrass(double U, double V, const BladeTexel& Blade)
		{
			const double Clumps = Fractal(U, V, 7, 10u, 5);
			const double Dry = 0.5 + 0.9 * Fractal(U, V, 5, 30u, 3);

			Texel Result;
			Result.Height = Blade.Height;

			const Rgb Soil{ 0.018, 0.024, 0.008 };
			const Rgb Green = Lerp(Rgb{ 0.040, 0.100, 0.018 }, Rgb{ 0.120, 0.235, 0.046 }, Blade.Tone + Clumps);
			Result.Color = Lerp(Soil, Green, 0.25 + 1.2 * Blade.Height);
			Result.Color = Lerp(Result.Color, Rgb{ 0.19, 0.20, 0.07 }, (Dry - 0.6) * 1.1 + (Blade.Tone - 0.8) * 1.3);
			return Result;
		}

		Texel MakeSand(double U, double V)
		{
			const double Warp = Fractal(U, V, 4, 40u, 3);
			const double Ripples = std::sin((V * 14.0 + Warp * 3.0) * 6.283185307);
			const double Dunes = Fractal(U, V, 6, 50u, 4);
			const double Grain = Sim::TiledNoise(U * 190.0, V * 190.0, 190, 190, Seed + 60u);

			Texel Result;
			Result.Height = 0.5 + 0.16 * Ripples + 0.26 * Dunes + 0.08 * Grain;
			Result.Color = Lerp(Rgb{ 0.22, 0.17, 0.10 }, Rgb{ 0.42, 0.35, 0.23 }, Result.Height + Grain * 0.3);
			return Result;
		}

		Texel MakeRock(double U, double V)
		{
			const double Body = Fractal(U, V, 5, 70u, 6);
			const double Warp = Fractal(U, V, 3, 80u, 3);
			const double Strata = std::sin((V * 9.0 + Warp * 2.2) * 6.283185307);

			// Thin dark lines where the noise crosses zero read as cracks.
			const double Crack = 1.0 - std::min(std::abs(Fractal(U, V, 6, 90u, 4)) * 14.0, 1.0);

			Texel Result;
			Result.Height = std::clamp(0.52 + 0.42 * Body + 0.12 * Strata - 0.32 * Crack, 0.0, 1.0);
			Result.Color = Scale(Lerp(Rgb{ 0.09, 0.085, 0.08 }, Rgb{ 0.34, 0.32, 0.29 }, Result.Height), 1.0 - 0.55 * Crack);
			return Result;
		}

		Texel MakeSnow(double U, double V)
		{
			const double Drifts = Fractal(U, V, 4, 100u, 4);
			const double Grain = Sim::TiledNoise(U * 150.0, V * 150.0, 150, 150, Seed + 110u);

			Texel Result;
			Result.Height = 0.5 + 0.34 * Drifts + 0.04 * Grain;
			Result.Color = Lerp(Rgb{ 0.70, 0.77, 0.88 }, Rgb{ 0.95, 0.96, 0.98 }, Result.Height + 0.2);
			return Result;
		}

		Texel MakeBark(double U, double V)
		{
			// Ridges that run along V, the length of the trunk, and wander and split as they go.
			const double Warp = Fractal(U, V, 4, 120u, 4);
			const double Plates = std::abs(std::sin((U * 7.0 + Warp * 1.3) * 3.141592654));
			const double Scales = Sim::TiledNoise(U * 36.0, V * 7.0, 36, 7, Seed + 130u);
			const double Grain = Fractal(U, V, 32, 140u, 3);

			Texel Result;
			Result.Height = std::clamp(0.2 + 0.62 * std::sqrt(Plates) + 0.2 * Scales + 0.1 * Grain, 0.0, 1.0);
			Result.Color = Lerp(Rgb{ 0.06, 0.055, 0.05 }, Rgb{ 0.33, 0.31, 0.28 }, Result.Height);
			return Result;
		}

		/** A leaf or needle over a texel of a cut-out card. */
		struct CutTexel
		{
			float R = 0.0f;
			float G = 0.0f;
			float B = 0.0f;
			float Height = -1.0f; // below zero where there is nothing, and the card is cut away
		};

		void Plot(std::vector<CutTexel>& Field, int Size, int X, int Y, double Height, const Rgb& Color)
		{
			if (X >= 0 && Y >= 0 && X < Size && Y < Size)
			{
				CutTexel& Target = Field[static_cast<std::size_t>(X) + static_cast<std::size_t>(Y) * Size];
				if (Height > Target.Height)
				{
					Target = { static_cast<float>(Color.R), static_cast<float>(Color.G), static_cast<float>(Color.B), static_cast<float>(Height) };
				}
			}
		}

		/** A short round-backed stroke between two points. */
		void DrawStroke(std::vector<CutTexel>& Field, int Size, double X0, double Y0, double X1, double Y1, double Width, double Top, const Rgb& Color)
		{
			const int Reach = static_cast<int>(Width) + 1;
			const int Steps = std::max(static_cast<int>(std::hypot(X1 - X0, Y1 - Y0)), 1);
			for (int Step = 0; Step <= Steps; ++Step)
			{
				const double X = X0 + (X1 - X0) * Step / Steps;
				const double Y = Y0 + (Y1 - Y0) * Step / Steps;
				for (int OffsetY = -Reach; OffsetY <= Reach; ++OffsetY)
				{
					for (int OffsetX = -Reach; OffsetX <= Reach; ++OffsetX)
					{
						const double Distance = std::hypot(OffsetX, OffsetY) / Width;
						if (Distance < 1.0)
						{
							Plot(Field, Size, static_cast<int>(X) + OffsetX, static_cast<int>(Y) + OffsetY, Top * (1.0 - Distance * Distance * 0.6), Color);
						}
					}
				}
			}
		}

		/** A spray of broad leaves, scattered clear of the edges so that a card never ends in a straight line. */
		std::vector<CutTexel> DrawLeaves(int Size)
		{
			std::vector<CutTexel> Field(static_cast<std::size_t>(Size) * Size);
			std::minstd_rand Random(Seed + 7u);
			const auto Next = [&Random] { return (Random() - 1.0) / 2147483645.0; };

			constexpr int Leaves = 170;
			for (int Leaf = 0; Leaf < Leaves; ++Leaf)
			{
				// More towards the middle, thinning out to a loose edge.
				const double Spread = std::sqrt(Next()) * 0.46;
				const double Around = Next() * 6.283185307;
				const double CenterX = (0.5 + std::cos(Around) * Spread) * Size;
				const double CenterY = (0.5 + std::sin(Around) * Spread) * Size;
				const double Angle = Next() * 6.283185307;
				const double HalfLength = Size * (0.04 + 0.03 * Next());
				const double HalfWidth = HalfLength * (0.42 + 0.14 * Next());
				const double Tone = Next();
				const double Layer = Next();

				const Rgb Body = Lerp(Rgb{ 0.035, 0.095, 0.02 }, Rgb{ 0.12, 0.25, 0.05 }, Tone);
				const int Reach = static_cast<int>(HalfLength) + 2;
				for (int OffsetY = -Reach; OffsetY <= Reach; ++OffsetY)
				{
					for (int OffsetX = -Reach; OffsetX <= Reach; ++OffsetX)
					{
						const double Along = (OffsetX * std::cos(Angle) + OffsetY * std::sin(Angle)) / HalfLength;
						const double Across = (OffsetY * std::cos(Angle) - OffsetX * std::sin(Angle)) / HalfWidth;
						if (std::abs(Along) >= 1.0)
						{
							continue;
						}

						// Wide near the stalk, drawn to a point at the tip.
						const double Edge = std::pow(1.0 - Along * Along, 0.7) * (1.0 - 0.3 * Along);
						if (std::abs(Across) >= Edge)
						{
							continue;
						}

						const double Rib = std::abs(Across) < 0.06 ? 0.72 : 1.0;
						const double Veins = 1.0 - 0.14 * std::pow(std::abs(std::sin((Along * 3.2 + std::abs(Across) * 1.4) * 3.141592654)), 12.0);
						const double Dome = 1.0 - (Across / Edge) * (Across / Edge);
						Plot(Field, Size, static_cast<int>(CenterX) + OffsetX, static_cast<int>(CenterY) + OffsetY, 0.1 + 0.55 * Layer + 0.35 * Dome, Scale(Body, Rib * Veins * (0.8 + 0.3 * Dome)));
					}
				}
			}
			return Field;
		}

		/** A bough seen from above: a stem along U, with twigs either side of it, all thick with needles. */
		std::vector<CutTexel> DrawNeedles(int Size)
		{
			std::vector<CutTexel> Field(static_cast<std::size_t>(Size) * Size);
			std::minstd_rand Random(Seed + 8u);
			const auto Next = [&Random] { return (Random() - 1.0) / 2147483645.0; };

			const double Stroke = std::max(Size / 512.0, 1.2);
			const Rgb Wood{ 0.10, 0.07, 0.045 };
			const auto AddNeedles = [&](double X0, double Y0, double X1, double Y1, double Reach)
			{
				const double Way = std::atan2(Y1 - Y0, X1 - X0);
				const double Span = std::hypot(X1 - X0, Y1 - Y0);
				const int Count = static_cast<int>(Span / (Stroke * 2.4));
				for (int Needle = 0; Needle < Count; ++Needle)
				{
					const double Along = (Needle + Next()) / Count;
					const double X = X0 + (X1 - X0) * Along;
					const double Y = Y0 + (Y1 - Y0) * Along;
					const double Tone = Next();
					const Rgb Color = Lerp(Rgb{ 0.02, 0.065, 0.04 }, Rgb{ 0.06, 0.16, 0.085 }, Tone);
					for (const double Side : { -1.0, 1.0 })
					{
						const double Angle = Way + Side * (0.55 + 0.35 * Next());
						const double Length = Reach * (0.75 + 0.4 * Next()) * (1.0 - 0.45 * Along);
						DrawStroke(Field, Size, X, Y, X + std::cos(Angle) * Length, Y + std::sin(Angle) * Length, Stroke, 0.5 + 0.4 * Tone, Color);
					}
				}
			};

			const double Middle = Size * 0.5;
			const double Long = Size * 0.94;
			constexpr int Twigs = 26;
			for (int Twig = 0; Twig < Twigs; ++Twig)
			{
				const double Along = (Twig + 0.5) / Twigs;
				const double X = Size * 0.03 + Long * Along;
				const double Side = Twig % 2 == 0 ? 1.0 : -1.0;
				const double Angle = Side * (0.95 - 0.45 * Along);
				const double Length = Size * 0.42 * (1.0 - 0.8 * Along) + Size * 0.05;
				const double EndX = X + std::cos(Angle) * Length;
				const double EndY = Middle + std::sin(Angle) * Length;
				DrawStroke(Field, Size, X, Middle, EndX, EndY, Stroke * 1.2, 0.45, Wood);
				AddNeedles(X, Middle, EndX, EndY, Size * 0.042);
			}
			DrawStroke(Field, Size, Size * 0.01, Middle, Size * 0.98, Middle, Stroke * 2.4, 0.95, Wood);
			AddNeedles(Size * 0.03, Middle, Size * 0.98, Middle, Size * 0.03);
			return Field;
		}

		double GetBumpiness(int Layer)
		{
			switch (Layer)
			{
			case static_cast<int>(Sim::ESurface::Sand):  return 3.0;
			case static_cast<int>(Sim::ESurface::Grass): return 1.6;
			case static_cast<int>(Sim::ESurface::Rock):  return 11.0;
			case BarkLayer:                              return 9.0;
			case LeafLayer:                              return 2.5;
			case NeedleLayer:                            return 2.5;
			default:                                     return 2.0;
			}
		}

		std::uint8_t ToByte(double Value)
		{
			return static_cast<std::uint8_t>(std::clamp(Value, 0.0, 1.0) * 255.0 + 0.5);
		}

		std::uint32_t HashCell(int X, int Y, int Z, std::uint32_t Salt)
		{
			std::uint32_t Value = Seed ^ Salt;
			for (const int Part : { X, Y, Z })
			{
				Value = (Value ^ static_cast<std::uint32_t>(Part)) * 0x01000193u;
				Value ^= Value >> 15;
			}
			Value *= 0x5BD1E995u;
			return Value ^ (Value >> 13);
		}

		double HashShare(std::uint32_t Value, int Shift)
		{
			return ((Value >> Shift) & 1023u) / 1023.0;
		}

		/** Near 1 at scattered points and falling away between them, which is what billows look like. Tiles. */
		double TiledBillows(double X, double Y, double Z, int Cells, std::uint32_t Salt)
		{
			const double GridX = X * Cells;
			const double GridY = Y * Cells;
			const double GridZ = Z * Cells;
			const int CellX = static_cast<int>(std::floor(GridX));
			const int CellY = static_cast<int>(std::floor(GridY));
			const int CellZ = static_cast<int>(std::floor(GridZ));

			double Nearest = 3.0;
			for (int OffsetZ = -1; OffsetZ <= 1; ++OffsetZ)
			{
				for (int OffsetY = -1; OffsetY <= 1; ++OffsetY)
				{
					for (int OffsetX = -1; OffsetX <= 1; ++OffsetX)
					{
						const int NearX = CellX + OffsetX;
						const int NearY = CellY + OffsetY;
						const int NearZ = CellZ + OffsetZ;
						const std::uint32_t Value = HashCell((NearX + Cells) % Cells, (NearY + Cells) % Cells, (NearZ + Cells) % Cells, Salt);
						const double DeltaX = NearX + HashShare(Value, 0) - GridX;
						const double DeltaY = NearY + HashShare(Value, 10) - GridY;
						const double DeltaZ = NearZ + HashShare(Value, 20) - GridZ;
						Nearest = std::min(Nearest, DeltaX * DeltaX + DeltaY * DeltaY + DeltaZ * DeltaZ);
					}
				}
			}
			return 1.0 - std::min(std::sqrt(Nearest), 1.0);
		}

		/** Three sizes of billows, each twice as fine and half as strong. */
		double LayeredBillows(double X, double Y, double Z, int Cells, std::uint32_t Salt)
		{
			return TiledBillows(X, Y, Z, Cells, Salt) * 0.625 + TiledBillows(X, Y, Z, Cells * 2, Salt + 1u) * 0.25 + TiledBillows(X, Y, Z, Cells * 4, Salt + 2u) * 0.125;
		}

		/** The largest mip of one layer, as RGBA8 rows. */
		void DrawLayer(int Layer, int Size, std::vector<std::uint8_t>& OutColor, std::vector<std::uint8_t>& OutNormal)
		{
			const std::size_t Count = static_cast<std::size_t>(Size) * Size;
			std::vector<double> Heights(Count);
			OutColor.resize(Count * 4);
			OutNormal.resize(Count * 4);

			const std::vector<BladeTexel> Blades = Layer == static_cast<int>(Sim::ESurface::Grass) ? DrawBlades(Size) : std::vector<BladeTexel>();
			const std::vector<CutTexel> Cuts = Layer == LeafLayer ? DrawLeaves(Size) : (Layer == NeedleLayer ? DrawNeedles(Size) : std::vector<CutTexel>());

			// What a cut-out card shows at its holes bleeds into the edges of leaves once it is
			// filtered, so the holes take on the leaves' own colour.
			Rgb Fill{ 0.0, 0.0, 0.0 };
			double Filled = 0.0;
			for (const CutTexel& Cut : Cuts)
			{
				if (Cut.Height >= 0.0f)
				{
					Fill.R += Cut.R;
					Fill.G += Cut.G;
					Fill.B += Cut.B;
					Filled += 1.0;
				}
			}
			Fill = Scale(Fill, 1.0 / std::max(Filled, 1.0));

			for (int Y = 0; Y < Size; ++Y)
			{
				for (int X = 0; X < Size; ++X)
				{
					const std::size_t Index = static_cast<std::size_t>(X) + static_cast<std::size_t>(Y) * Size;
					const double U = (X + 0.5) / Size;
					const double V = (Y + 0.5) / Size;

					Texel Value;
					double Alpha = 1.0;
					if (!Cuts.empty())
					{
						const CutTexel& Cut = Cuts[Index];
						const bool bThere = Cut.Height >= 0.0f;
						Value.Color = bThere ? Rgb{ Cut.R, Cut.G, Cut.B } : Fill;
						Value.Height = bThere ? Cut.Height : 0.0;
						Alpha = bThere ? 1.0 : 0.0;
					}
					else
					{
						switch (Layer)
						{
						case static_cast<int>(Sim::ESurface::Sand):  Value = MakeSand(U, V); break;
						case static_cast<int>(Sim::ESurface::Grass): Value = MakeGrass(U, V, Blades[Index]); break;
						case static_cast<int>(Sim::ESurface::Rock):  Value = MakeRock(U, V); break;
						case BarkLayer:                              Value = MakeBark(U, V); break;
						default:                                     Value = MakeSnow(U, V); break;
						}
					}

					Heights[Index] = Value.Height;
					OutColor[Index * 4 + 0] = ToByte(Value.Color.R);
					OutColor[Index * 4 + 1] = ToByte(Value.Color.G);
					OutColor[Index * 4 + 2] = ToByte(Value.Color.B);
					OutColor[Index * 4 + 3] = ToByte(Alpha);
				}
			}

			const double Bumpiness = GetBumpiness(Layer);
			const auto HeightAt = [&Heights, Size](int X, int Y)
			{
				return Heights[static_cast<std::size_t>((X + Size) % Size) + static_cast<std::size_t>((Y + Size) % Size) * Size];
			};

			for (int Y = 0; Y < Size; ++Y)
			{
				for (int X = 0; X < Size; ++X)
				{
					const double SlopeX = (HeightAt(X + 1, Y) - HeightAt(X - 1, Y)) * Bumpiness;
					const double SlopeY = (HeightAt(X, Y + 1) - HeightAt(X, Y - 1)) * Bumpiness;
					const double Length = std::sqrt(SlopeX * SlopeX + SlopeY * SlopeY + 1.0);

					const std::size_t Index = (static_cast<std::size_t>(X) + static_cast<std::size_t>(Y) * Size) * 4;
					OutNormal[Index + 0] = ToByte(-SlopeX / Length * 0.5 + 0.5);
					OutNormal[Index + 1] = ToByte(-SlopeY / Length * 0.5 + 0.5);
					OutNormal[Index + 2] = ToByte(1.0 / Length * 0.5 + 0.5);
					OutNormal[Index + 3] = ToByte(HeightAt(X, Y));
				}
			}
		}

		/** Averages four texels into one. Normals are renormalised, so a mip keeps its slopes unit length. */
		std::vector<std::uint8_t> Shrink(const std::vector<std::uint8_t>& Source, int Size, bool bNormals)
		{
			const int Half = std::max(Size / 2, 1);
			std::vector<std::uint8_t> Result(static_cast<std::size_t>(Half) * Half * 4);
			for (int Y = 0; Y < Half; ++Y)
			{
				for (int X = 0; X < Half; ++X)
				{
					double Sum[4] = {};
					for (int Corner = 0; Corner < 4; ++Corner)
					{
						const int SourceX = (X * 2 + (Corner & 1)) % Size;
						const int SourceY = (Y * 2 + (Corner >> 1)) % Size;
						const std::size_t Index = (static_cast<std::size_t>(SourceX) + static_cast<std::size_t>(SourceY) * Size) * 4;
						for (int Channel = 0; Channel < 4; ++Channel)
						{
							Sum[Channel] += Source[Index + Channel] / 255.0 * 0.25;
						}
					}

					if (bNormals)
					{
						const double NX = Sum[0] * 2.0 - 1.0;
						const double NY = Sum[1] * 2.0 - 1.0;
						const double NZ = Sum[2] * 2.0 - 1.0;
						const double Length = std::max(std::sqrt(NX * NX + NY * NY + NZ * NZ), 1e-6);
						Sum[0] = NX / Length * 0.5 + 0.5;
						Sum[1] = NY / Length * 0.5 + 0.5;
						Sum[2] = NZ / Length * 0.5 + 0.5;
					}

					const std::size_t Target = (static_cast<std::size_t>(X) + static_cast<std::size_t>(Y) * Half) * 4;
					for (int Channel = 0; Channel < 4; ++Channel)
					{
						Result[Target + Channel] = ToByte(Sum[Channel]);
					}
				}
			}
			return Result;
		}

		void AppendMips(std::vector<std::uint8_t> Level, int Size, bool bNormals, std::vector<std::uint8_t>& Out)
		{
			while (true)
			{
				Out.insert(Out.end(), Level.begin(), Level.end());
				if (Size == 1)
				{
					return;
				}
				Level = Shrink(Level, Size, bNormals);
				Size /= 2;
			}
		}
	}

	std::vector<std::uint8_t> MakeCloudNoise(int Size)
	{
		constexpr int WorkerCount = 4;

		std::vector<std::uint8_t> Pixels(static_cast<std::size_t>(Size) * Size * Size * 4);
		std::vector<std::thread> Workers;
		for (int Worker = 0; Worker < WorkerCount; ++Worker)
		{
			Workers.emplace_back([Worker, Size, &Pixels]
			{
				for (int Z = Size * Worker / WorkerCount; Z < Size * (Worker + 1) / WorkerCount; ++Z)
				{
					std::uint8_t* Row = Pixels.data() + static_cast<std::size_t>(Z) * Size * Size * 4;
					for (int Y = 0; Y < Size; ++Y)
					{
						for (int X = 0; X < Size; ++X)
						{
							const double U = (X + 0.5) / Size;
							const double V = (Y + 0.5) / Size;
							const double W = (Z + 0.5) / Size;
							*Row++ = ToByte(LayeredBillows(U, V, W, 3, 200u) * 1.15);
							*Row++ = ToByte(LayeredBillows(U, V, W, 4, 210u) * 1.25);
							*Row++ = 0;
							*Row++ = 255;
						}
					}
				}
			});
		}
		for (std::thread& Worker : Workers)
		{
			Worker.join();
		}
		return Pixels;
	}

	SurfaceTextures MakeSurfaceTextures(int Size)
	{
		SurfaceTextures Result;
		Result.Size = Size;
		Result.Layers = LayerCount;
		for (int Remaining = Size; Remaining > 0; Remaining /= 2)
		{
			++Result.Mips;
		}

		std::vector<std::uint8_t> LayerColors[LayerCount];
		std::vector<std::uint8_t> LayerNormals[LayerCount];
		std::vector<std::thread> Workers;
		for (int Layer = 0; Layer < LayerCount; ++Layer)
		{
			Workers.emplace_back([Layer, Size, &LayerColors, &LayerNormals]
			{
				DrawLayer(Layer, Size, LayerColors[Layer], LayerNormals[Layer]);
			});
		}
		for (std::thread& Worker : Workers)
		{
			Worker.join();
		}

		for (int Layer = 0; Layer < LayerCount; ++Layer)
		{
			AppendMips(std::move(LayerColors[Layer]), Size, false, Result.Colors);
			AppendMips(std::move(LayerNormals[Layer]), Size, true, Result.Normals);
		}
		return Result;
	}
}
