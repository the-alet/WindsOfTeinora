// The ground's textures, one layer per Sim::ESurface. They are drawn by code rather than
// stored as files, so both games use the same ones and no image is kept in the project.
#pragma once

#include <cstdint>
#include <vector>

namespace Teinora
{
	// After the layers of the ground: bark, and two cut-out cards of leaves and of needles, whose
	// alpha says what is there. These numbers are also in Shaders/Surfaces.hlsli.
	inline constexpr int BarkLayer = 4;
	inline constexpr int LeafLayer = 5;
	inline constexpr int NeedleLayer = 6;
	inline constexpr int GrassBladeLayer = 7; // a clump of blades for the grass cards

	struct SurfaceTextures
	{
		int Size = 0;   // width and height of the largest mip
		int Mips = 0;
		int Layers = 0;

		// Both RGBA8, packed as Rhi::Device::CreateTexture takes them.
		std::vector<std::uint8_t> Colors;  // linear albedo
		std::vector<std::uint8_t> Normals; // XYZ of the tangent-space normal in RGB, height in alpha
	};

	/** Size must be a power of two. Takes a moment, so it belongs in start-up. */
	SurfaceTextures MakeSurfaceTextures(int Size);

	/**
	 * A cube of tiling noise for the clouds, as RGBA8: billowing broad shapes in red, the finer
	 * ones that wear away their edges in green. Takes a moment as well.
	 */
	std::vector<std::uint8_t> MakeCloudNoise(int Size);
}
