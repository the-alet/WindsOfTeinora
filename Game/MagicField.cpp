#include "MagicField.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <latch>

namespace Teinora
{
	namespace
	{
		constexpr float SoundSpeed = 5.0f;     // m/s, how fast a pressure difference spreads
		constexpr float DragTime = 1.0f;       // seconds for magic to take up the motion driving it
		constexpr float RestoreTime = 5.0f;    // seconds for the world to restore the baseline
		constexpr float DrainTime = 0.4f;      // seconds for magic over its limit to go
		constexpr float Confinement = 1.4f;    // per second, how strongly swirls are kept up
		constexpr float Diffusivity = 3.0f;    // m2/s, how fast magic soaks into its neighbours
		constexpr float ThinDensity = 0.02f;   // below this, pressure forces no longer grow
		constexpr float MaxCourant = 0.45f;    // cells per step along an axis, which transport needs below 0.5

		constexpr double WindShare = 0.3;      // how much of the air's motion magic takes up
		constexpr double CurrentSpeed = 1.2;   // m/s, of the weak currents of magic's own

		constexpr int CoarseStep = 4;          // cells between the samples of what drives the gas
		constexpr double FinestEddy = 32.0;    // metres: eddies smaller than this fall between those samples
		constexpr double GroundRoughness = 0.03;
		constexpr int CoarseX = MagicField::GridX / CoarseStep + 2;
		constexpr int CoarseY = MagicField::GridY / CoarseStep + 2;
		constexpr int CoarseZ = MagicField::GridZ / CoarseStep + 2;

		constexpr int CellCount = MagicField::GridX * MagicField::GridY * MagicField::GridZ;
		constexpr int ColumnCount = MagicField::GridX * MagicField::GridY;
		constexpr int LongestLine = std::max({ MagicField::GridX, MagicField::GridY, MagicField::GridZ });

		constexpr double SimulationStep = 0.05; // seconds per step, on the simulation's thread
		constexpr int WarmUpSteps = 160;        // run before the first result, so that the gas is already moving

		constexpr int FarX = 64;
		constexpr int FarY = 64;
		constexpr int FarZ = 12;
		constexpr double FarCellSize = 64.0;

		int Wrap(int Value, int Size)
		{
			const int Rest = Value % Size;
			return Rest < 0 ? Rest + Size : Rest;
		}

		int FloorToInt(double Value)
		{
			return static_cast<int>(std::floor(Value));
		}

		float MinMod(float A, float B)
		{
			if (A * B <= 0.0f)
			{
				return 0.0f;
			}
			return std::fabs(A) < std::fabs(B) ? A : B;
		}

		std::uint8_t ToByte(double Value)
		{
			return static_cast<std::uint8_t>(std::clamp(Value, 0.0, 1.0) * 255.0 + 0.5);
		}

		/** The colours of the kinds of magic, in the order of Sim::EMagicType, linear. */
		constexpr double MagicColors[Sim::MagicTypeCount][3] = {
			{ 0.34, 0.17, 0.06 },   // brown
			{ 0.06, 0.28, 0.80 },   // light blue going on deep blue
			{ 0.17, 0.85, 0.46 },   // green turquoise
			{ 0.05, 0.30, 0.09 } }; // dark green

		/** Writes one texel of a volume: the total density, then the colour of what is there. */
		void WritePixel(std::uint8_t* Pixel, const double* Amounts, double Spin)
		{
			double Total = 0.0;
			double Color[3] = {};
			for (int Type = 0; Type < Sim::MagicTypeCount; ++Type)
			{
				Total += Amounts[Type];
				for (int Channel = 0; Channel < 3; ++Channel)
				{
					Color[Channel] += MagicColors[Type][Channel] * Amounts[Type];
				}
			}

			const double Scale = Total > 1e-6 ? (0.75 + 0.35 * std::min(Spin * 3.0, 1.0)) / Total : 0.0;
			Pixel[0] = ToByte(std::sqrt(Total / MagicVolume::DensityScale));
			Pixel[1] = ToByte(Color[0] * Scale);
			Pixel[2] = ToByte(Color[1] * Scale);
			Pixel[3] = ToByte(Color[2] * Scale);
		}

		/**
		 * Moves a line of densities along a line of velocities, as second-order finite volumes:
		 * what leaves one cell through a face enters the next, so the amount is conserved, and
		 * the limited slopes keep it positive. Diffusion is one more flow through the same faces,
		 * down the difference in density. Lines hold two extra cells at each end.
		 */
		void TransportLine(float* Line, const float* Speed, const std::uint8_t* Blocked, int Count, float Courant, float Soaking)
		{
			float Flux[LongestLine + 1];
			for (int Face = 0; Face <= Count; ++Face)
			{
				const int Left = Face + 1;
				const int Right = Face + 2;
				if (Blocked[Left] != 0 || Blocked[Right] != 0)
				{
					Flux[Face] = 0.0f;
					continue;
				}

				const float Moved = (Speed[Left] + Speed[Right]) * 0.5f * Courant;
				float Value;
				if (Moved >= 0.0f)
				{
					const float Slope = MinMod(Line[Left] - Line[Left - 1], Line[Left + 1] - Line[Left]);
					Value = Line[Left] + 0.5f * (1.0f - Moved) * Slope;
				}
				else
				{
					const float Slope = MinMod(Line[Right] - Line[Right - 1], Line[Right + 1] - Line[Right]);
					Value = Line[Right] - 0.5f * (1.0f + Moved) * Slope;
				}
				Flux[Face] = Moved * Value - Soaking * (Line[Right] - Line[Left]);
			}

			for (int Cell = 0; Cell < Count; ++Cell)
			{
				Line[Cell + 2] -= Flux[Cell + 1] - Flux[Cell];
			}
		}
	}

