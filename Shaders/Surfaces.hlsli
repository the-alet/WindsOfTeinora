// The ground's look: textures chosen by height and slope, for pixels of the terrain
// itself and for what rays hit. Their thresholds mirror Teinora::Sim::ClassifySurface.
#ifndef TEINORA_SURFACES_HLSLI
#define TEINORA_SURFACES_HLSLI

#include "Common.hlsli"

// One layer per surface, in the order of Teinora::Sim::ESurface.
[[vk::binding(2, 1)]] Texture2DArray<float4> SurfaceColors : register(t2);
[[vk::binding(3, 1)]] Texture2DArray<float4> SurfaceNormals : register(t3);

static const float SandLayer = 0.0;
static const float GrassLayer = 1.0;
static const float RockLayer = 2.0;
static const float SnowLayer = 3.0;
static const float BarkLayer = 4.0;
static const float LeafLayer = 5.0;
static const float NeedleLayer = 6.0;

// Whole fractions of 4096 m, the distance the camera position wraps at in the shaders.
static const float TileSize = 8.0;
static const float WideTileSize = 64.0;

struct Footprint
{
	float3 Across; // how far the world position moves per pixel, worked out by the caller
	float3 Down;   // because derivatives are not reliable inside the branches below
	float Wide;    // how much of the look comes from the larger tile
	float Card;    // which mip of a leaf card this pixel covers
};

struct Layer
{
	float3 Color;
	float2 Slope;
};

struct Shaded
{
	float3 Color;
	float3 Bend; // what the normal map adds to the world-space normal
	float Gloss;
	float3 Glow; // the light it gives off by itself
};

// Two sizes of one texture: the small tile gives close detail, the large one hides the repeat.
// The large one only keeps broad shapes, because what looks like a blade of grass in the small
// tile would look like a plank in it.
Layer SampleLayer(float Index, float2 Position, float2 Across, float2 Down, float Wide)
{
	const float2 StepX = Across * Detail.x / TileSize;
	const float2 StepY = Down * Detail.x / TileSize;
	const float3 Close = float3(Position / TileSize, Index);
	const float3 Large = float3(Position / WideTileSize, Index);

	float Width, Height, Layers;
	SurfaceColors.GetDimensions(Width, Height, Layers);
	const float Texels = sqrt(max(dot(StepX, StepX), dot(StepY, StepY))) * Width * TileSize / WideTileSize;
	const float LargeLevel = max(log2(max(Texels, 0.001)), 4.5);

	Layer Result;
	Result.Color = lerp(SurfaceColors.SampleGrad(LinearWrap, Close, StepX, StepY).rgb, SurfaceColors.SampleLevel(LinearWrap, Large, LargeLevel).rgb, 0.25 + 0.6 * Wide);
	Result.Slope = SurfaceNormals.SampleGrad(LinearWrap, Close, StepX, StepY).xy * 2.0 - 1.0;
	Result.Slope += (SurfaceNormals.SampleLevel(LinearWrap, Large, LargeLevel).xy * 2.0 - 1.0) * 1.5;
	return Result;
}

void AddLayer(inout Shaded Result, float Index, float3 World, Footprint Pixel, float Weight, float Gloss)
{
	const Layer Sampled = SampleLayer(Index, World.xy, Pixel.Across.xy, Pixel.Down.xy, Pixel.Wide);
	Result.Color += Sampled.Color * Weight;
	Result.Bend.xy += Sampled.Slope * Weight;
	Result.Gloss += Gloss * Weight;
}

