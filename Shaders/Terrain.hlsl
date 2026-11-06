// The ground and all that stands on it, from one kind of vertex. The FOLIAGE permutation draws
// cards with holes in them, cut by the alpha of their textures, and lit from both sides.
#include "Lighting.hlsli"
#include "Surfaces.hlsli"

struct ChunkConstants
{
	float4 Offset; // chunk origin relative to the camera
};

[[vk::push_constant]] ConstantBuffer<ChunkConstants> Chunk : register(b2);

struct VertexInput
{
	float3 Position : TEXCOORD0;
	float4 Normal : TEXCOORD1; // w says which material this is, see ETerrainMaterial
	float4 Color : TEXCOORD2;  // alpha is how far the wind bends this vertex
};

struct Varyings
{
	float4 Clip : SV_Position;
	float3 Relative : TEXCOORD0;
	float3 Normal : TEXCOORD1;
	float3 Albedo : TEXCOORD2;
	float ViewDepth : TEXCOORD3;
	float Material : TEXCOORD4;
	float Lift : TEXCOORD5; // how far the wind can have moved this point
};

static const float WindBend = 0.075;

Varyings VSMain(VertexInput Input)
{
	float3 Relative = Input.Position + Chunk.Offset.xyz;

	if (Input.Color.a > 0.0)
	{
		const float2 World = Relative.xy + CameraPosition.xy;
		const float Time = CameraPosition.w;
		const float Gust = 0.65 + 0.35 * sin(Time * 1.9 + dot(World, float2(0.21, 0.13)));
		Relative.xy += Wind.xy * WindBend * Input.Color.a * Gust;

		// Leaves shiver by themselves, faster than the boughs they hang on.
		if (Input.Normal.w < 0.19)
		{
			const float Flutter = sin(Time * 7.0 + dot(Input.Position, float3(3.1, 2.3, 1.7)));
			Relative += float3(0.5, 0.4, 0.3) * Flutter * Input.Color.a * 0.035 * (0.4 + length(Wind.xy) * 0.2);
		}

		// And what has been knocked by something passing through swings back.
		for (int Index = 0; Index < int(Conditions.w); ++Index)
		{
			const float3 Away = Relative - PlantPoints[Index].xyz;
			const float Reach = PlantPoints[Index].w;
			const float Share = saturate(1.0 - dot(Away, Away) / (Reach * Reach));
			Relative += PlantMoves[Index].xyz * (Share * Share);
		}
	}

	Varyings Output;
	Output.Clip = mul(PassViewProjection, float4(Relative, 1.0));
	Output.Relative = Relative;
	Output.Normal = Input.Normal.xyz;
	Output.Albedo = Input.Color.rgb;
	Output.ViewDepth = Output.Clip.w;
	Output.Material = Input.Normal.w;
	Output.Lift = length(Wind.xy) * WindBend * Input.Color.a;
	return Output;
}

float4 PSMain(Varyings Input) : SV_Target
{
	const float3 World = float3(Input.Relative.xy + CameraPosition.xy, Input.Relative.z + CameraPosition.z);
	float3 Normal = normalize(Input.Normal);

	Footprint Pixel;
	Pixel.Across = ddx(Input.Relative);
	Pixel.Down = ddy(Input.Relative);
	Pixel.Wide = smoothstep(25.0, 180.0, Input.ViewDepth);
	Pixel.Card = 0.0;

#ifdef FOLIAGE
	const float Layer = Input.Material > 0.095 ? NeedleLayer : LeafLayer;
	Pixel.Card = SurfaceColors.CalculateLevelOfDetail(LinearWrap, Input.Albedo.rg) - 0.5;

	// Sampled finer than the pixel asks for, or at a distance a card's holes average away and it turns solid.
	clip(SurfaceColors.SampleLevel(LinearWrap, float3(Input.Albedo.rg, Layer), max(Pixel.Card - 1.5, 0.0)).a - 0.42);

	// What faces away from the eye shows its other side.
	if (dot(Normal, Input.Relative) > 0.0)
	{
		Normal = normalize(Normal * 0.4 + float3(0.0, 0.0, 0.4));
	}
#endif

	const Shaded Surface = ShadeMaterial(Input.Material, Input.Albedo, World, Normal, Pixel);
	Normal = normalize(Normal + Surface.Bend);
	float3 Color = ShadeSurface(Surface.Color, Normal, Input.Relative, Input.ViewDepth, Surface.Gloss, Input.Lift);

#ifdef FOLIAGE
	// Light comes through leaves when the eye looks towards a sun past them.
	const float3 Ray = normalize(Input.Relative);
	const float3 Through = SunColor.rgb * pow(saturate(dot(Ray, SunDirection.xyz)), 3.0) + Sun2Color.rgb * pow(saturate(dot(Ray, Sun2Direction.xyz)), 3.0);
	Color += Surface.Color * Through * 0.4;
#endif

	return float4(Color, 1.0);
}

#ifdef FOLIAGE
/** A leaf card casting a shadow into the cascades: only where it has leaves. */
void ShadowMain(Varyings Input)
{
	const float Layer = Input.Material > 0.095 ? NeedleLayer : LeafLayer;
	const float Level = SurfaceColors.CalculateLevelOfDetail(LinearWrap, Input.Albedo.rg);
	clip(SurfaceColors.SampleLevel(LinearWrap, float3(Input.Albedo.rg, Layer), Level).a - 0.42);
}
#endif
