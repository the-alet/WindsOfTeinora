// The map of the night sky: one fixed catalogue of stars, made from the world's seed, so that the
// same stars stand in the same places in every run and in both games.
#pragma once

#include "Render/SceneTypes.h"

#include <cstdint>
#include <vector>

namespace Teinora
{
	inline constexpr int StarMapCount = 16000;
	inline constexpr double FaintestMagnitude = 7.0;

	// The cells to a side of the star sheet below, and the stars each can hold. Both are also
	// in Unreal/Scripts/make_content.py.
	inline constexpr int StarCellSide = 640;
	inline constexpr int StarsPerCell = 3;

	// A brightest star has 2 to this power the flux of a faintest one, as the sheet stores it.
	inline constexpr double StarFluxStops = 14.0;

	/**
	 * Stars over the whole sphere, brightest first. Their numbers by brightness follow the real
	 * sky, with about 5000 down to magnitude 6 and a handful brighter than magnitude 1; they lie
	 * evenly but for chance and some tight clusters, and most are white with some blue and orange.
	 */
	std::vector<SkyStar> MakeStarMap(std::uint32_t Seed);

	/** The magnitude a star of this flux has, where brighter is lower and 5 steps are 100 times. */
	double GetStarMagnitude(double Flux);

	/**
	 * A chart of the map to look at, as RGBA8, Height * 2 wide: a disc for each half of the
	 * sphere around the pole it turns on, with lines every 30 degrees.
	 */
	std::vector<std::uint8_t> DrawStarChart(const std::vector<SkyStar>& Stars, int Height);

	/** Where a direction falls when the sphere is folded out into a square, from 0 to 1 each way. */
	void FoldDirection(const Vec3& Direction, double& U, double& V);
	Vec3 UnfoldDirection(double U, double V);

	/** The map arranged for a pixel shader to find the stars around any direction. */
	struct StarSheet
	{
		// RGBA8, StarCellSide high and StarCellSide * StarsPerCell * 2 wide. A star takes two texels
		// side by side in its cell: where in the cell it lies and how bright (0 for no star), then
		// its colour. A cell's first star is its brightest.
		std::vector<std::uint8_t> Pixels;
		int Placed = 0;
	};

	/** When a cell is full the fainter star drops out, which only happens to a few in dense clusters. */
	StarSheet MakeStarSheet(const std::vector<SkyStar>& Stars);
}