	bool MagicVolume::Contains(const Vec3& Position) const
	{
		const double X = (Position.X - Corner.X) / CellSize;
		const double Y = (Position.Y - Corner.Y) / CellSize;
		const double Z = (Position.Z - Corner.Z) / CellSize;
		return X >= 0.0 && Y >= 0.0 && Z >= 0.0 && X < SizeX && Y < SizeY && Z < SizeZ;
	}

	Sim::MagicMix MagicVolume::GetMix(const Vec3& Position) const
	{
		Sim::MagicMix Result;
		if (Amounts[0].empty())
		{
			return Result;
		}

		// Density values sit at the centres of cells.
		const double X = std::clamp((Position.X - Corner.X) / CellSize - 0.5, 0.0, SizeX - 1.001);
		const double Y = std::clamp((Position.Y - Corner.Y) / CellSize - 0.5, 0.0, SizeY - 1.001);
		const double Z = std::clamp((Position.Z - Corner.Z) / CellSize - 0.5, 0.0, SizeZ - 1.001);
		const int CellX = static_cast<int>(X);
		const int CellY = static_cast<int>(Y);
		const int CellZ = static_cast<int>(Z);
		const double PartX = X - CellX;
		const double PartY = Y - CellY;
		const double PartZ = Z - CellZ;

		for (int Type = 0; Type < Sim::MagicTypeCount; ++Type)
		{
			const std::vector<float>& Field = Amounts[Type];
			const auto At = [&](int Column, int Row, int Slice)
			{
				return static_cast<double>(Field[static_cast<std::size_t>(Column + SizeX * (Row + SizeY * Slice))]);
			};
			const auto Along = [&](int Row, int Slice)
			{
				return At(CellX, Row, Slice) * (1.0 - PartX) + At(CellX + 1, Row, Slice) * PartX;
			};
			const auto Across = [&](int Slice)
			{
				return Along(CellY, Slice) * (1.0 - PartY) + Along(CellY + 1, Slice) * PartY;
			};
			Result.Amounts[Type] = Across(CellZ) * (1.0 - PartZ) + Across(CellZ + 1) * PartZ;
		}
		return Result;
	}

	MagicField::MagicField(std::uint32_t InSeed, const Options& InOptions)
		: Seed(InSeed)
		, Settings(InOptions)
		, Density(CellCount, 0.0f)
		, VelocityX(CellCount, 0.0f)
		, VelocityY(CellCount, 0.0f)
		, VelocityZ(CellCount, 0.0f)
		, NextX(CellCount, 0.0f)
		, NextY(CellCount, 0.0f)
		, NextZ(CellCount, 0.0f)
		, CurlX(CellCount, 0.0f)
		, CurlY(CellCount, 0.0f)
		, CurlZ(CellCount, 0.0f)
		, Spin(CellCount, 0.0f)
		, Solid(CellCount, 0)
		, Columns(ColumnCount)
		, AirX(CoarseX * CoarseY * CoarseZ, 0.0f)
		, AirY(CoarseX * CoarseY * CoarseZ, 0.0f)
		, AirZ(CoarseX * CoarseY * CoarseZ, 0.0f)
	{
		for (int Type = 0; Type < Sim::MagicTypeCount; ++Type)
		{
			Amounts[Type].assign(CellCount, 0.0f);
			Baselines[Type].assign(CellCount, 0.0f);
			Limits[Type].assign(CellCount, 0.0f);
		}
		if (Settings.Threads > 1)
		{
			Pool = std::make_unique<ThreadPool>(Settings.Threads);
		}
		OriginX = -GridX / 2;
		OriginY = -GridY / 2;
		OriginZ = -GridZ / 2;
		RefreshMaps();
	}

	void MagicField::ParallelFor(int Count, const std::function<void(int)>& Body)
	{
		if (Pool == nullptr)
		{
			for (int Index = 0; Index < Count; ++Index)
			{
				Body(Index);
			}
			return;
		}

		std::atomic<int> Next = 0;
		std::latch Finished(static_cast<std::ptrdiff_t>(Settings.Threads));
		for (unsigned Worker = 0; Worker < Settings.Threads; ++Worker)
		{
			Pool->Enqueue([&]
			{
				for (int Index = Next.fetch_add(1); Index < Count; Index = Next.fetch_add(1))
				{
					Body(Index);
				}
				Finished.count_down();
			});
		}
		Finished.wait();
	}

