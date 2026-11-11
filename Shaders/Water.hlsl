#include "Lighting.hlsli"
#include "Surfaces.hlsli"
#include "Flame.hlsli"

#ifdef RAY_TRACED
#include "Clouds.hlsli"
#include "Planet.hlsli"

// These mirror what Renderer::BuildInstances packs into an instance's index.
static const uint BodyInstance = 1u << 23;
static const uint BodyStride = 16;  // sizeof(MeshVertex)
static const float ReflectedDepth = 1000.0; // far enough that a reflection is spared the fine shading

float3 TraceReflection(float3 Origin, float3 Direction, out float Reach)
{
	Reach = 1e9;
	RayDesc Ray;
	Ray.Origin = Origin;
	Ray.Direction = Direction;
	Ray.TMin = 0.0;
	Ray.TMax = RayReach;

	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> Query;
	Query.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, Ray);
	while (Query.Proceed())
	{
		if (Query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE
			&& IsCardSolid(Query.CandidateInstanceID(), Query.CandidatePrimitiveIndex(), Query.CandidateTriangleBarycentrics()))
		{
			Query.CommitNonOpaqueTriangleHit();
		}
	}
	if (Query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
	{
		return AddClouds(AddPlanet(SkyColor(Direction), Direction), Direction, 10, 0.5);
	}

	const float Distance = Query.CommittedRayT();
	Reach = Distance;
	const float3 Hit = Origin + Direction * Distance;
	const float2 Barycentrics = Query.CommittedTriangleBarycentrics();
	const float3 Weights = float3(1.0 - Barycentrics.x - Barycentrics.y, Barycentrics.x, Barycentrics.y);
	const uint Instance = Query.CommittedInstanceID();
	const uint First = Query.CommittedPrimitiveIndex() * 3;

	if ((Instance & BodyInstance) != 0)
	{
		const uint Mesh = NonUniformResourceIndex((Instance >> 20) & 7u);
		float3 Local = float3(0.0, 0.0, 0.0);
		for (uint Corner = 0; Corner < 3; ++Corner)
		{
			Local += UnpackSigned(SceneVertices[Mesh].Load((First + Corner) * BodyStride + 12)).xyz * Weights[Corner];
		}

		const float3 Normal = normalize(mul(Local, (float3x3)Query.CommittedWorldToObject3x4()));
		const float3 Albedo = float3((Instance >> 13) & 127u, (Instance >> 6) & 127u, (Instance & 63u) * 2u) / 127.0;
		return ShadeSurface(Albedo, Normal, Hit, ReflectedDepth, 0.1, 0.0);
	}

	const uint Chunk = NonUniformResourceIndex(Instance);
	float4 Vertex = float4(0.0, 0.0, 0.0, 0.0);
	float3 Tint = float3(0.0, 0.0, 0.0);
	for (uint Corner = 0; Corner < 3; ++Corner)
	{
		const uint Offset = (First + Corner) * ChunkStride;
		Vertex += UnpackSigned(SceneVertices[Chunk].Load(Offset + 12)) * Weights[Corner];
		Tint += UnpackUnsigned(SceneVertices[Chunk].Load(Offset + 16)).rgb * Weights[Corner];
	}

	// A reflection needs no more texture detail than a pixel of the mirror image covers.
	Footprint Pixel;
	Pixel.Across = float3(Distance * 0.004, 0.0, 0.0);
	Pixel.Down = float3(0.0, Distance * 0.004, 0.0);
	Pixel.Wide = 1.0;
	Pixel.Card = 5.0;

	const float3 World = float3(Hit.xy + CameraPosition.xy, Hit.z + CameraPosition.z);
	const float3 Smooth = normalize(Vertex.xyz);
	const Shaded Surface = ShadeMaterial(Vertex.w, Tint, World, Smooth, Pixel);
	return ShadeSurface(Surface.Color, normalize(Smooth + Surface.Bend), Hit, ReflectedDepth, 0.0, 0.0, Surface.Glow);
}
#endif

// One body of water, drawn through the grid that follows the camera.
struct WaterConstants
{
	float4 Body;  // x height of its surface, y share of the sea's waves, z its radius, 0 for the sea
	float4 Shape; // xy its middle relative to the camera, z how much longer than wide, w the way its long side lies
};

[[vk::push_constant]] ConstantBuffer<WaterConstants> Water : register(b2);

// The patch of moving water around the camera: height in red, foam in green.
[[vk::binding(4, 1)]] Texture2D<float4> LiquidMap : register(t4);

// What lies behind the surface, drawn before it.
[[vk::binding(5, 1)]] Texture2D<float4> SceneBehind : register(t5);

static const float WaterEdge = 1.3; // Sim::Lake::WaterEdge

