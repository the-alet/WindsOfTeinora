// Trees and shrubs: grown branch by branch from a seed, as a mesh to show and as springy
// limbs and masses of leaves for what flies into them.
#pragma once

#include "Render/SceneTypes.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Teinora
{
	enum class EPlantKind : std::uint8_t
	{
		Fir,
		Pine,
		Oak,
		Birch,
		Bush
	};

	/** A branch or stem: fixed at the root, free at the tip, and bending as a spring does. */
	struct PlantLimb
	{
		Vec3 Root;
		Vec3 Tip;
		float Radius = 0.05f;     // how thick it is at the root
		float Stiffness = 100.0f; // N/m at the tip
		float Mass = 2.0f;        // kg that swing with the tip
	};

	/** A mass of leaves, which a body can pass through at a cost. */
	struct PlantFoliage
	{
		Vec3 Center;
		float Radius = 1.0f;
		float Density = 3.0f; // kg/m3 of what a body has to push aside
	};

	/** How much of a plant to build. */
	struct PlantDetail
	{
		int TrunkSides = 6;
		int BranchLevels = 2;   // 0 leaves only the trunk
		int BranchSides = 3;
		int Whorls = 8;         // tiers of boughs on a conifer
		double Cards = 1.0;     // how many leaf cards, against the most there can be
		double CardSize = 1.0;  // and how large; fewer and larger keep a crown the same size
	};

	/** What growing a plant adds to a chunk. Positions in the meshes are relative to the chunk, in the rest to the world. */
	struct PlantOutput
	{
		std::vector<TerrainVertex>* Solid = nullptr;
		std::vector<TerrainVertex>* Leaves = nullptr;
		std::vector<PlantLimb>* Limbs = nullptr;      // null when nothing is near enough to touch it
		std::vector<PlantFoliage>* Foliage = nullptr; // likewise
		double TrunkRadius = 0.0;                     // of what stops things dead
		double TrunkHeight = 0.0;
	};

	/** Base is relative to the chunk at Origin. */
	void GrowPlant(EPlantKind Kind, const Vec3& Base, const Vec3& Origin, double Height, const PlantDetail& Detail, std::uint64_t Seed, PlantOutput& Out);

	/** The plants near enough to touch, with what has been done to them. */
	class PlantField
	{
	public:
		void AddChunk(std::uint64_t Key, std::vector<PlantLimb>&& Limbs, std::vector<PlantFoliage>&& Foliage, const Vec3& Origin, double Size);
		void RemoveChunk(std::uint64_t Key);

		/** The force on a body moving among the plants, which they feel in turn. Call once for each body at each step. */
		Vec3 Touch(const Vec3& Position, double Radius, const Vec3& Velocity, double Mass, double Dt);

		/** Lets what has been pushed swing back. */
		void Advance(double Dt);

		/** The largest of what is moving, nearest to Eye first. */
		void CollectMoves(const Vec3& Eye, std::vector<PlantMove>& Out, std::size_t Most) const;

		std::size_t GetLimbCount() const { return LimbCount; }

	private:
		static constexpr int Cells = 16;

		struct Motion
		{
			Vec3 Bend;  // of the tip from its rest
			Vec3 Speed;
		};

		struct Chunk
		{
			Vec3 Origin;
			double Size = 0.0;
			std::vector<PlantLimb> Limbs;
			std::vector<PlantFoliage> Foliage;
			std::vector<std::uint32_t> LimbCells[Cells * Cells];
			std::vector<std::uint32_t> FoliageCells[Cells * Cells];
			std::vector<std::uint32_t> LimbStamps;    // the last pass that looked at each limb, so that one in several cells counts once
			std::vector<std::uint32_t> FoliageStamps;
			std::unordered_map<std::uint32_t, Motion> Moving;        // limbs
			std::unordered_map<std::uint32_t, Motion> MovingLeaves;  // masses of leaves
		};

		void TouchChunk(Chunk& Target, const Vec3& Position, double Radius, const Vec3& Velocity, double Mass, double Dt, Vec3& Force);

		std::unordered_map<std::uint64_t, Chunk> Chunks;
		std::uint32_t Stamp = 0;
		std::size_t LimbCount = 0;
	};
}