	void MagicField::RefreshMaps()
	{
		// Beyond the window a neighbour is the edge cell itself, so nothing pushes on the edges.
		for (int Index = -2; Index < GridX + 2; ++Index)
		{
			MapX[Index + 2] = Wrap(OriginX + std::clamp(Index, 0, GridX - 1), GridX);
		}
		for (int Index = -2; Index < GridY + 2; ++Index)
		{
			MapY[Index + 2] = Wrap(OriginY + std::clamp(Index, 0, GridY - 1), GridY) * GridX;
		}
		for (int Index = -2; Index < GridZ + 2; ++Index)
		{
			MapZ[Index + 2] = Wrap(OriginZ + std::clamp(Index, 0, GridZ - 1), GridZ) * ColumnCount;
		}
	}

	Vec3 MagicField::GetCellCenter(int X, int Y, int Z) const
	{
		return { (OriginX + X + 0.5) * CellSize, (OriginY + Y + 0.5) * CellSize, (OriginZ + Z + 0.5) * CellSize };
	}

	int MagicField::FindCell(const Vec3& Position) const
	{
		return CellIndex(
			std::clamp(FloorToInt(Position.X / CellSize) - OriginX, 0, GridX - 1),
			std::clamp(FloorToInt(Position.Y / CellSize) - OriginY, 0, GridY - 1),
			std::clamp(FloorToInt(Position.Z / CellSize) - OriginZ, 0, GridZ - 1));
	}

	void MagicField::ReadColumn(int WorldX, int WorldY)
	{
		Columns[static_cast<std::size_t>(Wrap(WorldX, GridX) + Wrap(WorldY, GridY) * GridX)] = Sim::ReadMagicGround((WorldX + 0.5) * CellSize, (WorldY + 0.5) * CellSize, Seed);
	}

	void MagicField::FillCell(int WorldX, int WorldY, int WorldZ)
	{
		const int Column = Wrap(WorldX, GridX) + Wrap(WorldY, GridY) * GridX;
		const std::size_t Cell = static_cast<std::size_t>(Column) + static_cast<std::size_t>(Wrap(WorldZ, GridZ)) * ColumnCount;
		const double Height = (WorldZ + 0.5) * CellSize;
		const Sim::MagicGround& Below = Columns[static_cast<std::size_t>(Column)];
		const bool bSolid = Height < Below.Surface;

		const Vec3 Center{ (WorldX + 0.5) * CellSize, (WorldY + 0.5) * CellSize, Height };
		const Sim::MagicMix Mix = bSolid ? Sim::MagicMix{} : Sim::MagicBaseline(Center, Wells, Below, Seed);
		for (int Type = 0; Type < Sim::MagicTypeCount; ++Type)
		{
			Amounts[Type][Cell] = static_cast<float>(Mix.Amounts[Type]);
			Baselines[Type][Cell] = static_cast<float>(Mix.Amounts[Type]);
			Limits[Type][Cell] = static_cast<float>(Sim::MagicLimit(static_cast<Sim::EMagicType>(Type), Height - Below.Surface, Below));
		}
		Solid[Cell] = bSolid ? 1 : 0;
		VelocityX[Cell] = 0.0f;
		VelocityY[Cell] = 0.0f;
		VelocityZ[Cell] = 0.0f;
	}

	void MagicField::MoveWindow(const Vec3& Focus)
	{
		constexpr double WellSpacing = 450.0; // reread well before any leaves the reach of the search

		const int NewX = FloorToInt(Focus.X / CellSize) - GridX / 2;
		const int NewY = FloorToInt(Focus.Y / CellSize) - GridY / 2;
		const int NewZ = FloorToInt(Focus.Z / CellSize) - GridZ / 2;

		const int WellX = FloorToInt(Focus.X / WellSpacing);
		const int WellY = FloorToInt(Focus.Y / WellSpacing);
		if (!bPlaced || WellX != WellCellX || WellY != WellCellY)
		{
			Wells = Sim::FindMagicWells(Focus.X, Focus.Y, Seed);
			WellCellX = WellX;
			WellCellY = WellY;
		}

		if (!bPlaced)
		{
			bPlaced = true;
			OriginX = NewX;
			OriginY = NewY;
			OriginZ = NewZ;
			RefreshMaps();

			ParallelFor(GridY, [this](int Row)
			{
				for (int Column = 0; Column < GridX; ++Column)
				{
					ReadColumn(OriginX + Column, OriginY + Row);
				}
			});
			ParallelFor(GridZ, [this](int Slice)
			{
				for (int Row = 0; Row < GridY; ++Row)
				{
					for (int Column = 0; Column < GridX; ++Column)
					{
						FillCell(OriginX + Column, OriginY + Row, OriginZ + Slice);
					}
				}
			});
			return;
		}

		// The world cells a window takes on as it moves from Old to New along one axis.
		const auto Entering = [](int Old, int New, int Size, int& OutFirst, int& OutLast)
		{
			OutFirst = New > Old ? std::max(Old + Size, New) : New;
			OutLast = New > Old ? New + Size : std::min(Old, New + Size);
		};

		int First = 0;
		int Last = 0;
		Entering(OriginX, NewX, GridX, First, Last);
		for (int X = First; X < Last; ++X)
		{
			for (int Y = OriginY; Y < OriginY + GridY; ++Y)
			{
				ReadColumn(X, Y);
				for (int Z = OriginZ; Z < OriginZ + GridZ; ++Z)
				{
					FillCell(X, Y, Z);
				}
			}
		}
		OriginX = NewX;

		Entering(OriginY, NewY, GridY, First, Last);
		for (int Y = First; Y < Last; ++Y)
		{
			for (int X = OriginX; X < OriginX + GridX; ++X)
			{
				ReadColumn(X, Y);
				for (int Z = OriginZ; Z < OriginZ + GridZ; ++Z)
				{
					FillCell(X, Y, Z);
				}
			}
		}
		OriginY = NewY;

		Entering(OriginZ, NewZ, GridZ, First, Last);
		for (int Z = First; Z < Last; ++Z)
		{
			for (int Y = OriginY; Y < OriginY + GridY; ++Y)
			{
				for (int X = OriginX; X < OriginX + GridX; ++X)
				{
					FillCell(X, Y, Z);
				}
			}
		}
		OriginZ = NewZ;
		RefreshMaps();
	}