// Steep faces would smear a texture laid on from above, so it is projected along each axis.
void AddProjected(inout Shaded Result, float Index, float3 World, float3 Normal, Footprint Pixel, float Weight, float Gloss)
{
	float3 Mix = pow(abs(Normal), 4.0);
	Mix /= Mix.x + Mix.y + Mix.z;

	const Layer Top = SampleLayer(Index, World.xy, Pixel.Across.xy, Pixel.Down.xy, Pixel.Wide);
	const Layer Across = SampleLayer(Index, World.yz, Pixel.Across.yz, Pixel.Down.yz, Pixel.Wide);
	const Layer Along = SampleLayer(Index, World.xz, Pixel.Across.xz, Pixel.Down.xz, Pixel.Wide);

	Result.Color += (Top.Color * Mix.z + Across.Color * Mix.x + Along.Color * Mix.y) * Weight;
	Result.Bend += (float3(Top.Slope, 0.0) * Mix.z + float3(0.0, Across.Slope) * Mix.x + float3(Along.Slope.x, 0.0, Along.Slope.y) * Mix.y) * Weight;
	Result.Gloss += Gloss * Weight;
}

/** Light gathered by the moving surface into a net of bright lines on what lies under it. */
float GetCaustics(float2 Position)
{
	const float Time = CameraPosition.w;
	const float A = SurfaceNormals.SampleLevel(LinearWrap, float3(Position / 7.0 + Time * float2(0.024, 0.017), SandLayer), 2.0).a;
	const float B = SurfaceNormals.SampleLevel(LinearWrap, float3(Position.yx / 4.0 - Time * float2(0.03, 0.021), SandLayer), 2.0).a;
	const float Line = saturate(1.0 - abs(A - B) * 7.0);
	return Line * Line * Line * Line;
}

Shaded ShadeGround(float3 World, float3 Normal, Footprint Pixel, float Brightness, float Lush, float Above)
{
	// Slow noise breaks up the borders between surfaces, which height and slope alone keep straight.
	const float Broad = SurfaceNormals.SampleLevel(LinearWrap, float3(World.xy / WideTileSize, RockLayer), 3.0).a;
	const float Waver = Broad - 0.5;

	const float Rock = 1.0 - smoothstep(0.70, 0.80, Normal.z + Waver * 0.08);
	const float Sand = (1.0 - smoothstep(0.8, 2.0, Above + Waver * 1.6)) * (1.0 - Rock);
	const float Snow = smoothstep(160.0, 182.0, World.z + Waver * 20.0) * (1.0 - Rock) * saturate(1.0 - Sand * 2.0) * smoothstep(0.0, 0.6, Above);
	const float Grass = saturate(1.0 - Rock - Sand - Snow);

	Shaded Result = (Shaded)0;
	if (Grass > 0.01)
	{
		AddLayer(Result, GrassLayer, World, Pixel, Grass, 0.03);
	}
	if (Sand > 0.01)
	{
		AddLayer(Result, SandLayer, World, Pixel, Sand, 0.05);
	}
	if (Snow > 0.01)
	{
		AddLayer(Result, SnowLayer, World, Pixel, Snow, 0.25);
	}
	if (Rock > 0.01)
	{
		AddProjected(Result, RockLayer, World, Normal, Pixel, Rock, 0.2);
	}

	const float3 Dry = float3(1.16, 1.0, 0.8);
	const float3 Green = float3(0.82, 1.0, 1.1);
	Result.Color *= lerp(float3(1.0, 1.0, 1.0), lerp(Dry, Green, Lush), Grass);
	Result.Color *= (0.8 + 0.4 * Broad) * Brightness;

	// Ground the water has been at stays dark and has a shine to it.
	const float Wet = 1.0 - smoothstep(0.1, 0.9, Above + Waver * 0.5);
	Result.Color *= lerp(1.0, 0.6, Wet);
	Result.Gloss += Wet * 0.25;

	// Under water, light moves over the bed, less and less of it the deeper.
	if (Above < 0.0)
	{
		Result.Color *= 1.0 + GetCaustics(World.xy) * 2.2 * exp(Above * 0.32) * saturate(-Above * 3.0);
	}
	return Result;
}

