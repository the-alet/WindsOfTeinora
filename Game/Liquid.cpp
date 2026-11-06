#include "Liquid.h"

#include "TeinoraSim.h"

#include <algorithm>
#include <cmath>

namespace Teinora
{
	namespace
	{
		constexpr double Step60 = 1.0 / 60.0;
		constexpr int MaxSteps = 3;
		constexpr float DeepestFlow = 4.0f;   // deep water moves like this; waves are held to a stable speed
		constexpr float FlowLoss = 0.35f;     // per second, from the bed and the water's own stickiness
		constexpr float RestReturn = 0.08f;   // per second: what is left finds its way out of the patch in the end
		constexpr float FoamLoss = 0.7f;      // per second
		constexpr int Sponge = 14;            // cells at the edge of the patch that soak up what reaches them
		constexpr float Calm = 0.0015f;       // metres, under which the water counts as still
		constexpr int MoveInSteps = 8;        // cells the middle may stray before the patch moves
	}

	LiquidField::LiquidField(std::uint32_t InSeed)
		: Seed(InSeed)
		, Depth(static_cast<std::size_t>(Side) * Side, 0.0f)
		, Height(static_cast<std::size_t>(Side) * Side, 0.0f)
		, FlowX(static_cast<std::size_t>(Side + 1) * Side, 0.0f)
		, FlowY(static_cast<std::size_t>(Side) * (Side + 1), 0.0f)
		, Foam(static_cast<std::size_t>(Side) * Side, 0.0f)
		, Pixels(static_cast<std::size_t>(Side) * Side * 2, 0.0f)
	{
	}

	void LiquidField::FillDepth(int FirstX, int FirstY, int LastX, int LastY)
	{
		// The ground is worked out every few cells, or a fast camera would spend all its time on it.
		constexpr int Stride = 4;
		const auto ReadDepth = [this](int X, int Y)
		{
			const double WorldX = CornerX + (X + 0.5) * Cell;
			const double WorldY = CornerY + (Y + 0.5) * Cell;
			return Sim::FindWater(WorldX, WorldY, Seed).Level - Sim::TerrainHeight(WorldX, WorldY, Seed);
		};

		const int FromX = FirstX / Stride * Stride;
		const int FromY = FirstY / Stride * Stride;
		for (int SampleY = FromY; SampleY < LastY; SampleY += Stride)
		{
			for (int SampleX = FromX; SampleX < LastX; SampleX += Stride)
			{
				const double D00 = ReadDepth(SampleX, SampleY);
				const double D10 = ReadDepth(SampleX + Stride, SampleY);
				const double D01 = ReadDepth(SampleX, SampleY + Stride);
				const double D11 = ReadDepth(SampleX + Stride, SampleY + Stride);
				for (int Y = std::max(SampleY, FirstY); Y < std::min(SampleY + Stride, LastY); ++Y)
				{
					const double PartY = static_cast<double>(Y - SampleY) / Stride;
					for (int X = std::max(SampleX, FirstX); X < std::min(SampleX + Stride, LastX); ++X)
					{
						const double PartX = static_cast<double>(X - SampleX) / Stride;
						const double Low = D00 + (D10 - D00) * PartX;
						const double High = D01 + (D11 - D01) * PartX;
						Depth[At(X, Y)] = static_cast<float>(std::max(Low + (High - Low) * PartY, 0.0));
					}
				}
			}
		}
	}

	void LiquidField::Slide(int CellsX, int CellsY)
	{
		const auto SlideField = [&](std::vector<float>& Field, int Width, int Rows)
		{
			std::vector<float> Moved(Field.size(), 0.0f);
			for (int Y = 0; Y < Rows; ++Y)
			{
				const int FromY = Y + CellsY;
				if (FromY < 0 || FromY >= Rows)
				{
					continue;
				}
				for (int X = 0; X < Width; ++X)
				{
					const int FromX = X + CellsX;
					if (FromX >= 0 && FromX < Width)
					{
						Moved[static_cast<std::size_t>(X) + static_cast<std::size_t>(Y) * Width] = Field[static_cast<std::size_t>(FromX) + static_cast<std::size_t>(FromY) * Width];
					}
				}
			}
			Field.swap(Moved);
		};

		SlideField(Depth, Side, Side);
		SlideField(Height, Side, Side);
		SlideField(Foam, Side, Side);
		SlideField(FlowX, Side + 1, Side);
		SlideField(FlowY, Side, Side + 1);
		CornerX += CellsX * Cell;
		CornerY += CellsY * Cell;

		// The strips that came in at the edges.
		if (CellsX > 0)
		{
			FillDepth(Side - CellsX, 0, Side, Side);
		}
		else if (CellsX < 0)
		{
			FillDepth(0, 0, -CellsX, Side);
		}
		if (CellsY > 0)
		{
			FillDepth(0, Side - CellsY, Side, Side);
		}
		else if (CellsY < 0)
		{
			FillDepth(0, 0, Side, -CellsY);
		}
	}

