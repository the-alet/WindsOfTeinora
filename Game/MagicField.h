// Magic as a gas on a grid that follows the camera.
//
// Each cell holds a density in kg/m3 for each kind of magic and one velocity they share. The
// equations are those of a compressible gas with the amount of magic as its mass: its pressure
// is the density times the square of a sound speed, a cell accelerates by force over the amount
// in it, and transport conserves each kind exactly, with some diffusion between neighbours.
// There is no gravity term. Moving air and magic's own weak currents drag it along, and
// vorticity confinement keeps the swirls the grid would blur away. The world's wells and thin
// places restore a baseline over several seconds, and whatever is carried past the limit of its
// kind, such as water magic pushed inland, drains away soon after.
#pragma once

#include "Core/Math.h"
#include "Core/ThreadPool.h"
#include "TeinoraSim.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace Teinora
{
	/** Magic over a box of the world, to read back or to draw. */
	struct MagicVolume
	{
		// Red holds the square root of the total density over this scale, which keeps thin magic
		// from vanishing in eight bits. The rest holds the colour of the mix, lit up a little
		// where the gas spins, so the renderers need not know the kinds of magic.
		static constexpr double DensityScale = 24.0;

		int SizeX = 0;
		int SizeY = 0;
		int SizeZ = 0;
		double CellSize = 0.0;
		Vec3 Corner;                      // the box's low corner in the world
		double PeakDensity = 0.0;         // the densest cell
		std::vector<float> Amounts[Sim::MagicTypeCount]; // kg/m3, X fastest; empty when only drawn
		std::vector<std::uint8_t> Pixels; // RGBA8, in the same order

		bool Contains(const Vec3& Position) const;

		/** Blended from the eight nearest cells. */
		Sim::MagicMix GetMix(const Vec3& Position) const;
	};

	class MagicField
	{
	public:
		static constexpr int GridX = 96;
		static constexpr int GridY = 96;
		static constexpr int GridZ = 48;
		static constexpr double CellSize = 4.0;

		struct Options
		{
			bool bOpen = true;    // false shuts the grid in walls, away from wind, wells and ground
			unsigned Threads = 1;
		};

		MagicField(std::uint32_t InSeed, const Options& InOptions);

		/** Moves the grid to stay around Focus and advances the gas. Open grids want the same Dt each time. */
		void Step(const Vec3& Focus, const Vec3& AirDrift, double Time, double Dt);

		void SetDensity(Sim::EMagicType Type, const std::function<double(const Vec3&)>& Density);
		void SetVelocity(const std::function<Vec3(const Vec3&)>& Velocity);

		/** In the cell around Position: of all kinds together, or of one. */
		double GetDensity(const Vec3& Position) const;
		double GetDensity(const Vec3& Position, Sim::EMagicType Type) const;
		Vec3 GetVelocity(const Vec3& Position) const;

		/** The kilograms of magic of one kind in the grid. */
		double GetTotalAmount(Sim::EMagicType Type) const;

		/** The strongest spin in the grid, in radians per second. */
		double GetPeakSpin() const;

		void WriteVolume(MagicVolume& Out) const;

	private:
		int CellIndex(int X, int Y, int Z) const { return MapX[X + 2] + MapY[Y + 2] + MapZ[Z + 2]; }
		int FindCell(const Vec3& Position) const;
		Vec3 GetCellCenter(int X, int Y, int Z) const;
		void RefreshMaps();
		void MoveWindow(const Vec3& Focus);
		void ReadColumn(int WorldX, int WorldY);
		void FillCell(int WorldX, int WorldY, int WorldZ);
		void ComputeAir(const Vec3& AirDrift, double Time);
		void ComputeSpin();
		void AddUpDensity();
		void UpdateVelocity(float Dt);
		void RestoreBaseline(float Dt);
		void Transport(int Axis, float Dt);
		void ParallelFor(int Count, const std::function<void(int)>& Body);

		std::uint32_t Seed;
		Options Settings;
		std::unique_ptr<ThreadPool> Pool;

		// The grid is a window onto a lattice of world cells. Storage wraps around, so moving
		// the window only rewrites the cells that enter it.
		int OriginX = 0;
		int OriginY = 0;
		int OriginZ = 0;
		bool bPlaced = false;
		int WellCellX = 0;
		int WellCellY = 0;
		int StepCount = 0;
		int MapX[GridX + 4] = {};
		int MapY[GridY + 4] = {};
		int MapZ[GridZ + 4] = {};

		std::vector<Sim::MagicWell> Wells;
		std::vector<float> Amounts[Sim::MagicTypeCount];
		std::vector<float> Baselines[Sim::MagicTypeCount];
		std::vector<float> Limits[Sim::MagicTypeCount];
		std::vector<float> Density; // all the kinds together, which is what pressure comes from
		std::vector<float> VelocityX;
		std::vector<float> VelocityY;
		std::vector<float> VelocityZ;
		std::vector<float> NextX;
		std::vector<float> NextY;
		std::vector<float> NextZ;
		std::vector<float> CurlX;
		std::vector<float> CurlY;
		std::vector<float> CurlZ;
		std::vector<float> Spin;
		std::vector<std::uint8_t> Solid;
		std::vector<Sim::MagicGround> Columns;
		std::vector<float> AirX;       // how the magic is being driven, on a coarser grid relative to the window
		std::vector<float> AirY;
		std::vector<float> AirZ;
	};

	/**
	 * Runs a MagicField on a thread of its own, at a steady rate of simulated time. The game asks
	 * for the latest result each frame and never waits for it. Far beyond the grid, a coarse
	 * volume of the baseline stands in.
	 */
	class MagicSimulation
	{
	public:
		explicit MagicSimulation(std::uint32_t InSeed);
		~MagicSimulation();

		MagicSimulation(const MagicSimulation&) = delete;
		MagicSimulation& operator=(const MagicSimulation&) = delete;

		/** Lets the gas run up to Time, around Focus. */
		void Advance(const Vec3& Focus, const Vec3& AirDrift, double Time);

		/** True once the gas has had time to get moving. */
		bool IsReady() const;

		/** Swaps in the newest grid, if there is one. Returns false otherwise. */
		bool TakeNear(MagicVolume& InOut);
		bool TakeFar(MagicVolume& InOut);

	private:
		void Run();
		void Publish(const Vec3& Focus);

		std::uint32_t Seed;
		MagicField Field;

		mutable std::mutex Mutex;
		std::condition_variable Wake;
		Vec3 Focus;
		Vec3 AirDrift;
		double TargetTime = 0.0;
		bool bStarted = false;
		bool bReady = false;
		bool bStopping = false;

		MagicVolume Near;
		MagicVolume Far;
		bool bNearFresh = false;
		bool bFarFresh = false;

		// What the thread keeps for itself between publishing.
		int FarCellX = 0;
		int FarCellY = 0;
		int FarCellZ = 0;
		bool bFarBuilt = false;
		std::vector<Sim::MagicWell> FarWells;

		std::thread Worker; // last, so it starts with everything above in place
	};
}