	void MagicField::ComputeAir(const Vec3& AirDrift, double Time)
	{
		// Far enough along that the currents share nothing with the eddies of the air.
		const Vec3 CurrentOffset{ 7300.0, -4100.0, 900.0 };

		ParallelFor(CoarseZ, [&](int Slice)
		{
			for (int Row = 0; Row < CoarseY; ++Row)
			{
				for (int Column = 0; Column < CoarseX; ++Column)
				{
					const int CellX = std::min(Column * CoarseStep, GridX - 1);
					const int CellY = std::min(Row * CoarseStep, GridY - 1);
					const int CellZ = std::min(Slice * CoarseStep, GridZ - 1);
					const Vec3 Center = GetCellCenter(CellX, CellY, CellZ);

					const double Floor = Columns[static_cast<std::size_t>(MapX[CellX + 2] + MapY[CellY + 2])].Surface;
					const double Height = std::max(Center.Z - Floor, 1.0);

					const Vec3 Wind = Sim::WindAt(Center, Time, Seed);
					const double Strength = Length(Wind) * Sim::TurbulenceIntensity(Height, GroundRoughness);
					const Vec3 Air = Wind + Sim::EddiesAt(Center, AirDrift, Time, FinestEddy).Velocity * Strength;

					// Magic only half minds the wind, and has slow currents of its own.
					const Vec3 Current = Sim::EddiesAt(Center + CurrentOffset, {}, Time, FinestEddy).Velocity * CurrentSpeed;
					const Vec3 Moving = Air * WindShare + Current;

					const std::size_t Index = static_cast<std::size_t>(Column + CoarseX * (Row + CoarseY * Slice));
					AirX[Index] = static_cast<float>(Moving.X);
					AirY[Index] = static_cast<float>(Moving.Y);
					AirZ[Index] = static_cast<float>(Moving.Z);
				}
			}
		});
	}

	void MagicField::ComputeSpin()
	{
		const float Half = static_cast<float>(0.5 / CellSize);
		ParallelFor(GridZ, [&](int Z)
		{
			for (int Y = 0; Y < GridY; ++Y)
			{
				for (int X = 0; X < GridX; ++X)
				{
					const int Here = CellIndex(X, Y, Z);
					const int Left = CellIndex(X - 1, Y, Z);
					const int Right = CellIndex(X + 1, Y, Z);
					const int Back = CellIndex(X, Y - 1, Z);
					const int Front = CellIndex(X, Y + 1, Z);
					const int Below = CellIndex(X, Y, Z - 1);
					const int Above = CellIndex(X, Y, Z + 1);

					const float CX = ((VelocityZ[Front] - VelocityZ[Back]) - (VelocityY[Above] - VelocityY[Below])) * Half;
					const float CY = ((VelocityX[Above] - VelocityX[Below]) - (VelocityZ[Right] - VelocityZ[Left])) * Half;
					const float CZ = ((VelocityY[Right] - VelocityY[Left]) - (VelocityX[Front] - VelocityX[Back])) * Half;
					CurlX[Here] = CX;
					CurlY[Here] = CY;
					CurlZ[Here] = CZ;
					Spin[Here] = std::sqrt(CX * CX + CY * CY + CZ * CZ);
				}
			}
		});
	}

	void MagicField::AddUpDensity()
	{
		ParallelFor(GridZ, [&](int Slice)
		{
			const int First = Slice * ColumnCount;
			for (int Cell = First; Cell < First + ColumnCount; ++Cell)
			{
				float Total = 0.0f;
				for (const std::vector<float>& Kind : Amounts)
				{
					Total += Kind[Cell];
				}
				Density[Cell] = Total;
			}
		});
	}