/** A card of leaves or needles. Red and green of a vertex are its coordinates on the card, blue how light its leaves are. */
Shaded ShadeCard(float Layer, float3 Tint, Footprint Pixel)
{
	const float3 Place = float3(Tint.rg, Layer);
	const float3 Tone = lerp(float3(0.72, 0.72, 0.74), float3(1.3, 1.12, 0.74), Tint.b);

	Shaded Result = (Shaded)0;
	const float3 Texel = SurfaceColors.SampleLevel(LinearWrap, Place, Pixel.Card).rgb;
	Result.Color = lerp(Texel, dot(Texel, float3(0.3, 0.6, 0.1)).xxx, 0.22) * Tone * 0.92;
	Result.Bend.xy = SurfaceNormals.SampleLevel(LinearWrap, Place, Pixel.Card).xy * 2.0 - 1.0;
	Result.Bend.xy *= 0.5;
	Result.Gloss = 0.06;
	return Result;
}

/** How near a place is to the edge of the plate of crust it lies on: the second nearest point of a scatter less the nearest. */
float GetCrustGap(float2 Place)
{
	const float2 Cell = floor(Place);
	float Nearest = 8.0;
	float Second = 8.0;
	for (int Y = -1; Y <= 1; ++Y)
	{
		for (int X = -1; X <= 1; ++X)
		{
			const float2 Home = Cell + float2(X, Y);
			const float2 Shift = frac(sin(float2(dot(Home, float2(127.1, 311.7)), dot(Home, float2(269.5, 183.3)))) * 43758.5453);
			const float Reach = length(Home + Shift - Place);
			Second = min(Second, max(Reach, Nearest));
			Nearest = min(Nearest, Reach);
		}
	}
	return Second - Nearest;
}

/** Molten rock under a crust: plates slowly drifting, light in the cracks between them, and more of it the hotter. */
float3 GetMoltenGlow(float2 Flat, float Heat)
{
	const float Time = CameraPosition.w;
	const float2 Drift = float2(sin(Time * 0.043), cos(Time * 0.031)) * 0.3;
	const float Plates = GetCrustGap(Flat * 0.5 + Drift);
	const float Splits = GetCrustGap(Flat * 2.0 - Drift * 2.0);

	const float Open = exp(-Plates * lerp(40.0, 7.0, Heat)) + 0.45 * exp(-Splits * lerp(40.0, 11.0, Heat));
	const float Pulse = 0.86 + 0.14 * sin(Time * 0.6 + dot(Flat, float2(0.31, 0.23)));
	const float Lit = saturate(Open * Pulse * smoothstep(0.0, 0.3, Heat));

	float3 Color = lerp(float3(0.9, 0.09, 0.01), float3(1.0, 0.36, 0.03), smoothstep(0.15, 0.6, Lit));
	Color = lerp(Color, float3(1.0, 0.72, 0.28), smoothstep(0.65, 1.0, Lit));
	return Color * Lit * Lit * 7.0;
}

// What a vertex's fourth normal component says it is made of, see ETerrainMaterial.
Shaded ShadeMaterial(float Material, float3 Tint, float3 World, float3 Normal, Footprint Pixel)
{
	Shaded Result = (Shaded)0;
	if (Material > 0.75)
	{
		Result = ShadeGround(World, Normal, Pixel, Tint.r * 2.0, Tint.g, Tint.b * 24.0 - 8.0); // EncodeGroundLevel, undone
	}
	else if (Material > 0.62)
	{
		AddProjected(Result, RockLayer, World, Normal, Pixel, 1.0, 0.2);
		Result.Color *= 0.12; // nearly black
		Result.Glow = GetMoltenGlow(World.xy, Tint.r);
	}
	else if (Material > 0.38)
	{
		AddProjected(Result, RockLayer, World, Normal, Pixel, 1.0, 0.2);
		Result.Color *= Tint * 4.5; // the rock texture averages about 0.22
	}
	else if (Material > 0.19)
	{
		AddProjected(Result, BarkLayer, World * float3(1.6, 1.6, 0.6), Normal, Pixel, 1.0, 0.04);
		Result.Color *= Tint * 4.5; // and the bark, about the same
	}
	else if (Material > 0.095)
	{
		Result = ShadeCard(NeedleLayer, Tint, Pixel);
	}
	else if (Material > 0.03)
	{
		Result = ShadeCard(LeafLayer, Tint, Pixel);
	}
	else
	{
		Result.Color = Tint;
	}
	return Result;
}

#endif
