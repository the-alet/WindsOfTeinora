#include "MagicShows.h"

#include <algorithm>
#include <cmath>

namespace Teinora
{
	namespace
	{
		constexpr double LookEvery = 0.25;      // seconds between looks over the gas

		// Fireflies, as many in a cell of dense light magic as FirefliesFor says.
		constexpr std::size_t MostFireflies = 600;
		constexpr double FireflyWander = 0.6;   // m/s of their own
		constexpr double FireflyFade = 4.0;     // seconds to go out once the light magic has thinned
		constexpr double JoinRadius = 3.0;      // metres: ten fireflies this close make one big one
		constexpr int JoinCount = 10;
		constexpr double BigLife = 120.0;       // seconds
		constexpr double WaveEvery = 2.0;       // seconds between a big one's waves
		constexpr double WaveSpeed = 8.0;       // m/s
		constexpr double WaveReach = 16.0;      // metres
		constexpr double WaveDensity = 1.0;     // kg/m3 of light magic each wave leaves as it passes

		// Whirlwinds.
		constexpr double WhirlRadius = 3.0;     // metres to the fastest wind
		constexpr double WhirlHeight = 25.0;    // metres it stands over the ground
		constexpr double WhirlLeast = 5.0;      // m/s at the density that makes one
		constexpr double WhirlPerExcess = 0.6;  // m/s more for each kg/m3 over that
		constexpr double WhirlFastest = 25.0;

		// Currents.
		constexpr double CurrentPerExcess = 0.15; // m/s for each kg/m3 of water magic over what makes one
		constexpr double CurrentFastest = 3.0;

		// Minerals and plants, each cell of the gas making them a few at a time.
		constexpr double MineralsEvery = 1.5;   // seconds
		constexpr int MostMinerals = 5;         // in one cell
		constexpr double PlantsEvery = 3.0;
		constexpr int MostPlants = 3;

		double GetAmount(const MagicVolume& Magic, Sim::EMagicType Type, std::size_t Index)
		{
			return Magic.Amounts[static_cast<int>(Type)][Index];
		}

		std::int64_t CellKey(std::int64_t X, std::int64_t Y, std::int64_t Z)
		{
			constexpr std::int64_t Mask = (1 << 21) - 1;
			return (X & Mask) | ((Y & Mask) << 21) | ((Z & Mask) << 42);
		}

		/** How many fireflies light magic this dense calls up: 3 where it reaches what makes them, and one more for each 3 kg/m3 over. */
		int FirefliesFor(double Light)
		{
			const double Least = Sim::MagicManifestDensity(Sim::EMagicType::Light);
			return Light < Least ? 0 : 3 + static_cast<int>(std::floor((Light - Least) / 3.0));
		}
	}

	void MagicShows::Update(const MagicVolume& Magic, double Time, double Dt)
	{
		if (Magic.Amounts[0].empty())
		{
			return;
		}
		if (Time - LastLook >= LookEvery || Time < LastLook)
		{
			LastLook = Time;
			Look(Magic, Time);
			JoinFireflies(Time);
		}
		MoveFireflies(Magic, Time, Dt);
		SpreadWaves(Magic, Time);
	}