	void MagicField::UpdateVelocity(float Dt)
	{
		const float Width = static_cast<float>(CellSize);
		const float Half = 0.5f / Width;
		const float Cells = Dt / Width;
		const float Pressure = SoundSpeed * SoundSpeed; // pressure per unit of density
		const float MaxSpeed = MaxCourant * Width / Dt;
		const float Volume = Width * Width * Width;
		const float Blend = 1.0f - std::exp(-Dt / (DragTime * static_cast<float>(Sim::MagicInertia)));

		ParallelFor(GridZ, [&](int Z)
		{
			for (int Y = 0; Y < GridY; ++Y)
			{
				for (int X = 0; X < GridX; ++X)
				{
					const int Here = CellIndex(X, Y, Z);
					if (Solid[Here] != 0)
					{
						NextX[Here] = 0.0f;
						NextY[Here] = 0.0f;
						NextZ[Here] = 0.0f;
						continue;
					}

					// What flowed here a moment ago brings its motion with it.
					const float FromX = std::clamp(static_cast<float>(X) - VelocityX[Here] * Cells, 0.0f, GridX - 1.001f);
					const float FromY = std::clamp(static_cast<float>(Y) - VelocityY[Here] * Cells, 0.0f, GridY - 1.001f);
					const float FromZ = std::clamp(static_cast<float>(Z) - VelocityZ[Here] * Cells, 0.0f, GridZ - 1.001f);
					const int BaseX = static_cast<int>(FromX);
					const int BaseY = static_cast<int>(FromY);
					const int BaseZ = static_cast<int>(FromZ);
					const float PartX = FromX - static_cast<float>(BaseX);
					const float PartY = FromY - static_cast<float>(BaseY);
					const float PartZ = FromZ - static_cast<float>(BaseZ);

					const int Corners[8] = {
						CellIndex(BaseX, BaseY, BaseZ), CellIndex(BaseX + 1, BaseY, BaseZ),
						CellIndex(BaseX, BaseY + 1, BaseZ), CellIndex(BaseX + 1, BaseY + 1, BaseZ),
						CellIndex(BaseX, BaseY, BaseZ + 1), CellIndex(BaseX + 1, BaseY, BaseZ + 1),
						CellIndex(BaseX, BaseY + 1, BaseZ + 1), CellIndex(BaseX + 1, BaseY + 1, BaseZ + 1) };
					const auto Blended = [&](const std::vector<float>& Field)
					{
						const float Y0 = Field[Corners[0]] + (Field[Corners[1]] - Field[Corners[0]]) * PartX;
						const float Y1 = Field[Corners[2]] + (Field[Corners[3]] - Field[Corners[2]]) * PartX;
						const float Y2 = Field[Corners[4]] + (Field[Corners[5]] - Field[Corners[4]]) * PartX;
						const float Y3 = Field[Corners[6]] + (Field[Corners[7]] - Field[Corners[6]]) * PartX;
						const float Z0 = Y0 + (Y1 - Y0) * PartY;
						const float Z1 = Y2 + (Y3 - Y2) * PartY;
						return Z0 + (Z1 - Z0) * PartZ;
					};
					float NewX = Blended(VelocityX);
					float NewY = Blended(VelocityY);
					float NewZ = Blended(VelocityZ);

					// Pressure pushes from dense towards thin: force is the pressure difference
					// over the cell's faces, and what it moves is the amount of magic in the cell.
					// Walls push back as hard as the cell pushes on them, so they cancel out.
					const int Left = CellIndex(X - 1, Y, Z);
					const int Right = CellIndex(X + 1, Y, Z);
					const int Back = CellIndex(X, Y - 1, Z);
					const int Front = CellIndex(X, Y + 1, Z);
					const int Below = CellIndex(X, Y, Z - 1);
					const int Above = CellIndex(X, Y, Z + 1);

					const float Own = Density[Here];
					const auto Beside = [&](int Cell) { return Solid[Cell] != 0 ? Own : Density[Cell]; };
					const float Amount = std::max(Own, ThinDensity) * Volume * static_cast<float>(Sim::MagicInertia);
					const float ForceScale = -Pressure * Half * Volume / Amount * Dt;
					NewX += (Beside(Right) - Beside(Left)) * ForceScale;
					NewY += (Beside(Front) - Beside(Back)) * ForceScale;
					NewZ += (Beside(Above) - Beside(Below)) * ForceScale;

					// Vorticity confinement: a push around each swirl, against the numerical
					// blurring of the grid, directed from the weak side of the swirl to the strong.
					const float RiseX = (Spin[Right] - Spin[Left]) * Half;
					const float RiseY = (Spin[Front] - Spin[Back]) * Half;
					const float RiseZ = (Spin[Above] - Spin[Below]) * Half;
					const float Rise = std::sqrt(RiseX * RiseX + RiseY * RiseY + RiseZ * RiseZ);
					if (Rise > 1e-5f)
					{
						const float Push = Confinement * Width * Dt / Rise;
						NewX += (RiseY * CurlZ[Here] - RiseZ * CurlY[Here]) * Push;
						NewY += (RiseZ * CurlX[Here] - RiseX * CurlZ[Here]) * Push;
						NewZ += (RiseX * CurlY[Here] - RiseY * CurlX[Here]) * Push;
					}

					// What drives magic drags it along in proportion to the amount there is to
					// drag, so where that leads does not depend on the amount.
					if (Settings.bOpen)
					{
						const float SpotX = static_cast<float>(X) / CoarseStep;
						const float SpotY = static_cast<float>(Y) / CoarseStep;
						const float SpotZ = static_cast<float>(Z) / CoarseStep;
						const int CellX = static_cast<int>(SpotX);
						const int CellY = static_cast<int>(SpotY);
						const int CellZ = static_cast<int>(SpotZ);
						const float ShareX = SpotX - static_cast<float>(CellX);
						const float ShareY = SpotY - static_cast<float>(CellY);
						const float ShareZ = SpotZ - static_cast<float>(CellZ);
						const int Base = CellX + CoarseX * (CellY + CoarseY * CellZ);
						const int Up = CoarseX * CoarseY;

						const auto Drive = [&](const std::vector<float>& Field)
						{
							const float A = Field[Base] + (Field[Base + 1] - Field[Base]) * ShareX;
							const float B = Field[Base + CoarseX] + (Field[Base + CoarseX + 1] - Field[Base + CoarseX]) * ShareX;
							const float C = Field[Base + Up] + (Field[Base + Up + 1] - Field[Base + Up]) * ShareX;
							const float D = Field[Base + Up + CoarseX] + (Field[Base + Up + CoarseX + 1] - Field[Base + Up + CoarseX]) * ShareX;
							const float Near = A + (B - A) * ShareY;
							const float Far = C + (D - C) * ShareY;
							return Near + (Far - Near) * ShareZ;
						};
						NewX += (Drive(AirX) - NewX) * Blend;
						NewY += (Drive(AirY) - NewY) * Blend;
						NewZ += (Drive(AirZ) - NewZ) * Blend;
					}

					NextX[Here] = std::clamp(NewX, -MaxSpeed, MaxSpeed);
					NextY[Here] = std::clamp(NewY, -MaxSpeed, MaxSpeed);
					NextZ[Here] = std::clamp(NewZ, -MaxSpeed, MaxSpeed);
				}
			}
		});

		VelocityX.swap(NextX);
		VelocityY.swap(NextY);
		VelocityZ.swap(NextZ);
	}

