// Thunderstorms, by the axioms in TeinoraSim.h. Clouds are vapour, which gives magic the aspect of
// water and holds it, so a cloud is a mix of water and air magic. Water magic goes where the vapour
// goes. Air magic only takes up part of the air's motion, so the two pass through each other, the
// faster the taller the cloud, since tall clouds churn. Their rubbing makes lightning, which builds
// up in the cloud as a charge until it breaks through to the ground as a bolt.
//
// This runs on a coarse sheet of cells at the height of the clouds, fixed to the clouds as they
// drift, over a far wider area than the gas around the camera, and far above it.
#pragma once

#include "Core/Math.h"
#include "Render/SceneTypes.h"
#include "TeinoraSim.h"

#include <cstdint>
#include <random>
#include <vector>

namespace Teinora
{
	/** What a bolt brings to where it lands. */
	struct BoltStrike
	{
		Vec3 Point;
		std::vector<Vec3> Channel; // the last of its way down, every few metres
	};

	class StormField
	{
	public:
		static constexpr int Side = 48;
		static constexpr double CellSize = 400.0;
		static constexpr double LeaderTime = 0.14; // seconds for a bolt to find its way down

		/** How a storm stands above a point. */
		struct Reading
		{
			double CloudDepth = 0.0; // metres of full cloud
			double Speed = 0.0;      // how fast water and air magic pass each other there, in m/s
			double Rubbing = 0.0;    // the pressure of that, in Pa
			Sim::MagicMix Mix;
		};

		/**
		 * Noise is what shapes the clouds, MakeCloudNoise's RGBA8. CloudBase is the height of their
		 * underside over the camera, and BaseFall how far that sinks per square metre of distance
		 * from it, since the world's clouds lie on a sphere.
		 */
		StormField(std::uint32_t InSeed, std::vector<std::uint8_t> InNoise, int InNoiseSize, double InCloudBase, double InBaseFall);

		/** Advances the storms up to Time, around Focus. CloudDrift is how far the clouds have moved so far, and in Z how far they have changed. */
		void Update(const Vec3& Focus, const Vec3& CloudDrift, const Vec3& Wind, double CloudCover, double Time);

		/** Hands over where bolts have landed since the last call. */
		void TakeStrikes(std::vector<BoltStrike>& Out);

		/** The bolts in the sky at this moment. They point into this object, so they last until the next Update. */
		void CollectBolts(std::vector<BoltDraw>& Out) const;

		/** Puts the light of bolts first among those near the camera, and keeps Most of them. */
		void AddFlashes(std::vector<HotLight>& Lights, std::size_t Most) const;

		/** The brightest flash in the clouds: where it is, and how strong, 0 for none. */
		double GetCloudFlash(Vec3& OutPoint) const;

		Reading Read(const Vec3& Position) const;

		/** Metres of full cloud over a point of the clouds' own map, at a share of the usual cover. */
		double MeasureCloud(double CarriedX, double CarriedY, double CloudCover) const;

		/** Sends a bolt down at once from the cloud base over a point, already at its brightest. For tests. */
		void ForceStrike(const Vec3& Point);

		/** All the magic in the sheet, in kg. */
		double GetTotalAmount() const;

		/** The kg of magic that bolts have brought down out of the clouds so far. */
		double GetCarriedDown() const { return CarriedDown; }

		/** How many bolts there have been so far. */
		int GetBoltCount() const { return BoltCount; }

	private:
		struct Cell
		{
			std::int64_t X = INT64_MIN;
			std::int64_t Y = 0;
			Sim::MagicMix Mix;
			double Depth = 0.0;
			double Vapour = 0.0;
			double Volume = 0.0;
		};

		struct Bolt
		{
			std::vector<BoltSegment> Segments;
			Vec3 Top;
			Vec3 Strike;
			double Born = 0.0;
			int Strokes = 1;
			double StrokeTimes[4] = {};
			double StrokeSizes[4] = {};
			bool bLanded = false;
		};

		Cell& CellAt(std::int64_t X, std::int64_t Y);
		const Cell& CellAt(std::int64_t X, std::int64_t Y) const;
		double SampleNoise(double U, double V, double W) const;
		void Fill(Cell& Item, std::int64_t X, std::int64_t Y);
		void ReadClouds(Cell& Item);
		void Step(double Dt);
		void Discharge(std::int64_t X, std::int64_t Y);
		Vec3 FindStrikePoint(const Vec3& Under);
		void AddBolt(const Vec3& Top, const Vec3& Strike, double Born);
		double GetStroke(const Bolt& Item) const;

		std::uint32_t Seed;
		std::vector<std::uint8_t> Noise;
		int NoiseSize;
		double CloudBase;
		double BaseFall;

		std::vector<Cell> Cells;
		std::vector<Bolt> Bolts;
		std::vector<BoltStrike> Strikes;
		std::mt19937 Dice;

		Vec3 Eye;
		Vec3 Drift;
		double RubbingSpeed = 0.0; // what the wind alone does
		double Cover = -1.0;
		double Clock = 0.0;
		double Now = 0.0;
		bool bStarted = false;
		int BoltCount = 0;
		double CarriedDown = 0.0;
	};
}
