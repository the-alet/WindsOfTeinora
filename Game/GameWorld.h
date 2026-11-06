// The open world: terrain chunks streamed around the camera, their static
// colliders, and the loose rigid bodies scattered over them.
#pragma once

#include "Core/ThreadPool.h"
#include "Liquid.h"
#include "Physics/Physics.h"
#include "Plants.h"
#include "Render/SceneTypes.h"
#include "TeinoraSim.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Teinora
{
	enum class EPropKind : std::uint8_t
	{
		Crate,
		Barrel,
		Boulder,
		Balloon,
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

		/** How far the wind has carried the air so far, which moves its eddies with it. */
		const Vec3& GetAirDrift() const { return AirDrift; }

		/** Throws a ball along the line of sight, at the point on it that the eye is resting on, if it can get there. */
		void ThrowBall(const Vec3& Eye, const Vec3& Gaze, double Speed);

		void CollectObjects(std::vector<ObjectDraw>& Out) const;
		std::size_t GetPropCount() const { return Props.size(); }

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
			float MinZ = 0.0f;
			float MaxZ = 0.0f;
		};

		struct Chunk
		{
			int Lod = -1;
			int Detail = 0;
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
		};

		/** Eddies change far more slowly than the physics steps, so each body keeps a recent reading. */
		struct AirReading
		{
			Sim::AirFlow Flow;
			double Time = -1.0;
		};

		struct ResultQueue
		{
			std::mutex Mutex;
			std::deque<std::unique_ptr<ChunkBuild>> Items;
		};

		static std::unique_ptr<ChunkBuild> BuildChunk(int X, int Y, int Lod, int Detail);
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
		std::vector<Prop> Props;
		std::vector<int> PropKindByBody;
		std::vector<AirReading> AirByBody;
		std::vector<WaterReading> WaterByBody;
		Vec3 AirDrift;
		double LastUpdateTime = 0.0;
		PlantField Plants;
		LiquidField Liquid;

		std::shared_ptr<ResultQueue> Finished;
		ThreadPool Workers; // last, so workers stop before anything they touch goes away
	};
}