	void MagicShows::Look(const MagicVolume& Magic, double Time)
	{
		const auto Roll = [this]
		{
			Dice ^= Dice << 13;
			Dice ^= Dice >> 7;
			Dice ^= Dice << 17;
			return static_cast<double>(Dice >> 11) / 9007199254740992.0;
		};

		Whirlwinds.clear();
		Currents.clear();
		Wanted.clear();

		const double Size = Magic.CellSize;
		const double Earth = Sim::MagicManifestDensity(Sim::EMagicType::Earth);
		const double Nature = Sim::MagicManifestDensity(Sim::EMagicType::Nature);
		const double Water = Sim::MagicManifestDensity(Sim::EMagicType::Water);
		const double Air = Sim::MagicManifestDensity(Sim::EMagicType::Air);
		const double Light = Sim::MagicManifestDensity(Sim::EMagicType::Light);

		std::size_t Index = 0;
		for (int Z = 0; Z < Magic.SizeZ; ++Z)
		{
			for (int Y = 0; Y < Magic.SizeY; ++Y)
			{
				for (int X = 0; X < Magic.SizeX; ++X, ++Index)
				{
					const double EarthHere = GetAmount(Magic, Sim::EMagicType::Earth, Index);
					const double NatureHere = GetAmount(Magic, Sim::EMagicType::Nature, Index);
					const double WaterHere = GetAmount(Magic, Sim::EMagicType::Water, Index);
					const double AirHere = GetAmount(Magic, Sim::EMagicType::Air, Index);
					const double LightHere = GetAmount(Magic, Sim::EMagicType::Light, Index);
					if (EarthHere < Earth && NatureHere < Nature && WaterHere < Water && AirHere < Air && LightHere < Light)
					{
						continue;
					}

					const Vec3 Low = Magic.Corner + Vec3{ X * Size, Y * Size, Z * Size };
					const Vec3 Middle = Low + Vec3{ Size, Size, Size } * 0.5;
					const std::int64_t Key = CellKey(static_cast<std::int64_t>(std::floor(Low.X / Size + 0.5)), static_cast<std::int64_t>(std::floor(Low.Y / Size + 0.5)), static_cast<std::int64_t>(std::floor(Low.Z / Size + 0.5)));
					if (LightHere >= Light)
					{
						Wanted[Key] = FirefliesFor(LightHere);
					}

					// The rest stand on the ground, or in the water: only the cell with the surface in it, or the one over that.
					const double Ground = Sim::TerrainHeight(Middle.X, Middle.Y, Sim::WorldSeed);
					const double Shore = Sim::GetShoreLevel(Middle.X, Middle.Y, Sim::WorldSeed);
					const double Surface = std::max(Ground, Shore);
					const bool bAtSurface = Surface >= Low.Z - Size && Surface < Low.Z + Size;

					if (AirHere >= Air && Surface < Low.Z + Size && Low.Z - Surface < WhirlHeight)
					{
						Whirlwind Whirl;
						Whirl.Position = { Middle.X, Middle.Y, Surface };
						Whirl.Speed = std::min(WhirlLeast + (AirHere - Air) * WhirlPerExcess, WhirlFastest);
						Whirl.Radius = WhirlRadius;
						Whirlwinds.push_back(Whirl);
					}
					if (!bAtSurface)
					{
						continue;
					}

					if (WaterHere >= Water && Shore > Ground)
					{
						// It drives the water the way the water magic is going, or out of where it is densest.
						const Vec3 Going = Magic.GetFlow(Middle);
						Vec3 Way{ Going.X, Going.Y, 0.0 };
						if (Length(Way) < 0.1)
						{
							const Vec3 Slope = Magic.GetSlope(Sim::EMagicType::Water, Middle);
							Way = Vec3{ -Slope.X, -Slope.Y, 0.0 };
						}
						if (Length(Way) > 1e-6)
						{
							Currents.push_back({ { Middle.X, Middle.Y, Shore }, Normalize(Way) * std::min((WaterHere - Water) * CurrentPerExcess, CurrentFastest) });
						}
					}

					Making& Here = Made[Key];
					const auto Somewhere = [&]
					{
						const double PlaceX = Low.X + Roll() * Size;
						const double PlaceY = Low.Y + Roll() * Size;
						return Vec3{ PlaceX, PlaceY, Sim::TerrainHeight(PlaceX, PlaceY, Sim::WorldSeed) };
					};
					if (EarthHere >= Earth && Here.Count < MostMinerals && Time - Here.Last >= MineralsEvery)
					{
						// Minerals grow on land and on the beds of lakes and the sea alike.
						Sim::MagicGrowth Growth;
						Growth.Kind = Sim::EMagicGrowth::Minerals;
						Growth.Position = Somewhere();
						Growth.Size = std::min(0.4 + (EarthHere - Earth) * 0.03, 1.4);
						Growth.Seed = static_cast<std::uint32_t>(Roll() * 4294967295.0);
						Growths.push_back(Growth);
						Here.Last = Time;
						++Here.Count;
					}
					else if (NatureHere >= Nature && Here.Count < MostPlants && Time - Here.Last >= PlantsEvery)
					{
						const Vec3 Spot = Somewhere();
						if (Spot.Z > Sim::GetShoreLevel(Spot.X, Spot.Y, Sim::WorldSeed) + 0.3 && Sim::TerrainNormal(Spot.X, Spot.Y, Sim::WorldSeed).Z > 0.75)
						{
							Sim::MagicGrowth Growth;
							Growth.Kind = Sim::EMagicGrowth::Plant;
							Growth.Position = Spot;
							Growth.Size = std::min(0.8 + (NatureHere - Nature) * 0.05, 2.2);
							Growth.Seed = static_cast<std::uint32_t>(Roll() * 4294967295.0);
							Growths.push_back(Growth);
							++Here.Count;
						}
						Here.Last = Time;
					}
				}
			}
		}

		// Each cell of dense light magic calls up as many fireflies as it has room for. Those of cells
		// that have thinned out begin to go out.
		std::unordered_map<std::int64_t, int> Have;
		for (Firefly& Each : Fireflies)
		{
			if (Each.bBig)
			{
				continue;
			}
			const auto Found = Wanted.find(Each.Home);
			if (Found == Wanted.end() || Have[Each.Home] >= Found->second)
			{
				if (Each.Fading < 0.0)
				{
					Each.Fading = Time;
				}
				continue;
			}
			Each.Fading = -1.0;
			++Have[Each.Home];
		}
		for (const auto& [Key, Count] : Wanted)
		{
			const auto Unpack = [Key](int Shift)
			{
				std::int64_t Value = (Key >> Shift) & ((1 << 21) - 1);
				return Value >= (1 << 20) ? Value - (1 << 21) : Value;
			};
			const Vec3 Low{ Unpack(0) * Size, Unpack(21) * Size, Unpack(42) * Size };
			for (int Missing = Count - Have[Key]; Missing > 0 && Fireflies.size() < MostFireflies; --Missing)
			{
				Firefly Fly;
				Fly.Position = Low + Vec3{ Roll(), Roll(), Roll() } * Size;
				Fly.Home = Key;
				Fly.Seed = Roll() * 100.0;
				Fly.Born = Time;
				Fireflies.push_back(Fly);
			}
		}
	}