struct Varyings
{
	float4 Clip : SV_Position;
	float3 Relative : TEXCOORD0;
	float ViewDepth : TEXCOORD1;
	float Edge : TEXCOORD2;      // how far inside the shape that holds the water
};

/** The flames that a ray off the water meets before Reach: the same volumes the eye sees, traced from the water. */
float3 ReflectFlames(float3 Origin, float3 Direction, float Reach)
{
	float3 Light = float3(0.0, 0.0, 0.0);
	for (int Index = 0; Index < 8 && HotLights[Index].w > 0.0; ++Index)
	{
		const float Height = HotPoints[Index].w;
		if (Height > 0.0)
		{
			const float3 Foot = HotPoints[Index].xyz - float3(0.0, 0.0, Height * 0.33);
			Light += MarchFlame(Origin - Foot, Direction, Reach, Height, HotShapes[Index].x, Index * 37.0, CameraPosition.w, 0.5).rgb;
		}
	}
	return Light;
}

/** How much of the patch of moving water a point has, which fades out towards the patch's edge. */
float2 GetLiquidPlace(float2 Relative)
{
	return (Relative - LiquidArea.xy) * LiquidArea.z;
}

float GetLiquidHeight(float2 Relative)
{
	const float2 Place = GetLiquidPlace(Relative);
	if (LiquidArea.w <= 0.0 || any(Place < 0.0) || any(Place > 1.0))
	{
		return 0.0;
	}
	return LiquidMap.SampleLevel(LinearClamp, Place, 0.0).r;
}

// The grid follows the camera. The depth of the scene behind the water arrives in Texture0.
Varyings VSMain(float2 Position : TEXCOORD0)
{
	const float2 World = Position + CameraPosition.xy;
	const float Waves = Water.Body.y > 0.0 ? WaterHeight(World, CameraPosition.w) * Water.Body.y : 0.0;
	const float Height = Water.Body.x + Waves + GetLiquidHeight(Position);
	const float3 Relative = float3(Position, Height - CameraPosition.z);

	Varyings Output;
	Output.Clip = mul(ViewProjection, float4(Relative, 1.0));
	Output.Relative = Relative;
	Output.ViewDepth = Output.Clip.w;
	Output.Edge = 1.0;
	if (Water.Body.z > 0.0)
	{
		const float2 Off = Position - Water.Shape.xy;
		const float2 Way = float2(cos(Water.Shape.w), sin(Water.Shape.w));
		const float Along = dot(Off, Way) / Water.Shape.z;
		const float Across = Off.y * Way.x - Off.x * Way.y;
		Output.Edge = WaterEdge - sqrt(Along * Along + Across * Across) / Water.Body.z;
	}
	return Output;
}

/** The slope of the moving water at a point, and in w its foam. */
float3 GetLiquidSurface(float2 Relative)
{
	const float2 Place = GetLiquidPlace(Relative);
	if (LiquidArea.w <= 0.0 || any(Place < 0.0) || any(Place > 1.0))
	{
		return float3(0.0, 0.0, 0.0);
	}

	const float Step = LiquidArea.z * 0.75;
	const float Left = LiquidMap.SampleLevel(LinearClamp, Place - float2(Step, 0.0), 0.0).r;
	const float Right = LiquidMap.SampleLevel(LinearClamp, Place + float2(Step, 0.0), 0.0).r;
	const float Down = LiquidMap.SampleLevel(LinearClamp, Place - float2(0.0, Step), 0.0).r;
	const float Up = LiquidMap.SampleLevel(LinearClamp, Place + float2(0.0, Step), 0.0).r;
	return float3((Right - Left) / 1.5, (Up - Down) / 1.5, LiquidMap.SampleLevel(LinearClamp, Place, 0.0).g);
}