	void MagicField::RestoreBaseline(float Dt)
	{
		const float Share = 1.0f - std::exp(-Dt / RestoreTime);
		const float Drain = 1.0f - std::exp(-Dt / DrainTime);
		ParallelFor(GridZ, [&](int Slice)
		{
			const int First = Slice * ColumnCount;
			for (int Type = 0; Type < Sim::MagicTypeCount; ++Type)
			{
				std::vector<float>& Kind = Amounts[Type];
				const std::vector<float>& Wanted = Baselines[Type];
				const std::vector<float>& Limit = Limits[Type];
				for (int Cell = First; Cell < First + ColumnCount; ++Cell)
				{
					float Value = Kind[Cell] + (Wanted[Cell] - Kind[Cell]) * Share;
					if (Value > Limit[Cell])
					{
						Value -= (Value - Limit[Cell]) * Drain;
					}
					Kind[Cell] = Value;
				}
			}
		});
	}

	void MagicField::Transport(int Axis, float Dt)
	{
		const float Courant = Dt / static_cast<float>(CellSize);
		const float Soaking = Diffusivity * Dt / static_cast<float>(CellSize * CellSize);
		const std::vector<float>& Velocity = Axis == 0 ? VelocityX : (Axis == 1 ? VelocityY : VelocityZ);
		const int Count = Axis == 0 ? GridX : (Axis == 1 ? GridY : GridZ);
		const int* Along = Axis == 0 ? MapX : (Axis == 1 ? MapY : MapZ);
		const int* Across = Axis == 0 ? MapY : MapX;
		const int* Outer = Axis == 2 ? MapY : MapZ;
		const int AcrossCount = Axis == 0 ? GridY : GridX;
		const int OuterCount = Axis == 2 ? GridY : GridZ;
		const bool bWalls = !Settings.bOpen;

		ParallelFor(OuterCount, [&](int OuterIndex)
		{
			float Line[LongestLine + 4];
			float Speed[LongestLine + 4];
			std::uint8_t Blocked[LongestLine + 4];

			for (int AcrossIndex = 0; AcrossIndex < AcrossCount; ++AcrossIndex)
			{
				const int Base = Across[AcrossIndex + 2] + Outer[OuterIndex + 2];
				for (int Cell = 0; Cell < Count + 4; ++Cell)
				{
					const int Index = Base + Along[Cell];
					Speed[Cell] = Velocity[Index];
					Blocked[Cell] = Solid[Index];
				}
				if (bWalls)
				{
					Blocked[1] = 1;
					Blocked[Count + 2] = 1;
				}

				for (std::vector<float>& Kind : Amounts)
				{
					for (int Cell = 0; Cell < Count + 4; ++Cell)
					{
						Line[Cell] = Kind[Base + Along[Cell]];
					}

					TransportLine(Line, Speed, Blocked, Count, Courant, Soaking);

					for (int Cell = 2; Cell < Count + 2; ++Cell)
					{
						Kind[Base + Along[Cell]] = Line[Cell];
					}
				}
			}
		});
	}