	void MagicShows::JoinFireflies(double Time)
	{
		// Ten that come close enough together go into one bigger one, as many times over as there are such tens.
		for (std::size_t First = 0; First < Fireflies.size();)
		{
			if (Fireflies[First].bBig || Fireflies[First].Fading >= 0.0)
			{
				++First;
				continue;
			}
			std::vector<std::size_t> Near;
			for (std::size_t Other = 0; Other < Fireflies.size(); ++Other)
			{
				if (!Fireflies[Other].bBig && Fireflies[Other].Fading < 0.0 && LengthSquared(Fireflies[Other].Position - Fireflies[First].Position) < JoinRadius * JoinRadius)
				{
					Near.push_back(Other);
					if (Near.size() == JoinCount)
					{
						break;
					}
				}
			}
			if (Near.size() < JoinCount)
			{
				++First;
				continue;
			}

			Firefly Big;
			for (const std::size_t Each : Near)
			{
				Big.Position = Big.Position + Fireflies[Each].Position * (1.0 / JoinCount);
				Big.Velocity = Big.Velocity + Fireflies[Each].Velocity * (1.0 / JoinCount);
			}
			Big.bBig = true;
			Big.Born = Time;
			Big.NextWave = Time;
			Big.Seed = Fireflies[First].Seed;
			Big.Home = Fireflies[First].Home;
			for (auto Each = Near.rbegin(); Each != Near.rend(); ++Each)
			{
				Fireflies.erase(Fireflies.begin() + static_cast<std::ptrdiff_t>(*Each));
			}
			Fireflies.push_back(Big);
			First = 0;
		}
	}

