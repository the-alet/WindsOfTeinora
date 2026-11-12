// The open world: terrain chunks streamed around the camera, their static
// colliders, and the loose rigid bodies scattered over them.
#pragma once

#include "Core/ThreadPool.h"
#include "Liquid.h"
#include "MagicField.h"
#include "MagicShows.h"
#include "Physics/Physics.h"
#include "Plants.h"
#include "Render/SceneTypes.h"
#include "TeinoraSim.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Teinora
{
	enum class EPropKind : std::uint8_t
	{
		Crate,
		Barrel,
		Ball,
		Count
	};

	class GameWorld final
	{
	public:
		GameWorld(IChunkDisplay& InDisplay, Physics::World& InPhysics, int InViewRadius, int InDetail);
		~GameWorld();

		void SetViewRadius(int Chunks);
		double GetViewDistance() const;

		/** Chooses the geometry level; chunks of another level are rebuilt, nearest first. */
		void SetDetail(int Level);

		/** Requests chunks around Focus, uploads finished ones and removes what fell behind. */
		void Update(const Vec3& Focus, double Time);

		/** True once everything the current focus asks for has been built. */
		bool IsSettled() const { return bSettled; }

		/** Advances everything that moves: the bodies, the plants they have disturbed, and the water. */
		void Step(double Dt);

		/** Forces of wind, turbulence, water, plants and the ground, called by the physics world for each awake body. */
		void ApplyForces(Physics::BodyId Id, Physics::Body& Item, double Time, double Dt);

		const LiquidField& GetLiquid() const { return Liquid; }
		const PlantField& GetPlants() const { return Plants; }

		/** Lets the magic around the camera act on what is in it. The volume stays the caller's. */
		void SetMagic(const MagicVolume* Near) { Magic = Near; }

		/** Hands over what bodies have done back to the magic since the last call. */
		void TakeMagicPushes(std::vector<MagicPush>& Out);

		/** Hands over the magic that what dense magic made has let out since the last call: the big fireflies' waves of light magic. */
		void TakeMagicDeposits(std::vector<MagicDeposit>& Out);

		/** What dense magic has made: fireflies, whirlwinds, currents. */
		const MagicShows& GetShows() const { return Shows; }

		/** How far the wind has carried the air so far, which moves its eddies with it. */
		const Vec3& GetAirDrift() const { return AirDrift; }

		/** Throws a ball along the line of sight, at the point on it that the eye is resting on, if it can get there. */
		void ThrowBall(const Vec3& Eye, const Vec3& Gaze, double Speed);

		/** Lights a campfire where the line of sight meets dry, level enough ground. False if it does not. */
		bool PlaceCampfire(const Vec3& Eye, const Vec3& Gaze);

		/** Sets a crate, barrel or ball down where the line of sight meets something, or a little way ahead. */
		void PlaceProp(EPropKind Kind, const Vec3& Eye, const Vec3& Gaze);

		void CollectObjects(std::vector<ObjectDraw>& Out) const;
		std::size_t GetPropCount() const { return Props.size(); }

		/** A bolt of lightning landing. All it does comes of the magic it brings: this wakes what is near, and stands in for the gas beyond the gas. */
		void ReceiveStrike(const Vec3& Point, double Time);

		/** Where dense fire has met plants or water: the one catches, and the other boils. */
		void ReceiveFire(const std::vector<FireContact>& Contacts, double Time);

		/** What has caught fire and is still burning. */
		const std::vector<Sim::HotSpot>& GetBurning() const { return Burning; }

		/** What is burning nearest to Eye, nearest first. */
		void CollectHotLights(const Vec3& Eye, std::vector<HotLight>& Out, std::size_t Most);

		/** The puffs of steam and smoke nearest to Eye, nearest first. */
		void CollectPuffs(const Vec3& Eye, std::vector<PuffDraw>& Out, std::size_t Most);

	private:
		struct StaticSpawn
		{
			Physics::Shape Collider;
			Vec3 Position;
		};

		struct PropSpawn
		{
			EPropKind Kind = EPropKind::Crate;
			Vec3 Position;
		};

		/** What a worker thread produces for one chunk. */
		struct ChunkBuild
		{
			int X = 0;
			int Y = 0;
			int Lod = 0;
			int Detail = 0;
			bool bDenseGrass = false;
			std::vector<TerrainVertex> Vertices;
			std::vector<TerrainVertex> Leaves; // joined on after the solid vertices
			std::uint32_t SolidVertexCount = 0;
			std::uint32_t ShadowVertexCount = 0;
			std::vector<float> Heights; // full-detail grid, for physics
			std::vector<std::uint8_t> Surfaces; // two per cell of that grid
			std::vector<StaticSpawn> Statics;
			std::vector<PropSpawn> Props;
			std::vector<PlantLimb> Limbs;
			std::vector<PlantFoliage> Foliage;
			std::vector<Sim::HotSpot> HotSpots; // near enough to reach this chunk
			float MinZ = 0.0f;
			float MaxZ = 0.0f;
		};

		struct Chunk
		{
			int Lod = -1;
			int Detail = 0;
			bool bDenseGrass = false; // right around the eye, where the grass grows thicker
			Physics::GroundId Ground = Physics::NoGround;
			std::vector<Physics::BodyId> Statics;
			std::vector<float> Heights;         // kept for reading the ground without working it out again
			std::vector<std::uint8_t> Surfaces;
			int Cells = 0;
		};

		/** The ground under a point, as the physics has it. */
		struct GroundPoint
		{
			double Height = 0.0;
			Vec3 Normal{ 0.0, 0.0, 1.0 };
			Sim::ESurface Surface = Sim::ESurface::Grass;
		};

		/** What a body last pushed aside, so that the water can be told what changed. */
		struct WaterReading
		{
			Vec3 Position;
			double Volume = 0.0;
		};

		struct Prop
		{
			Physics::BodyId Body = Physics::NoBody;
			EPropKind Kind = EPropKind::Crate;
			double Held = Sim::MatterHolds; // the magic it holds inside itself, in kg/m3, rule 17
		};

		/** Something that has fallen apart for want of magic, as the dust it leaves. */
		struct Crumbled
		{
			Vec3 Point;
			double Radius = 0.0;
			double Born = 0.0;
		};

		/** Eddies change far more slowly than the physics steps, so each body keeps a recent reading. */
		struct AirReading
		{
			Sim::AirFlow Flow;
			double Time = -1.0;
		};

		/** The same for magic, along with what the body has pushed back on it since. */
		struct MagicReading
		{
			Sim::MagicMix Mix;
			Vec3 Flow;
			Vec3 AirSlope;
			Vec3 Impulse;
			double Rising = 0.0; // how fast the air here rises, warmed by fire magic
			Vec3 FireSlope;      // how fire magic rises per metre around here
			double Time = -1.0;
		};

		/** A fire that has gone out, whose last smoke is still rising. */
		struct Smouldering
		{
			Sim::HotSpot Spot;
			double Lit = 0.0;
			double Out = 0.0;
		};

		/** Steam off boiling water, which the wind takes as it rises. */
		struct SteamPuff
		{
			Vec3 Point;
			Vec3 Drift;         // how far the air had gone by then
			double Amount = 0.0; // kg of fire magic that boiled it
			double Born = 0.0;
		};

		struct ResultQueue
		{
			std::mutex Mutex;
			std::deque<std::unique_ptr<ChunkBuild>> Items;
		};

		static std::unique_ptr<ChunkBuild> BuildChunk(int X, int Y, int Lod, int Detail, bool bDenseGrass);
		static void BuildTerrain(ChunkBuild& Build);
		static void ScatterObjects(ChunkBuild& Build);
		static void GrowGrass(ChunkBuild& Build);

		void RequestChunks(int CenterX, int CenterY);
		void ApplyFinishedChunks();
		void ApplyChunk(ChunkBuild& Build);
		void RemoveColliders(Chunk& Target);
		void SpawnProp(EPropKind Kind, const Vec3& Position, const Vec3& Velocity);
		void RemoveProp(std::size_t Index);
		void CullProps(double Time);
		void UpdateBurning(double Time);
		void RefreshHotSpots(const Vec3& Eye);
		double GetFlameStrength(const Sim::HotSpot& Spot, const Vec3& Point) const;
		bool HasGroundAt(double X, double Y) const;
		bool ReadGround(double X, double Y, GroundPoint& Out) const;
		void DisplaceWater(std::size_t Index, const Vec3& Position, double Volume, double Radius);

		IChunkDisplay& Display;
		Physics::World& Physics;
		int ViewRadius = 10;
		int Detail = 1;
		bool bSettled = false;

		std::unordered_map<std::uint64_t, Chunk> Chunks;
		std::unordered_map<std::uint64_t, int> InFlight;
		std::unordered_set<std::uint64_t> Stale; // chunks to build again, for what has been placed on them
		std::vector<Prop> Props;
		std::vector<int> PropKindByBody;
		std::vector<AirReading> AirByBody;
		std::vector<WaterReading> WaterByBody;
		std::vector<MagicReading> MagicByBody;
		std::vector<MagicPush> MagicPushes;
		const MagicVolume* Magic = nullptr;
		MagicShows Shows;
		std::vector<Sim::HotSpot> HotSpotsAround;
		Vec3 HotSpotsCenter{ 1e12, 0.0, 0.0 };
		std::vector<Sim::MagicWell> WellsAround;
		Vec3 WellsCenter{ 1e12, 0.0, 0.0 };
		std::vector<Sim::HotSpot> Burning;
		std::vector<double> BurningUntil;
		std::vector<Smouldering> BurntOut;
		std::vector<SteamPuff> Steam;
		std::vector<Crumbled> Dust;
		Vec3 AirDrift;
		double LastUpdateTime = 0.0;
		double LastCullTime = 0.0;
		PlantField Plants;
		LiquidField Liquid;

		std::shared_ptr<ResultQueue> Finished;
		ThreadPool Workers; // last, so workers stop before anything they touch goes away
	};
}