	void MagicField::Step(const Vec3& Focus, const Vec3& AirDrift, double Time, double Dt)
	{
		const float Span = static_cast<float>(Dt);
		if (Settings.bOpen)
		{
			MoveWindow(Focus);

			// Eddies and currents change little from one step to the next.
			if (StepCount % 2 == 0)
			{
				ComputeAir(AirDrift, Time);
			}
			RestoreBaseline(Span);
		}

		AddUpDensity();
		ComputeSpin();
		UpdateVelocity(Span);

		// One axis at a time, in turns, so that no direction is favoured.
		const bool bForward = StepCount % 2 == 0;
		Transport(bForward ? 0 : 2, Span);
		Transport(1, Span);
		Transport(bForward ? 2 : 0, Span);
		AddUpDensity();
		++StepCount;
	}

	void MagicField::SetDensity(Sim::EMagicType Type, const std::function<double(const Vec3&)>& Value)
	{
		for (int Z = 0; Z < GridZ; ++Z)
		{
			for (int Y = 0; Y < GridY; ++Y)
			{
				for (int X = 0; X < GridX; ++X)
				{
					Amounts[static_cast<int>(Type)][CellIndex(X, Y, Z)] = static_cast<float>(Value(GetCellCenter(X, Y, Z)));
				}
			}
		}
		AddUpDensity();
	}

	void MagicField::SetVelocity(const std::function<Vec3(const Vec3&)>& Value)
	{
		for (int Z = 0; Z < GridZ; ++Z)
		{
			for (int Y = 0; Y < GridY; ++Y)
			{
				for (int X = 0; X < GridX; ++X)
				{
					const int Cell = CellIndex(X, Y, Z);
					const Vec3 Velocity = Value(GetCellCenter(X, Y, Z));
					VelocityX[Cell] = static_cast<float>(Velocity.X);
					VelocityY[Cell] = static_cast<float>(Velocity.Y);
					VelocityZ[Cell] = static_cast<float>(Velocity.Z);
				}
			}
		}
	}

	double MagicField::GetDensity(const Vec3& Position) const
	{
		return Density[FindCell(Position)];
	}

	double MagicField::GetDensity(const Vec3& Position, Sim::EMagicType Type) const
	{
		return Amounts[static_cast<int>(Type)][FindCell(Position)];
	}

	Vec3 MagicField::GetVelocity(const Vec3& Position) const
	{
		const int Cell = FindCell(Position);
		return { VelocityX[Cell], VelocityY[Cell], VelocityZ[Cell] };
	}

	double MagicField::GetTotalAmount(Sim::EMagicType Type) const
	{
		double Sum = 0.0;
		for (const float Value : Amounts[static_cast<int>(Type)])
		{
			Sum += Value;
		}
		return Sum * CellSize * CellSize * CellSize;
	}

	double MagicField::GetPeakSpin() const
	{
		return *std::max_element(Spin.begin(), Spin.end());
	}

	void MagicField::WriteVolume(MagicVolume& Out) const
	{
		Out.SizeX = GridX;
		Out.SizeY = GridY;
		Out.SizeZ = GridZ;
		Out.CellSize = CellSize;
		Out.Corner = { OriginX * CellSize, OriginY * CellSize, OriginZ * CellSize };
		Out.Pixels.resize(static_cast<std::size_t>(CellCount) * 4);
		for (std::vector<float>& Kind : Out.Amounts)
		{
			Kind.resize(CellCount);
		}
		Out.PeakDensity = 0.0;

		std::size_t Index = 0;
		for (int Z = 0; Z < GridZ; ++Z)
		{
			for (int Y = 0; Y < GridY; ++Y)
			{
				for (int X = 0; X < GridX; ++X)
				{
					const int Cell = CellIndex(X, Y, Z);
					double Here[Sim::MagicTypeCount];
					for (int Type = 0; Type < Sim::MagicTypeCount; ++Type)
					{
						Out.Amounts[Type][Index] = Amounts[Type][Cell];
						Here[Type] = Amounts[Type][Cell];
					}
					Out.PeakDensity = std::max(Out.PeakDensity, static_cast<double>(Density[Cell]));
					WritePixel(Out.Pixels.data() + Index * 4, Here, Spin[Cell]);
					++Index;
				}
			}
		}
	}

	MagicSimulation::MagicSimulation(std::uint32_t InSeed)
		: Seed(InSeed)
		, Field(InSeed, { true, std::max(std::thread::hardware_concurrency() / 3u, 2u) })
		, Worker([this] { Run(); })
	{
	}

