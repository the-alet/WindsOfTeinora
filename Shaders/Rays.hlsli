// Ray queries against the scene the renderer builds every frame, relative to the camera.
// Only the RAY_TRACED shader permutations include this: they need shader model 6.5.
#ifndef TEINORA_RAYS_HLSLI
#define TEINORA_RAYS_HLSLI

#include "Common.hlsli"
#include "Surfaces.hlsli"

[[vk::binding(0, 2)]] RaytracingAccelerationStructure Scene : register(t6);
[[vk::binding(1, 2)]] ByteAddressBuffer SceneVertices[2048] : register(t7); // Rhi::MaxRayBuffers

static const float RayReach = 8000.0;
static const uint ChunkStride = 20; // sizeof(TerrainVertex)

float4 UnpackSigned(uint Packed)
{
	return float4(asint(uint4(Packed << 24, Packed << 16, Packed << 8, Packed)) >> 24) / 127.0;
}

float4 UnpackUnsigned(uint Packed)
{
	return float4(uint4(Packed, Packed >> 8, Packed >> 16, Packed >> 24) & 255u) / 255.0;
}

/** Whether a ray that has reached a leaf card meets a leaf there, or goes through one of its holes. */
bool IsCardSolid(uint Instance, uint Primitive, float2 Barycentrics)
{
	const float3 Weights = float3(1.0 - Barycentrics.x - Barycentrics.y, Barycentrics.x, Barycentrics.y);
	const uint Buffer = NonUniformResourceIndex(Instance);
	const uint First = Primitive * 3;

	float2 Place = float2(0.0, 0.0);
	for (uint Corner = 0; Corner < 3; ++Corner)
	{
		Place += UnpackUnsigned(SceneVertices[Buffer].Load((First + Corner) * ChunkStride + 16)).rg * Weights[Corner];
	}
	const float Material = UnpackSigned(SceneVertices[Buffer].Load(First * ChunkStride + 12)).w;
	const float Layer = Material > 0.095 ? NeedleLayer : LeafLayer;
	return SurfaceColors.SampleLevel(LinearWrap, float3(Place, Layer), 3.0).a > 0.42;
}

/** 1 when nothing stands between the point and the far end of the ray, otherwise 0. */
float TraceLight(float3 Origin, float3 Direction, float Reach)
{
	RayDesc Ray;
	Ray.Origin = Origin;
	Ray.Direction = Direction;
	Ray.TMin = 0.0;
	Ray.TMax = Reach;

	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> Query;
	Query.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, Ray);
	while (Query.Proceed())
	{
		if (Query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE
			&& IsCardSolid(Query.CandidateInstanceID(), Query.CandidatePrimitiveIndex(), Query.CandidateTriangleBarycentrics()))
		{
			Query.CommitNonOpaqueTriangleHit();
		}
	}
	return Query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

#endif