	void MagicShows::MoveFireflies(const MagicVolume& Magic, double Time, double Dt)
	{
		std::erase_if(Fireflies, [Time](const Firefly& Each)
		{
			return Each.bBig ? Time - Each.Born > BigLife : (Each.Fading >= 0.0 && Time - Each.Fading > FireflyFade);
		});

		const double Size = Magic.CellSize;
		for (Firefly& Each : Fireflies)
		{
			// They drift with the magic, wander a little of their own, and keep to where they came out.
			const Vec3 Flow = Magic.Contains(Each.Position) ? Magic.GetFlow(Each.Position) : Vec3{};
			const double Phase = Time * 0.7 + Each.Seed;
			const Vec3 Own = Vec3{ std::sin(Phase * 1.3), std::cos(Phase * 0.9 + 1.0), std::sin(Phase * 1.7 + 2.0) * 0.5 } * (Each.bBig ? FireflyWander * 0.4 : FireflyWander);
			Vec3 Want = Flow * 0.5 + Own;
			if (!Each.bBig)
			{
				const auto Unpack = [&Each](int Shift)
				{
					std::int64_t Value = (Each.Home >> Shift) & ((1 << 21) - 1);
					return Value >= (1 << 20) ? Value - (1 << 21) : Value;
				};
				const Vec3 Home = Vec3{ Unpack(0) * Size, Unpack(21) * Size, Unpack(42) * Size } + Vec3{ Size, Size, Size } * 0.5;
				Want = Want + (Home - Each.Position) * 0.3;
			}
			Each.Velocity = Each.Velocity + (Want - Each.Velocity) * std::min(Dt * 2.0, 1.0);
			Each.Position = Each.Position + Each.Velocity * Dt;

			if (Each.bBig && Time >= Each.NextWave)
			{
				Waves.push_back({ Each.Position, Time, 0.0 });
				Each.NextWave = Time + WaveEvery;
			}
		}
	}

	void MagicShows::SpreadWaves(const MagicVolume& Magic, double Time)
	{
		// As a wave goes out it leaves its light magic in each cell of the gas it reaches.
		const double Size = Magic.CellSize;
		for (LightWave& Wave : Waves)
		{
			const double Now = std::min((Time - Wave.Born) * WaveSpeed, WaveReach);
			if (Now <= Wave.Reached)
			{
				continue;
			}
			const int Reach = static_cast<int>(std::ceil(Now / Size)) + 1;
			const Vec3 Local = Wave.Center - Magic.Corner;
			const int MiddleX = static_cast<int>(std::floor(Local.X / Size));
			const int MiddleY = static_cast<int>(std::floor(Local.Y / Size));
			const int MiddleZ = static_cast<int>(std::floor(Local.Z / Size));
			for (int Z = MiddleZ - Reach; Z <= MiddleZ + Reach; ++Z)
			{
				for (int Y = MiddleY - Reach; Y <= MiddleY + Reach; ++Y)
				{
					for (int X = MiddleX - Reach; X <= MiddleX + Reach; ++X)
					{
						const Vec3 Cell = Magic.Corner + Vec3{ (X + 0.5) * Size, (Y + 0.5) * Size, (Z + 0.5) * Size };
						const double Away = Length(Cell - Wave.Center);
						if (Away >= Wave.Reached && Away < Now)
						{
							Deposits.push_back({ Cell, Sim::EMagicType::Light, WaveDensity });
						}
					}
				}
			}
			Wave.Reached = Now;
		}
		std::erase_if(Waves, [](const LightWave& Wave) { return Wave.Reached >= WaveReach; });
	}