	MagicSimulation::~MagicSimulation()
	{
		{
			std::lock_guard Lock(Mutex);
			bStopping = true;
		}
		Wake.notify_all();
		Worker.join();
	}

	void MagicSimulation::Advance(const Vec3& NewFocus, const Vec3& NewDrift, double Time)
	{
		{
			std::lock_guard Lock(Mutex);
			Focus = NewFocus;
			AirDrift = NewDrift;
			TargetTime = Time;
			bStarted = true;
		}
		Wake.notify_one();
	}

	bool MagicSimulation::IsReady() const
	{
		std::lock_guard Lock(Mutex);
		return bReady;
	}

	bool MagicSimulation::TakeNear(MagicVolume& InOut)
	{
		std::lock_guard Lock(Mutex);
		if (!bNearFresh)
		{
			return false;
		}
		std::swap(InOut, Near);
		bNearFresh = false;
		return true;
	}

	bool MagicSimulation::TakeFar(MagicVolume& InOut)
	{
		std::lock_guard Lock(Mutex);
		if (!bFarFresh)
		{
			return false;
		}
		std::swap(InOut, Far);
		bFarFresh = false;
		return true;
	}

	void MagicSimulation::Publish(const Vec3& Around)
	{
		MagicVolume Fresh;
		Field.WriteVolume(Fresh);

		// The far box moves in whole cells, so it only needs drawing when it has moved.
		const int CellX = FloorToInt(Around.X / FarCellSize) - FarX / 2;
		const int CellY = FloorToInt(Around.Y / FarCellSize) - FarY / 2;
		const int CellZ = FloorToInt(Around.Z / FarCellSize) - FarZ / 2;
		const bool bFarMoved = !bFarBuilt || CellX != FarCellX || CellY != FarCellY || CellZ != FarCellZ;

		MagicVolume FreshFar;
		if (bFarMoved)
		{
			bFarBuilt = true;
			FarCellX = CellX;
			FarCellY = CellY;
			FarCellZ = CellZ;
			FarWells = Sim::FindMagicWells(Around.X, Around.Y, Seed);

			FreshFar.SizeX = FarX;
			FreshFar.SizeY = FarY;
			FreshFar.SizeZ = FarZ;
			FreshFar.CellSize = FarCellSize;
			FreshFar.Corner = { CellX * FarCellSize, CellY * FarCellSize, CellZ * FarCellSize };
			FreshFar.Pixels.resize(static_cast<std::size_t>(FarX) * FarY * FarZ * 4);

			for (int Y = 0; Y < FarY; ++Y)
			{
				for (int X = 0; X < FarX; ++X)
				{
					const double WorldX = (CellX + X + 0.5) * FarCellSize;
					const double WorldY = (CellY + Y + 0.5) * FarCellSize;
					const Sim::MagicGround Below = Sim::ReadMagicGround(WorldX, WorldY, Seed);

					for (int Z = 0; Z < FarZ; ++Z)
					{
						const Vec3 Center{ WorldX, WorldY, (CellZ + Z + 0.5) * FarCellSize };
						const Sim::MagicMix Mix = Center.Z < Below.Surface ? Sim::MagicMix{} : Sim::MagicBaseline(Center, FarWells, Below, Seed);
						const std::size_t Index = static_cast<std::size_t>(X + FarX * (Y + FarY * Z));
						WritePixel(FreshFar.Pixels.data() + Index * 4, Mix.Amounts, 0.0);
					}
				}
			}
		}

		std::lock_guard Lock(Mutex);
		std::swap(Near, Fresh);
		bNearFresh = true;
		if (bFarMoved)
		{
			std::swap(Far, FreshFar);
			bFarFresh = true;
		}
	}

	void MagicSimulation::Run()
	{
		double Clock = 0.0;
		bool bWarmedUp = false;

		for (;;)
		{
			Vec3 Around;
			Vec3 Drift;
			{
				std::unique_lock Lock(Mutex);
				Wake.wait(Lock, [&] { return bStopping || (bStarted && (!bWarmedUp || TargetTime >= Clock + SimulationStep)); });
				if (bStopping)
				{
					return;
				}
				Around = Focus;
				Drift = AirDrift;
				if (!bWarmedUp)
				{
					Clock = TargetTime;
				}
			}

			if (!bWarmedUp)
			{
				// Started from rest around the first position, leading up to the present moment.
				for (int Step = WarmUpSteps; Step > 0; --Step)
				{
					Field.Step(Around, Drift, Clock - Step * SimulationStep, SimulationStep);
					std::lock_guard Lock(Mutex);
					if (bStopping)
					{
						return;
					}
				}
				bWarmedUp = true;
			}
			else
			{
				Clock += SimulationStep;
				Field.Step(Around, Drift, Clock, SimulationStep);

				// A slow machine falls behind rather than falling further and further behind.
				std::lock_guard Lock(Mutex);
				Clock = std::max(Clock, TargetTime - SimulationStep * 2.0);
			}

			Publish(Around);
			std::lock_guard Lock(Mutex);
			bReady = true;
		}
	}
}
