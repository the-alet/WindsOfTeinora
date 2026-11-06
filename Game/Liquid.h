// Water as a liquid, around the camera: a patch of the shallow water equations laid over
// whatever seas and lakes are there. Volume pushed in by bodies runs off as ripples, at the
// speed the depth allows, and they break on the shore, slow in the shallows and bend round points.
#pragma once

#include "Core/Math.h"

#include <cstdint>
#include <vector>

namespace Teinora
{
	class LiquidField
	{
	public:
		static constexpr int Side = 256;       // cells along each edge of the patch
		static constexpr double Cell = 0.5;    // metres

		explicit LiquidField(std::uint32_t Seed);

		/** Keeps the patch around a point. It moves in whole cells, and what comes into it starts calm. */
		void Follow(const Vec3& Eye);

		/** Adds this many cubic metres of water around a point, or takes it away if negative. */
		void Displace(double X, double Y, double Volume, double Radius);

		void Advance(double Dt);

		/** How far the surface stands above its rest at a point. Nothing happens away from the patch. */
		double GetHeight(double X, double Y) const;

		/** How the water moves at a point. */
		Vec3 GetFlow(double X, double Y) const;

		/** All the water that has been moved in the patch, in cubic metres, for checking the books. */
		double GetExtraVolume() const;

		double GetCornerX() const { return CornerX; }
		double GetCornerY() const { return CornerY; }
		double GetSpan() const { return Side * Cell; }

		/** Two floats per cell, row by row: height over rest, and foam from 0 to 1. */
		const float* GetPixels() const { return Pixels.data(); }

		/** Whether anything is moving, so that a calm patch need not be simulated or sent to the screen. */
		bool IsStirred() const { return bStirred; }

	private:
		void FillDepth(int FirstX, int FirstY, int LastX, int LastY);
		void Slide(int CellsX, int CellsY);
		void Step(double Dt);

		std::size_t At(int X, int Y) const { return static_cast<std::size_t>(X) + static_cast<std::size_t>(Y) * Side; }

		std::uint32_t Seed;
		double CornerX = 0.0;
		double CornerY = 0.0;
		bool bPlaced = false;
		bool bStirred = false;
		double Accumulator = 0.0;

		std::vector<float> Depth;   // of still water; zero on land
		std::vector<float> Height;  // over the rest level of the water at that cell
		std::vector<float> FlowX;   // across the cell's left face
		std::vector<float> FlowY;   // across its lower face
		std::vector<float> Foam;
		std::vector<float> Pixels;
	};
}
