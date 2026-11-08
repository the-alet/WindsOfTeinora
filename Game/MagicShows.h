// What aspects of magic make once they are dense enough, rule 14 in TeinoraSim.h: air a whirlwind,
// water a current, earth minerals, nature plants, light fireflies. Fire makes fire, which the gas
// itself works out (MagicField::ActOnMatter). This looks over the gas around the camera a few times
// a second and keeps what is made: the minerals and plants it hands to the world, which builds them
// into the ground for good; whirlwinds and currents last only while the magic stays dense.
#pragma once

#include "Core/Math.h"
#include "MagicField.h"
#include "Render/SceneTypes.h"
#include "TeinoraSim.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Teinora
{
	/** No insect: light magic showing itself where it has gathered. Ten that come together make one big one. */
	struct Firefly
	{
		Vec3 Position;
		Vec3 Velocity;
		std::int64_t Home = 0;  // the cell of the gas it came out of
		double Seed = 0.0;
		double Fading = -1.0;   // when it began to go out, once the light magic thinned, or -1
		bool bBig = false;
		double Born = 0.0;
		double NextWave = 0.0;  // for a big one, when it sends out its next wave of light magic
	};

	/** A wave of light magic going out from a big firefly. */
	struct LightWave
	{
		Vec3 Center;
		double Born = 0.0;
		double Reached = 0.0; // metres out it has already left its magic
	};

	/** Air magic dense enough to spin a whirlwind round a column standing on the ground. */
	struct Whirlwind
	{
		Vec3 Position;
		double Speed = 0.0;  // m/s round its rim
		double Radius = 0.0; // metres to that rim
	};

	/** Water magic dense enough to drive a current in the water it lies in. */
	struct MagicCurrent
	{
		Vec3 Position;
		Vec3 Flow; // m/s
	};

	class MagicShows final
	{
	public:
		/** Looks at the gas for aspects dense enough to make something, and moves what they have made. */
		void Update(const MagicVolume& Magic, double Time, double Dt);

		/** The wind whirlwinds add at a point. */
		Vec3 GetWhirl(const Vec3& Position) const;

		/** The flow currents add to the water at a point. */
		Vec3 GetCurrent(const Vec3& Position) const;

		/** Fireflies as glowing puffs, and the dust that whirlwinds lift. */
		void CollectPuffs(double Time, std::vector<PuffDraw>& Out) const;

		/** The big fireflies, which are bright enough to light what is around them. */
		void CollectLights(std::vector<HotLight>& Out) const;

		/** Light magic the big fireflies have sent out since the last call. */
		void TakeDeposits(std::vector<MagicDeposit>& Out);

		/** Minerals and plants made since the last call. */
		void TakeGrowths(std::vector<Sim::MagicGrowth>& Out);

		const std::vector<Firefly>& GetFireflies() const { return Fireflies; }
		const std::vector<Whirlwind>& GetWhirlwinds() const { return Whirlwinds; }

	private:
		/** When a cell of the gas last made minerals or plants, and how many it has made. */
		struct Making
		{
			double Last = -1e9;
			int Count = 0;
		};

		void Look(const MagicVolume& Magic, double Time);
		void MoveFireflies(const MagicVolume& Magic, double Time, double Dt);
		void JoinFireflies(double Time);
		void SpreadWaves(const MagicVolume& Magic, double Time);

		std::vector<Firefly> Fireflies;
		std::vector<LightWave> Waves;
		std::vector<Whirlwind> Whirlwinds;
		std::vector<MagicCurrent> Currents;
		std::vector<MagicDeposit> Deposits;
		std::vector<Sim::MagicGrowth> Growths;
		std::unordered_map<std::int64_t, Making> Made;
		std::unordered_map<std::int64_t, int> Wanted; // fireflies each cell of dense light magic calls for
		double LastLook = -1e9;
		std::uint64_t Dice = 0x9e3779b97f4a7c15ull;
	};
}
