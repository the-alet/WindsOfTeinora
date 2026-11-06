#include "ChunkBuffers.h"

namespace Teinora
{
	namespace
	{
		std::uint64_t MakeKey(int X, int Y)
		{
			return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(X)) << 32) | static_cast<std::uint32_t>(Y);
		}
	}

	void ChunkBuffers::ShowChunk(int X, int Y, const ChunkMesh& Mesh)
	{
		ChunkDraw& Draw = Chunks[MakeKey(X, Y)];
		Release(Draw);

		Draw.Buffer = Device.CreateBuffer(Mesh.VertexCount * sizeof(TerrainVertex), Mesh.Vertices);
		if (Draw.Buffer && Device.SupportsRayTracing())
		{
			Draw.Geometry = Device.CreateAcceleration(Draw.Buffer, sizeof(TerrainVertex), Mesh.SolidVertexCount);

			const std::uint32_t LeafVertices = Mesh.ShadowVertexCount - Mesh.SolidVertexCount;
			if (LeafVertices > 0)
			{
				Draw.LeafBuffer = Device.CreateBuffer(LeafVertices * sizeof(TerrainVertex), Mesh.Vertices + Mesh.SolidVertexCount);
				Draw.LeafGeometry = Device.CreateAcceleration(Draw.LeafBuffer, sizeof(TerrainVertex), LeafVertices);
			}
		}
		Draw.VertexCount = Mesh.VertexCount;
		Draw.SolidVertexCount = Mesh.SolidVertexCount;
		Draw.ShadowVertexCount = Mesh.ShadowVertexCount;
		Draw.Origin = Mesh.Origin;
		Draw.Size = Mesh.Size;
		Draw.MinZ = Mesh.MinZ;
		Draw.MaxZ = Mesh.MaxZ;
		bDrawsDirty = true;
	}

	void ChunkBuffers::HideChunk(int X, int Y)
	{
		const auto Found = Chunks.find(MakeKey(X, Y));
		if (Found != Chunks.end())
		{
			Release(Found->second);
			Chunks.erase(Found);
			bDrawsDirty = true;
		}
	}

	void ChunkBuffers::Release(ChunkDraw& Draw)
	{
		if (Draw.Buffer)
		{
			Device.DestroyBuffer(Draw.Buffer);
		}
		if (Draw.Geometry)
		{
			Device.DestroyAcceleration(Draw.Geometry);
		}
		if (Draw.LeafBuffer)
		{
			Device.DestroyBuffer(Draw.LeafBuffer);
		}
		if (Draw.LeafGeometry)
		{
			Device.DestroyAcceleration(Draw.LeafGeometry);
		}
		Draw = {};
	}

	const std::vector<ChunkDraw>& ChunkBuffers::GetDraws()
	{
		if (bDrawsDirty)
		{
			bDrawsDirty = false;
			Draws.clear();
			for (const auto& [Key, Draw] : Chunks)
			{
				Draws.push_back(Draw);
			}
		}
		return Draws;
	}
}
