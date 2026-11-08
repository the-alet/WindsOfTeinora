#include "Storms.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Teinora
{
	namespace
	{
		constexpr double Pi = 3.14159265358979;
		constexpr double StepTime = 0.25;       // seconds
		constexpr double BreakdownRate = 0.05;  // per second, for a cell at the limit
		constexpr int DischargeReach = 4;       // cells around a bolt that it empties
		constexpr double DischargeShare = 0.85; // of the charge within those
		constexpr double FlashReach = 4200.0;   // metres
		constexpr double StrokeFade = 0.055;    // seconds
		constexpr double BranchFade = 0.03;
		constexpr double BoltLife = 1.1;
		constexpr double ChannelReach = 120.0;  // metres of a channel over the ground that the gas gets to hear of
		constexpr double ChannelSpacing = 4.0;  // metres, the size of a cell of that gas
		constexpr double LayerStep = (Sim::CloudTop - Sim::CloudBottom) / 7.0;

		double Saturate(double Value)
		{
			return std::clamp(Value, 0.0, 1.0);
		}

		double SmoothStep(double From, double To, double Value)
		{
			const double Share = Saturate((Value - From) / (To - From));
			return Share * Share * (3.0 - 2.0 * Share);
		}

		std::int64_t Wrap(std::int64_t Value)
		{
			const std::int64_t Rest = Value % StormField::Side;
			return Rest < 0 ? Rest + StormField::Side : Rest;
		}

		/** A way to go, at random. */
		template <typename Random>
		Vec3 PickWay(Random& Roll)
		{
			const double Up = Roll(-1.0, 1.0);
			const double Angle = Roll(0.0, 2.0 * Pi);
			const double Across = std::sqrt(std::max(1.0 - Up * Up, 0.0));
			return { Across * std::cos(Angle), Across * std::sin(Angle), Up };
		}

		/**
		 * A leader finding its way down, in steps that each go off a little, along a heading that
		 * wanders, and with branches that give out. Near the ground it makes straight for it.
		 */
		template <typename Random>
		std::vector<BoltSegment> GrowBolt(const Vec3& Top, const Vec3& Strike, Random& Roll)
		{
			constexpr double Stride = 1.0 / 125.0; // of the way down, at each step
			constexpr double Closing = 260.0;      // metres over the ground where it can see where to go

			const double Drop = Top.Z - Strike.Z;
			const auto GetReached = [&](const Vec3& At) { return Saturate((Top.Z - At.Z) / Drop); };

			std::vector<BoltSegment> Segments;
			const auto AddBranch = [&](auto&& Self, Vec3 At, Vec3 Way, double Current, int Depth) -> void
			{
				const int Steps = static_cast<int>(Roll(10.0, 44.0)) >> Depth;
				for (int Step = 0; Step < Steps && At.Z > Strike.Z + 40.0; ++Step)
				{
					Way = Normalize(Way * 0.8 + PickWay(Roll) * 0.42 + Vec3{ 0.0, 0.0, -0.14 });
					const Vec3 To = At + Way * (Drop * Stride * Roll(0.5, 1.2));
					const double Fading = Current * (1.0 - 0.8 * Step / Steps);
					Segments.push_back({ At, To, Fading, GetReached(At) });
					if (Depth < 2 && Roll(0.0, 1.0) < 0.05)
					{
						Self(Self, At, Normalize(Way + PickWay(Roll) * 0.8), Fading * 0.5, Depth + 1);
					}
					At = To;
				}
			};

			Vec3 At = Top;
			Vec3 Way{ 0.0, 0.0, -1.0 };
			Vec3 Heading = Way;
			while (At.Z > Strike.Z + 0.5 && Segments.size() < 1200)
			{
				const Vec3 Left = Strike - At;
				const double Remaining = Length(Left);
				const Vec3 Aim = Left * (1.0 / std::max(Remaining, 1e-3));
				const double Seen = 1.0 - Saturate((At.Z - Strike.Z) / Closing);
				const double Length = Drop * Stride * Roll(0.45, 1.5);

				if (Roll(0.0, 1.0) < 0.14)
				{
					Heading = Normalize(Aim + Vec3{ Roll(-0.75, 0.75), Roll(-0.75, 0.75), 0.0 });
				}
				Way = Normalize(Way * 0.5 + Heading * 0.55 + Aim * (0.12 + 1.6 * Seen) + PickWay(Roll) * (0.36 * (1.0 - Seen * 0.6)));
				if (Way.Z > -0.2)
				{
					Way = Normalize(Vec3{ Way.X, Way.Y, -0.2 });
				}

				const Vec3 To = Remaining < Length * 1.6 ? Strike : At + Way * Length;
				Segments.push_back({ At, To, 1.0, GetReached(At) });

				// More of them high up, where it has the whole cloud's charge to spread through.
				if (Roll(0.0, 1.0) < 0.05 + 0.16 * (1.0 - GetReached(At)) * (1.0 - GetReached(At)))
				{
					AddBranch(AddBranch, At, Normalize(Way + PickWay(Roll) * 0.95), 0.34, 0);
				}
				At = To;
			}
			return Segments;
		}
	}

	StormField::StormField(std::uint32_t InSeed, std::vector<std::uint8_t> InNoise, int InNoiseSize, double InCloudBase, double InBaseFall)
		: Seed(InSeed)
		, Noise(std::move(InNoise))
		, NoiseSize(InNoiseSize)
		, CloudBase(InCloudBase)
		, BaseFall(InBaseFall)
		, Cells(static_cast<std::size_t>(Side) * Side)
		, Dice(InSeed + 900u)
	{
	}

	StormField::Cell& StormField::CellAt(std::int64_t X, std::int64_t Y)
	{
		return Cells[static_cast<std::size_t>(Wrap(X) + Wrap(Y) * Side)];
	}

	const StormField::Cell& StormField::CellAt(std::int64_t X, std::int64_t Y) const
	{
		return Cells[static_cast<std::size_t>(Wrap(X) + Wrap(Y) * Side)];
	}

	double StormField::SampleNoise(double U, double V, double W) const
	{
		// Red, as Shaders/Clouds.hlsli reads it: trilinear, and tiling.
		const double GridX = (U - std::floor(U)) * NoiseSize - 0.5;
		const double GridY = (V - std::floor(V)) * NoiseSize - 0.5;
		const double GridZ = (W - std::floor(W)) * NoiseSize - 0.5;
		const int BaseX = static_cast<int>(std::floor(GridX));
		const int BaseY = static_cast<int>(std::floor(GridY));
		const int BaseZ = static_cast<int>(std::floor(GridZ));
		const double PartX = GridX - BaseX;
		const double PartY = GridY - BaseY;
		const double PartZ = GridZ - BaseZ;

		const auto Texel = [this](int X, int Y, int Z)
		{
			const std::size_t Index = (static_cast<std::size_t>((Z + NoiseSize) % NoiseSize) * NoiseSize + (Y + NoiseSize) % NoiseSize) * NoiseSize + (X + NoiseSize) % NoiseSize;
			return Noise[Index * 4] / 255.0;
		};
		const auto Row = [&](int Y, int Z)
		{
			return Texel(BaseX, Y, Z) + (Texel(BaseX + 1, Y, Z) - Texel(BaseX, Y, Z)) * PartX;
		};
		const auto Sheet = [&](int Z)
		{
			return Row(BaseY, Z) + (Row(BaseY + 1, Z) - Row(BaseY, Z)) * PartY;
		};
		return Sheet(BaseZ) + (Sheet(BaseZ + 1) - Sheet(BaseZ)) * PartZ;
	}

	double StormField::MeasureCloud(double CarriedX, double CarriedY, double CloudCover) const
	{
		// SampleCloud in Shaders/Clouds.hlsli, without its finer shapes, at a few heights.
		double Depth = 0.0;
		for (int Layer = 0; Layer < 7; ++Layer)
		{
			const double Altitude = Sim::CloudBottom + (Layer + 0.5) * LayerStep;
			const double Height = (Altitude - Sim::CloudBottom) / (Sim::CloudTop - Sim::CloudBottom);
			const double Profile = SmoothStep(0.0, 0.08, Height) * (1.0 - SmoothStep(0.25, 1.0, Height));
			const double Broad = SampleNoise(CarriedX / 4096.0, CarriedY / 4096.0, Altitude / 2048.0 + Drift.Z); // Z: how far they have changed
			Depth += Saturate((Broad - 0.74 + CloudCover * 0.2 - (1.0 - Profile) * 0.16) * 5.0) * LayerStep;
		}
		return Depth;
	}

	void StormField::ReadClouds(Cell& Item)
	{
		const double Middle = (Item.X + 0.5) * CellSize;
		Item.Depth = MeasureCloud(Middle, (Item.Y + 0.5) * CellSize, Cover);
		Item.Vapour = Saturate(Item.Depth / (Sim::CloudTop - Sim::CloudBottom) * 1.6);
		Item.Volume = CellSize * CellSize * std::max(Item.Depth, 50.0);
	}

	void StormField::Fill(Cell& Item, std::int64_t X, std::int64_t Y)
	{
		Item.X = X;
		Item.Y = Y;
		ReadClouds(Item);

		const double WorldX = (X + 0.5) * CellSize;
		const double WorldY = (Y + 0.5) * CellSize;
		const Sim::MagicMix Air = Sim::MagicBaseline({ WorldX, WorldY, Sim::CloudBottom }, {}, Sim::ReadMagicGround(WorldX, WorldY, Seed), Seed);

		Item.Mix = Sim::MagicMix{};
		Item.Mix.Amounts[static_cast<int>(Sim::EMagicType::Air)] = Air.Get(Sim::EMagicType::Air);
		Item.Mix.Amounts[static_cast<int>(Sim::EMagicType::Neutral)] = Air.Get(Sim::EMagicType::Neutral) + Sim::MagicHeldByVapour(Item.Vapour);

		// A cloud has long been what it is by the time anyone comes to it. Its charge starts from nothing.
		Sim::MagicMix Rates;
		Rates.Amounts[static_cast<int>(Sim::EMagicType::Water)] = Sim::MagicVapourImprint(Item.Vapour);
		Rates.Amounts[static_cast<int>(Sim::EMagicType::Air)] = 1.0 / Sim::MagicFadeTime(Sim::EMagicType::Air);
		for (int Round = 0; Round < 30; ++Round)
		{
			Sim::UpdateMagicAspects(Item.Mix, Rates, 60.0);
		}
	}

	void StormField::Step(double Dt)
	{
		for (Cell& Item : Cells)
		{
			if (Item.Depth <= 0.0)
			{
				continue;
			}

			// Vapour goes with the air, and water magic with it. Air magic takes up only part of that.
			const double Rising = Sim::CloudUpdraft(Item.Depth);
			const double Speed = std::hypot(RubbingSpeed, Rising * (1.0 - Sim::MagicWindShare));
			const double Rubbing = Sim::MagicRubPressure(Item.Mix.Get(Sim::EMagicType::Water), Item.Mix.Get(Sim::EMagicType::Air), Speed);

			Sim::MagicMix Rates;
			Rates.Amounts[static_cast<int>(Sim::EMagicType::Water)] = Sim::MagicVapourImprint(Item.Vapour);
			Rates.Amounts[static_cast<int>(Sim::EMagicType::Air)] = 1.0 / Sim::MagicFadeTime(Sim::EMagicType::Air);
			Rates.Amounts[static_cast<int>(Sim::EMagicType::Fire)] = Sim::MagicRubImprint(Rubbing);
			Sim::UpdateMagicAspects(Item.Mix, Rates, Dt);

			const double Pressure = Item.Mix.GetTotal() * Sim::MagicSoundSpeed * Sim::MagicSoundSpeed + Rubbing;
			Sim::UpdateFireForms(Item.Mix, Pressure, Dt);
		}

		std::uniform_real_distribution<double> Share(0.0, 1.0);
		for (Cell& Item : Cells)
		{
			const double Charge = Item.Mix.Get(Sim::EMagicType::Lightning);
			if (Charge >= Sim::MagicBreakdown && Share(Dice) < 1.0 - std::exp(-BreakdownRate * Charge / Sim::MagicBreakdown * Dt))
			{
				Discharge(Item.X, Item.Y);
			}
		}
	}

	void StormField::Discharge(std::int64_t X, std::int64_t Y)
	{
		const int Lightning = static_cast<int>(Sim::EMagicType::Lightning);
		const int Neutral = static_cast<int>(Sim::EMagicType::Neutral);

		// Axiom 11: what was charged goes back to neutral, and no amount is lost by it.
		for (std::int64_t Row = Y - DischargeReach; Row <= Y + DischargeReach; ++Row)
		{
			for (std::int64_t Column = X - DischargeReach; Column <= X + DischargeReach; ++Column)
			{
				Cell& Item = CellAt(Column, Row);
				if (Item.X == Column && Item.Y == Row)
				{
					const double Spent = Item.Mix.Amounts[Lightning] * DischargeShare;
					Item.Mix.Amounts[Lightning] -= Spent;
					Item.Mix.Amounts[Neutral] += Spent;
				}
			}
		}

		std::uniform_real_distribution<double> Offset(-0.5, 0.5);
		const Vec3 Under{ (X + 0.5 + Offset(Dice)) * CellSize + Drift.X, (Y + 0.5 + Offset(Dice)) * CellSize + Drift.Y, 0.0 };
		const double Away = std::hypot(Under.X - Eye.X, Under.Y - Eye.Y);
		const Vec3 Strike = FindStrikePoint(Under);
		AddBolt(Vec3{ Under.X, Under.Y, std::max(CloudBase - BaseFall * Away * Away, Strike.Z + 200.0) }, Strike, Now);
	}

	Vec3 StormField::FindStrikePoint(const Vec3& Under)
	{
		// It goes for what stands highest around there.
		std::uniform_real_distribution<double> Offset(-110.0, 110.0);
		Vec3 Best = Under;
		Best.Z = -1e30;
		for (int Try = 0; Try < 24; ++Try)
		{
			const double X = Under.X + Offset(Dice);
			const double Y = Under.Y + Offset(Dice);
			const double Height = std::max(Sim::TerrainHeight(X, Y, Seed), Sim::FindWater(X, Y, Seed).Level);
			if (Height > Best.Z)
			{
				Best = { X, Y, Height };
			}
		}
		return Best;
	}

	void StormField::AddBolt(const Vec3& Top, const Vec3& Strike, double Born)
	{
		std::uniform_real_distribution<double> Share(0.0, 1.0);
		const auto Roll = [this, &Share](double From, double To) { return From + (To - From) * Share(Dice); };

		Bolt Item;
		Item.Top = Top;
		Item.Strike = Strike;
		Item.Born = Born;
		Item.Segments = GrowBolt(Top, Strike, Roll);

		// Most bolts strike more than once down the same channel.
		Item.Strokes = 1 + static_cast<int>(Roll(0.0, 3.99));
		double When = LeaderTime;
		double Size = 1.0;
		for (int Stroke = 0; Stroke < Item.Strokes; ++Stroke)
		{
			Item.StrokeTimes[Stroke] = When;
			Item.StrokeSizes[Stroke] = Size;
			When += Roll(0.05, 0.16);
			Size *= Roll(0.4, 0.8);
		}

		Bolts.push_back(std::move(Item));
		++BoltCount;
	}

	void StormField::ForceStrike(const Vec3& Point)
	{
		const double Away = std::hypot(Point.X - Eye.X, Point.Y - Eye.Y);
		const Vec3 Strike{ Point.X, Point.Y, std::max(Sim::TerrainHeight(Point.X, Point.Y, Seed), Sim::FindWater(Point.X, Point.Y, Seed).Level) };
		AddBolt(Vec3{ Point.X, Point.Y, std::max(CloudBase - BaseFall * Away * Away, Strike.Z + 200.0) }, Strike, Now - LeaderTime);
	}

	void StormField::Update(const Vec3& Focus, const Vec3& CloudDrift, const Vec3& Wind, double CloudCover, double Time)
	{
		Eye = Focus;
		Drift = CloudDrift;
		Now = Time;
		RubbingSpeed = Length(Wind) * (1.0 - Sim::MagicWindShare);

		const bool bCoverChanged = CloudCover != Cover;
		Cover = CloudCover;

		const auto CenterX = static_cast<std::int64_t>(std::floor((Focus.X - Drift.X) / CellSize));
		const auto CenterY = static_cast<std::int64_t>(std::floor((Focus.Y - Drift.Y) / CellSize));
		for (std::int64_t Y = CenterY - Side / 2; Y < CenterY + Side / 2; ++Y)
		{
			for (std::int64_t X = CenterX - Side / 2; X < CenterX + Side / 2; ++X)
			{
				Cell& Item = CellAt(X, Y);
				if (Item.X != X || Item.Y != Y)
				{
					Fill(Item, X, Y);
				}
				else if (bCoverChanged)
				{
					ReadClouds(Item);
				}
			}
		}

		if (!bStarted)
		{
			Clock = Time;
			bStarted = true;
		}
		Clock = std::max(Clock, Time - 2.0); // nothing is owed for a long pause
		while (Clock + StepTime <= Time)
		{
			Step(StepTime);
			Clock += StepTime;
		}

		for (Bolt& Item : Bolts)
		{
			if (!Item.bLanded && Time >= Item.Born + LeaderTime)
			{
				Item.bLanded = true;
				BoltStrike Struck;
				Struck.Point = Item.Strike;
				for (const BoltSegment& Segment : Item.Segments)
				{
					if (Segment.Current >= 1.0 && Segment.From.Z < Item.Strike.Z + ChannelReach)
					{
						const double Along = Length(Segment.To - Segment.From);
						const int Points = std::max(static_cast<int>(Along / ChannelSpacing), 1);
						for (int Index = 0; Index < Points; ++Index)
						{
							Struck.Channel.push_back(Segment.From + (Segment.To - Segment.From) * (static_cast<double>(Index) / Points));
						}
					}
				}
				Struck.Channel.push_back(Item.Strike); // each length stops short of its end, and this is where the last one ends
				CarriedDown += static_cast<double>(Struck.Channel.size()) * Sim::MagicChannelAmount;
				Strikes.push_back(std::move(Struck));
			}
		}
		std::erase_if(Bolts, [Time](const Bolt& Item) { return Time > Item.Born + BoltLife; });
	}

	void StormField::TakeStrikes(std::vector<BoltStrike>& Out)
	{
		Out.clear();
		Out.swap(Strikes);
	}

	double StormField::GetStroke(const Bolt& Item) const
	{
		const double Age = Now - Item.Born;
		double Stroke = 0.0;
		for (int Index = 0; Index < Item.Strokes; ++Index)
		{
			if (Age >= Item.StrokeTimes[Index])
			{
				Stroke += Item.StrokeSizes[Index] * std::exp(-(Age - Item.StrokeTimes[Index]) / StrokeFade);
			}
		}
		return Stroke;
	}

	void StormField::CollectBolts(std::vector<BoltDraw>& Out) const
	{
		Out.clear();
		for (const Bolt& Item : Bolts)
		{
			const double Age = Now - Item.Born;
			BoltDraw Draw;
			Draw.Segments = &Item.Segments;
			Draw.Leader = Saturate(Age / LeaderTime);
			Draw.Faint = Age < LeaderTime ? 0.1 : 0.0;
			Draw.Stroke = GetStroke(Item);
			Draw.Branches = Age >= LeaderTime ? std::exp(-(Age - LeaderTime) / BranchFade) : 0.0;
			Out.push_back(Draw);
		}
	}

	void StormField::AddFlashes(std::vector<HotLight>& Lights, std::size_t Most) const
	{
		for (const Bolt& Item : Bolts)
		{
			const double Stroke = GetStroke(Item);
			if (Stroke > 0.01)
			{
				HotLight Flash;
				Flash.Point = Item.Strike + Vec3{ 0.0, 0.0, 240.0 };
				Flash.Color = Vec3{ 6.0, 6.6, 9.0 } * Stroke;
				Flash.Reach = FlashReach;
				Lights.insert(Lights.begin(), Flash);
			}
		}
		if (Lights.size() > Most)
		{
			Lights.resize(Most);
		}
	}

	double StormField::GetCloudFlash(Vec3& OutPoint) const
	{
		double Brightest = 0.0;
		for (const Bolt& Item : Bolts)
		{
			const double Stroke = GetStroke(Item);
			if (Stroke > Brightest)
			{
				Brightest = Stroke;
				OutPoint = Item.Top;
			}
		}
		return Brightest;
	}

	StormField::Reading StormField::Read(const Vec3& Position) const
	{
		Reading Result;
		const auto X = static_cast<std::int64_t>(std::floor((Position.X - Drift.X) / CellSize));
		const auto Y = static_cast<std::int64_t>(std::floor((Position.Y - Drift.Y) / CellSize));
		const Cell& Item = CellAt(X, Y);
		if (Item.X == X && Item.Y == Y)
		{
			Result.CloudDepth = Item.Depth;
			Result.Speed = std::hypot(RubbingSpeed, Sim::CloudUpdraft(Item.Depth) * (1.0 - Sim::MagicWindShare));
			Result.Rubbing = Sim::MagicRubPressure(Item.Mix.Get(Sim::EMagicType::Water), Item.Mix.Get(Sim::EMagicType::Air), Result.Speed);
			Result.Mix = Item.Mix;
		}
		return Result;
	}

	double StormField::GetTotalAmount() const
	{
		double Total = 0.0;
		for (const Cell& Item : Cells)
		{
			Total += Item.Mix.GetTotal() * Item.Volume;
		}
		return Total;
	}
}