	Vec3 MagicShows::GetWhirl(const Vec3& Position) const
	{
		Vec3 Sum;
		for (const Whirlwind& Whirl : Whirlwinds)
		{
			const double AcrossX = Position.X - Whirl.Position.X;
			const double AcrossY = Position.Y - Whirl.Position.Y;
			const double Away = std::hypot(AcrossX, AcrossY);
			const double Up = Position.Z - Whirl.Position.Z;
			if (Away > Whirl.Radius * 8.0 || Up < -2.0 || Up > WhirlHeight || Away < 1e-3)
			{
				continue;
			}
			// A core turning as one, and outside it wind that falls off as one over the distance, rising in the middle.
			const double Round = Away < Whirl.Radius ? Whirl.Speed * Away / Whirl.Radius : Whirl.Speed * Whirl.Radius / Away;
			const double Inside = Away < Whirl.Radius ? 1.0 : (Whirl.Radius / Away) * (Whirl.Radius / Away);
			const double Fade = 1.0 - std::clamp((Up - WhirlHeight * 0.6) / (WhirlHeight * 0.4), 0.0, 1.0);
			Sum = Sum + (Vec3{ -AcrossY / Away, AcrossX / Away, 0.0 } * Round + Vec3{ 0.0, 0.0, Whirl.Speed * 0.4 * Inside }) * Fade;
		}
		return Sum;
	}

	Vec3 MagicShows::GetCurrent(const Vec3& Position) const
	{
		Vec3 Sum;
		for (const MagicCurrent& Each : Currents)
		{
			const double Away = std::hypot(Position.X - Each.Position.X, Position.Y - Each.Position.Y);
			if (Away < 6.0)
			{
				Sum = Sum + Each.Flow * (1.0 - Away / 6.0);
			}
		}
		return Sum;
	}

	void MagicShows::CollectPuffs(double Time, std::vector<PuffDraw>& Out) const
	{
		for (const Firefly& Each : Fireflies)
		{
			const double Twinkle = 0.65 + 0.35 * std::sin(Time * (Each.bBig ? 2.0 : 5.0) + Each.Seed * 7.0);
			const double Shown = Each.Fading >= 0.0 ? std::max(1.0 - (Time - Each.Fading) / FireflyFade, 0.0) : std::min((Time - Each.Born) / 0.5, 1.0);
			PuffDraw Draw;
			Draw.Point = Each.Position;
			Draw.Radius = Each.bBig ? 0.5 : 0.14;
			Draw.Thickness = 1.0;
			Draw.Seed = Each.Seed;
			Draw.Floor = -1e9;
			Draw.Shade = 0.0;
			Draw.Glow = (Each.bBig ? 4.0 : 1.5) * Twinkle * Shown;
			Out.push_back(Draw);
		}

		// Dust the whirlwinds lift, on a spiral turning as fast as the wind.
		for (const Whirlwind& Whirl : Whirlwinds)
		{
			constexpr int Puffs = 14;
			const double Turn = Time * Whirl.Speed / Whirl.Radius;
			for (int Index = 0; Index < Puffs; ++Index)
			{
				const double Rise = static_cast<double>(Index) / Puffs;
				const double Angle = Turn + Index * 2.4;
				const double Round = Whirl.Radius * (0.5 + 1.2 * Rise);
				PuffDraw Draw;
				Draw.Point = Whirl.Position + Vec3{ std::cos(Angle) * Round, std::sin(Angle) * Round, 0.6 + Rise * WhirlHeight * 0.5 };
				Draw.Radius = 0.9 + 1.4 * Rise;
				Draw.Thickness = 0.35 * (1.0 - Rise * 0.7);
				Draw.Seed = Index * 3.7 + Whirl.Position.X * 0.01;
				Draw.Floor = Whirl.Position.Z;
				Draw.Shade = 0.6;
				Out.push_back(Draw);
			}
		}
	}

	void MagicShows::CollectLights(std::vector<HotLight>& Out) const
	{
		for (const Firefly& Each : Fireflies)
		{
			if (Each.bBig)
			{
				HotLight Light;
				Light.Point = Each.Position;
				Light.Color = Vec3{ 1.0, 0.95, 0.8 } * 1.6;
				Light.Reach = 12.0;
				Out.push_back(Light);
			}
		}
	}

	void MagicShows::TakeDeposits(std::vector<MagicDeposit>& Out)
	{
		Out.insert(Out.end(), Deposits.begin(), Deposits.end());
		Deposits.clear();
	}

	void MagicShows::TakeGrowths(std::vector<Sim::MagicGrowth>& Out)
	{
		Out.insert(Out.end(), Growths.begin(), Growths.end());
		Growths.clear();
	}
}