	void LiquidField::Follow(const Vec3& Eye)
	{
		const double WantedX = std::floor((Eye.X - Side * Cell * 0.5) / Cell) * Cell;
		const double WantedY = std::floor((Eye.Y - Side * Cell * 0.5) / Cell) * Cell;
		if (!bPlaced)
		{
			CornerX = WantedX;
			CornerY = WantedY;
			bPlaced = true;
			FillDepth(0, 0, Side, Side);
			return;
		}

		const int CellsX = static_cast<int>(std::lround((WantedX - CornerX) / Cell));
		const int CellsY = static_cast<int>(std::lround((WantedY - CornerY) / Cell));
		if (std::abs(CellsX) >= Side || std::abs(CellsY) >= Side)
		{
			// A jump: nothing of the old patch is left to keep.
			CornerX = WantedX;
			CornerY = WantedY;
			std::fill(Height.begin(), Height.end(), 0.0f);
			std::fill(Foam.begin(), Foam.end(), 0.0f);
			std::fill(FlowX.begin(), FlowX.end(), 0.0f);
			std::fill(FlowY.begin(), FlowY.end(), 0.0f);
			FillDepth(0, 0, Side, Side);
		}
		else if (std::abs(CellsX) >= MoveInSteps || std::abs(CellsY) >= MoveInSteps)
		{
			Slide(CellsX, CellsY);
		}
	}

	void LiquidField::Displace(double X, double Y, double Volume, double Radius)
	{
		if (!bPlaced)
		{
			return;
		}

		// Spread over a soft patch of about the size of what pushed it, into the water that is there.
		const double Reach = std::max(Radius + Cell * 2.0, Cell * 3.0);
		const int Cells = static_cast<int>(Reach / Cell) + 1;
		const int MiddleX = static_cast<int>(std::floor((X - CornerX) / Cell));
		const int MiddleY = static_cast<int>(std::floor((Y - CornerY) / Cell));

		double Total = 0.0;
		for (int Pass = 0; Pass < 2; ++Pass)
		{
			const double Share = Pass == 1 && Total > 0.0 ? Volume / (Total * Cell * Cell) : 0.0;
			for (int OffsetY = -Cells; OffsetY <= Cells; ++OffsetY)
			{
				for (int OffsetX = -Cells; OffsetX <= Cells; ++OffsetX)
				{
					const int CellX = MiddleX + OffsetX;
					const int CellY = MiddleY + OffsetY;
					if (CellX < 0 || CellY < 0 || CellX >= Side || CellY >= Side || Depth[At(CellX, CellY)] <= 0.0f)
					{
						continue;
					}

					const double Square = (OffsetX * OffsetX + OffsetY * OffsetY) * Cell * Cell / (Reach * Reach);
					if (Square < 1.0)
					{
						const double Weight = (1.0 - Square) * (1.0 - Square);
						if (Pass == 0)
						{
							Total += Weight;
						}
						else
						{
							Height[At(CellX, CellY)] += static_cast<float>(Weight * Share);
						}
					}
				}
			}
			if (Total <= 0.0)
			{
				return;
			}
		}
		bStirred = true;
	}