float4 PSMain(Varyings Input) : SV_Target
{
	clip(Input.Edge);

	const float2 World = Input.Relative.xy + CameraPosition.xy;
	const float3 View = normalize(-Input.Relative);
	const bool bFromBelow = Input.Relative.z > 0.0;
	const float WaveShare = Water.Body.y;

	// Ripples too small for the wave mesh, two soft textures sliding across each other. A lake
	// has none of the sea's swell and only a breath of them.
	// Tile sizes divide the distance the camera position wraps at; mips calm them far away.
	const float2 RippleA = SurfaceNormals.Sample(LinearWrap, float3((World + Wind.zw * 0.35) / 16.0, SnowLayer)).xy * 2.0 - 1.0;
	const float2 RippleB = SurfaceNormals.Sample(LinearWrap, float3((World.yx - Wind.wz * 0.2) / 4.0, SnowLayer)).xy * 2.0 - 1.0;
	const float RippleSize = lerp(0.05, 0.5, WaveShare);
	const float3 Liquid = GetLiquidSurface(Input.Relative.xy);
	const float3 Swell = WaveShare > 0.0 ? WaterNormal(World, CameraPosition.w) : float3(0.0, 0.0, 1.0);
	const float3 Normal = normalize(Swell + float3((RippleA + RippleB) * RippleSize - Liquid.xy * 9.0, 0.0));

	const float3 Sunlight = SunColor.rgb * max(SunDirection.z, 0.0) + Sun2Color.rgb * max(Sun2Direction.z, 0.0);
	const float3 Ambient = SkyZenith.rgb + SkyHorizon.rgb * 0.3 + Sunlight * 0.25;

	if (bFromBelow)
	{
		// From under the surface the sky shows through a round window overhead. Past its edge
		// the surface is a mirror for the water itself.
		const float3 Bent = refract(-View, -Normal, 1.33);
		const float3 Mirror = float3(0.03, 0.17, 0.19) * Ambient;
		const float3 Sky = SkyColor(normalize(Bent));
		const float Inside = dot(Bent, Bent) > 0.0 ? 1.0 - pow(1.0 - saturate(dot(View, -Normal)), 5.0) : 0.0;
		return float4(lerp(Mirror, Sky, Inside), 1.0);
	}

	// Reversed depth stores near / z, so the view depth comes straight back.
	const float2 Uv = Input.Clip.xy * ScreenSize.zw;
	const float SceneDepth = Options.z / max(Texture0.SampleLevel(PointClamp, Uv, 0.0).r, 1e-7);
	const float Straight = max(SceneDepth - Input.ViewDepth, 0.0);

	// The surface bends what is seen through it, more the deeper that lies, but only what is really behind it.
	const float2 Bent = Uv + Normal.xy * saturate(Straight * 0.4) * 0.05 / max(Input.ViewDepth * 0.04, 1.0);
	const float BentDepth = Options.z / max(Texture0.SampleLevel(PointClamp, Bent, 0.0).r, 1e-7);
	const bool bBehind = BentDepth > Input.ViewDepth;
	const float2 Seen = bBehind ? Bent : Uv;
	const float Thickness = bBehind ? BentDepth - Input.ViewDepth : Straight;

	// Colour fades on the way through, red soonest, while the water adds a haze of its own.
	const float3 Passing = exp(-float3(0.36, 0.085, 0.06) * Thickness);
	const float3 Haze = lerp(float3(0.025, 0.15, 0.13), float3(0.02, 0.14, 0.2), WaveShare) * Ambient;
	const float3 Through = SceneBehind.SampleLevel(LinearClamp, Seen, 0.0).rgb * Passing + Haze * (1.0 - Passing);

	const float Facing = saturate(dot(Normal, View));
	const float Fresnel = 0.03 + 0.97 * pow(1.0 - Facing, 5.0);
	// Where a wave turns a reflection into the water, it bounces off the next one back up.
	float3 Bounce = reflect(-View, Normal);
	Bounce.z = max(abs(Bounce.z), 0.001);
	Bounce = normalize(Bounce);
	const float3 Origin = Input.Relative + float3(0.0, 0.0, 0.05);
#ifdef RAY_TRACED
	float Reach;
	const float3 Reflected = TraceReflection(Origin, Bounce, Reach) + ReflectFlames(Origin, Bounce, Reach);
#else
	const float3 Reflected = SkyColor(Bounce) + ReflectFlames(Origin, Bounce, 1e9);
#endif

	const float Sparkle = pow(saturate(dot(Normal, normalize(View + SunDirection.xyz))), 420.0) * 1.4;
	const float Sparkle2 = pow(saturate(dot(Normal, normalize(View + Sun2Direction.xyz))), 420.0) * 1.4;

	// Firelight on the water itself is faint: it shows mostly as the flames' own reflection. The magic view
	// sees no reflections at all, only the water.
	const float Mirrored = 1.0 - Weather.w;
	float3 Color = lerp(Through, Reflected, Fresnel * Mirrored) + (SunColor.rgb * Sparkle + Sun2Color.rgb * Sparkle2) * Mirrored + GetFirelight(Normal, Input.Relative) * 0.03;

	// Foam where waves run up the shore and where the water has been thrown about.
	const float Lapping = smoothstep(0.7, 0.0, Thickness) * (0.55 + 0.45 * sin(Thickness * 9.0 - CameraPosition.w * 1.3)) * WaveShare;
	const float Foam = saturate(Lapping + Liquid.z * 1.4);
	Color = lerp(Color, Ambient * 0.9 + (SunColor.rgb + Sun2Color.rgb) * 0.5, Foam * 0.6);

	const float Distance = length(Input.Relative);
	const float Density = Options.w;
	Color = lerp(Color, SkyColor(float3(-View.xy, 0.02)), 1.0 - exp(-Distance * Density));

	return float4(Color, saturate(Thickness * 7.0 + Fresnel));
}
