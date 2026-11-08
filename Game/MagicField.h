// Magic as a gas on a grid that follows the camera.
//
// Each cell holds a density in kg/m3 for each kind of magic and one velocity they share. The
// equations are those of a compressible gas with the amount of magic as its mass: its pressure
// is the density times the square of a sound speed, a cell accelerates by force over the amount
// in it, and transport moves each kind exactly, with some diffusion between neighbours. There is
// no gravity term. Moving air and magic's own weak currents drag it along, and vorticity
// confinement keeps the swirls the grid would blur away.
//
// Magic passes through land and water like anything else, slowed by them. What it passes through
// gives neutral magic an aspect, and away from there the aspect fades, by the rules in TeinoraSim.h,
// so nothing here says where any aspect should be. The world only keeps up the total density: matter
// holds magic to itself just hard enough to balance the pressure of that, and the wells feed it.
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
		std::vector<float> Flow[3];       // m/s along each axis, likewise
		std::vector<std::uint8_t> Pixels; // RGBA8, in the same order

		bool Contains(const Vec3& Position) const;

		/** Blended from the eight nearest cells. */
		Sim::MagicMix GetMix(const Vec3& Position) const;
		Vec3 GetFlow(const Vec3& Position) const;

		/** How one kind rises per metre around a point. */
		Vec3 GetSlope(Sim::EMagicType Type, const Vec3& Position) const;

	private:
		double Sample(const std::vector<float>& Field, const Vec3& Position) const;
	};

	/** A push on the magic around a point, in N s: what matter does back to it. */
	struct MagicPush
	{
		Vec3 Position;
		Vec3 Impulse;
	};

	/** Magic let into the gas at a point, in kg/m3 of the cell around it. */
	struct MagicDeposit
	{
		Vec3 Position;
		Sim::EMagicType Type = Sim::EMagicType::Lightning;
		double Density = 0.0;
	};

	/** Where fire magic has reached Sim::MagicFierceFire on what it can change. */
	struct FireContact
	{
		Vec3 Position;       // on top of the land or the water
		double Boiled = 0.0; // kg of it spent on boiling water, none where something grows
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

		/** How much the suns are heating the ground, from 0 at night. */
		void SetSunHeat(double Heat) { SunHeat = Heat; }

		void SetDensity(Sim::EMagicType Type, const std::function<double(const Vec3&)>& Density);
		void SetVelocity(const std::function<Vec3(const Vec3&)>& Velocity);

		/** In the cell around Position: of all kinds together, or of one. */
		double GetDensity(const Vec3& Position) const;
		double GetDensity(const Vec3& Position, Sim::EMagicType Type) const;
		Vec3 GetVelocity(const Vec3& Position) const;

		/** The kilograms of magic of one kind in the grid, or of all of it. */
		double GetTotalAmount(Sim::EMagicType Type) const;
		double GetTotalAmount() const;

		/** Changes how the magic in the cell around a point is moving: the impulse over the amount of it. */
		void Push(const MagicPush& Push);

		/** Adds magic from elsewhere to the cell around a point, if it is in the grid. */
		void Deposit(const MagicDeposit& Deposit);

		/** What is burning that was not always there, which gives the magic around it the aspect of fire. */
		void SetBurning(const std::vector<Sim::HotSpot>& Spots) { Burning = Spots; }

		/** Adds on where dense fire has met plants or water since the last call. */
		void TakeContacts(std::vector<FireContact>& Out);

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
		void RestoreAmount(float Dt);
		void UpdateAspects(float Dt);
		void ApplyBurning(float Dt);
		void ActOnMatter(float Dt);
		void RefreshHotImprints();
		bool IsInGrid(const Vec3& Position) const;
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
		std::uint32_t PlacedSeen = 0; // Sim::GetPlacedVersion when the hot spots were last read
		int StepCount = 0;
		int MapX[GridX + 4] = {};
		int MapY[GridY + 4] = {};
		int MapZ[GridZ + 4] = {};

		std::vector<Sim::MagicWell> Wells;
		std::vector<Sim::HotSpot> HotSpots;
		std::vector<Sim::HotSpot> Burning;
		std::vector<FireContact> Contacts;
		double SunHeat = 0.0;
		double WindSpeed = 0.0;
		std::vector<float> Amounts[Sim::MagicTypeCount];
		std::vector<float> Imprints[Sim::MagicTypeCount]; // per second, by what is in each cell
		std::vector<float> Wanted;  // the density the world keeps up, which matter holds in place
		std::vector<float> Slowing; // the share of the stream's speed left in each cell
		std::vector<float> GroundCool;   // how warm the ground is in each cell, in degrees
		std::vector<float> GroundSunned; // and what the suns add to that, where there is ground to heat
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

		/** Lets the gas run up to Time, around Focus. SunHeat says how much the suns are heating the ground. */
		void Advance(const Vec3& Focus, const Vec3& AirDrift, double Time, double SunHeat);

		/** What matter has done back to the magic since the last call, for the next step of the gas. */
		void AddPushes(const std::vector<MagicPush>& Pushes);

		/** Magic to let in at the next step of the gas. */
		void AddDeposit(const MagicDeposit& Deposit);

		/** What is burning that was not always there. */
		void SetBurning(const std::vector<Sim::HotSpot>& Spots);

		/** Hands over where dense fire has met plants or water since the last call. */
		void TakeContacts(std::vector<FireContact>& Out);

		/** True once the gas has had time to get moving. */
		bool IsReady() const;

		/** How far the gas has got, in the world's seconds. It falls behind when time runs faster than it can keep up with. */
		double GetTime() const;

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
		double ReachedTime = 0.0;
		double SunHeat = 0.0;
		std::vector<MagicPush> Pending;
		std::vector<MagicDeposit> PendingDeposits;
		std::vector<Sim::HotSpot> PendingBurning;
		std::vector<FireContact> FinishedContacts;
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
