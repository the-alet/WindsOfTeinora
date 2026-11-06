// The standalone renderer's side of the terrain: chunk meshes in GPU buffers.
#pragma once

#include "Render/Renderer.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Teinora
{
	class ChunkBuffers final : public IChunkDisplay
	{
	public:
		explicit ChunkBuffers(Rhi::Device& InDevice) : Device(InDevice) {}

		void ShowChunk(int X, int Y, const ChunkMesh& Mesh) override;
		void HideChunk(int X, int Y) override;

		const std::vector<ChunkDraw>& GetDraws();

	private:
		void Release(ChunkDraw& Draw);

		Rhi::Device& Device;
		std::unordered_map<std::uint64_t, ChunkDraw> Chunks;
		std::vector<ChunkDraw> Draws;
		bool bDrawsDirty = false;
	};
}