	void LiquidField::Step(double Dt)
	{
		const float Pull = static_cast<float>(Sim::Gravity * Dt / Cell);
		const float Keep = 1.0f - FlowLoss * static_cast<float>(Dt);

		// Water is pushed from high to low, except across the shore, where it cannot go.
		for (int Y = 0; Y < Side; ++Y)
		{
			for (int X = 1; X < Side; ++X)
			{
				float& Flow = FlowX[static_cast<std::size_t>(X) + static_cast<std::size_t>(Y) * (Side + 1)];
				const std::size_t Here = At(X, Y);
				const std::size_t Left = At(X - 1, Y);
				Flow = Depth[Here] > 0.0f && Depth[Left] > 0.0f ? (Flow - (Height[Here] - Height[Left]) * Pull) * Keep : 0.0f;
			}
		}
		for (int Y = 1; Y < Side; ++Y)
		{
			for (int X = 0; X < Side; ++X)
			{
				float& Flow = FlowY[At(X, Y)];
				const std::size_t Here = At(X, Y);
				const std::size_t Below = At(X, Y - 1);
				Flow = Depth[Here] > 0.0f && Depth[Below] > 0.0f ? (Flow - (Height[Here] - Height[Below]) * Pull) * Keep : 0.0f;
			}
		}

		// And the height follows from what runs in and out: no water is made or lost on the way.
		const float Rate = static_cast<float>(Dt / Cell);
		const float Settle = 1.0f - RestReturn * static_cast<float>(Dt);
		const float FoamKeep = 1.0f - FoamLoss * static_cast<float>(Dt);
		float Largest = 0.0f;
		for (int Y = 0; Y < Side; ++Y)
		{
			for (int X = 0; X < Side; ++X)
			{
				const std::size_t Here = At(X, Y);
				if (Depth[Here] <= 0.0f)
				{
					continue;
				}

				const std::size_t Row = static_cast<std::size_t>(Y) * (Side + 1);
				const float East = FlowX[Row + X + 1] * (X + 1 < Side ? std::min(std::min(Depth[Here], Depth[At(X + 1, Y)]), DeepestFlow) : 0.0f);
				const float West = FlowX[Row + X] * (X > 0 ? std::min(std::min(Depth[Here], Depth[At(X - 1, Y)]), DeepestFlow) : 0.0f);
				const float North = FlowY[At(X, Y + 1 < Side ? Y + 1 : Y)] * (Y + 1 < Side ? std::min(std::min(Depth[Here], Depth[At(X, Y + 1)]), DeepestFlow) : 0.0f);
				const float South = FlowY[Here] * (Y > 0 ? std::min(std::min(Depth[Here], Depth[At(X, Y - 1)]), DeepestFlow) : 0.0f);
				const float Change = (West - East + South - North) * Rate;
				Height[Here] = (Height[Here] + Change) * Settle;

				// Where the surface is thrown about or runs up into the shallows, it breaks white.
				const float Rough = std::abs(Change) * 60.0f + std::max(std::abs(Height[Here]) - Depth[Here] * 0.6f, 0.0f) * 2.0f;
				Foam[Here] = std::min(Foam[Here] * FoamKeep + Rough * static_cast<float>(Dt) * 0.8f, 1.0f);
				Largest = std::max(Largest, std::abs(Height[Here]));
			}
		}

		// The edge of the patch stands for all the water beyond it, which takes what comes without sending it back.
		for (int Y = 0; Y < Side; ++Y)
		{
			for (int X = 0; X < Side; ++X)
			{
				const int FromEdge = std::min({ X, Y, Side - 1 - X, Side - 1 - Y });
				if (FromEdge < Sponge)
				{
					const float Soak = 1.0f - 0.16f * static_cast<float>(Sponge - FromEdge) / Sponge;
					Height[At(X, Y)] *= Soak;
				}
			}
		}

		bStirred = Largest > Calm;
	}

	void LiquidField::Advance(double Dt)
	{
		if (!bStirred)
		{
			Accumulator = 0.0;
			return;
		}

		Accumulator = std::min(Accumulator + Dt, Step60 * MaxSteps);
		bool bStepped = false;
		while (Accumulator >= Step60)
		{
			Step(Step60);
			Accumulator -= Step60;
			bStepped = true;
		}

		if (bStepped)
		{
			for (std::size_t Index = 0; Index < Height.size(); ++Index)
			{
				Pixels[Index * 2] = Height[Index];
				Pixels[Index * 2 + 1] = Foam[Index];
			}
		}
	}

	double LiquidField::GetHeight(double X, double Y) const
	{
		const double GridX = (X - CornerX) / Cell - 0.5;
		const double GridY = (Y - CornerY) / Cell - 0.5;
		const int CellX = static_cast<int>(std::floor(GridX));
		const int CellY = static_cast<int>(std::floor(GridY));
		if (CellX < 0 || CellY < 0 || CellX + 1 >= Side || CellY + 1 >= Side)
		{
			return 0.0;
		}

		const double PartX = GridX - CellX;
		const double PartY = GridY - CellY;
		const double Low = Height[At(CellX, CellY)] + (Height[At(CellX + 1, CellY)] - Height[At(CellX, CellY)]) * PartX;
		const double High = Height[At(CellX, CellY + 1)] + (Height[At(CellX + 1, CellY + 1)] - Height[At(CellX, CellY + 1)]) * PartX;
		return Low + (High - Low) * PartY;
	}

	Vec3 LiquidField::GetFlow(double X, double Y) const
	{
		const int CellX = static_cast<int>(std::floor((X - CornerX) / Cell));
		const int CellY = static_cast<int>(std::floor((Y - CornerY) / Cell));
		if (CellX < 0 || CellY < 0 || CellX >= Side || CellY >= Side)
		{
			return {};
		}

		const std::size_t Row = static_cast<std::size_t>(CellY) * (Side + 1);
		return {
			(FlowX[Row + CellX] + FlowX[Row + CellX + 1]) * 0.5,
			(FlowY[At(CellX, CellY)] + FlowY[At(CellX, std::min(CellY + 1, Side - 1))]) * 0.5,
			0.0 };
	}

	double LiquidField::GetExtraVolume() const
	{
		double Total = 0.0;
		for (const float Value : Height)
		{
			Total += Value;
		}
		return Total * Cell * Cell;
	}
}
