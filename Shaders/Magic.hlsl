// Follows each pixel's ray through the magic in front of what it hits.
//
// PSMain makes the haze of the magic view: its light in RGB, and in alpha how much of what lies
// behind it covers. BendMain finds where dense magic bends light in plain sight: red is the
// optical path the swirls add to the ray, and green how much dense magic the ray went through.
// Tonemap.hlsl makes a picture of both.
#include "Clouds.hlsli"

struct MagicConstants
{
	float4 NearCorner; // low corner of the grid around the camera, relative to the camera
	float4 NearSize;
	float4 FarCorner;  // and of the coarse box beyond it
	float4 FarSize;
};

[[vk::push_constant]] ConstantBuffer<MagicConstants> Magic : register(b2);

[[vk::binding(2, 1)]] Texture3D<float4> NearVolume : register(t2);
[[vk::binding(3, 1)]] Texture3D<float4> FarVolume : register(t3);

static const int Steps = 48;
static const float FirstStep = 1.5;
static const float Reach = 3500.0;
static const float Growth = 1.1771; // (Reach / FirstStep)^(1 / Steps)

// The magic view sees only so far around the eye, though magic goes on beyond it.
static const float SightRadius = 100.0; // Sim::MagicSightRadius
static const float SightFade = 70.0;    // where it starts to give out
static const int SightSteps = 32;
static const float SightGrowth = 1.1402; // (SightRadius / FirstStep)^(1 / SightSteps)

static const float DensityScale = 120.0;  // MagicVolume::DensityScale
static const float DewPoint = 30.0;       // Sim::MagicDewPoint, where magic shows and bends light
static const float BendRamp = 13.6;       // kg/m3 over that for it to bend fully
static const float ThickDensity = 0.8;    // thinner than this, the haze has no structure worth drawing
static const float Extinction = 0.0035;   // square metres per kilogram of thin magic
static const float Crowding = 20.0;       // kg/m3 at which the dense hides only half as much more again

/** How much a density hides, per metre: in proportion while thin, less and less so when dense, so that the dense cores
 * of wells and bolts do not black out what is behind them. */
float Hiding(float Density)
{
	return Density * Extinction / (1.0 + Density / Crowding);
}

struct Varyings
{
	float4 Clip : SV_Position;
	float2 Screen : TEXCOORD0;
	float2 Uv : TEXCOORD1;
};

/** What a volume holds at a point: density, then its colour multiplied by that, which blends properly. */
float4 ReadVolume(Texture3D<float4> Volume, float3 Uv)
{
	const float4 Texel = Volume.SampleLevel(LinearClamp, Uv, 0.0);
	const float Density = Texel.r * Texel.r * DensityScale;
	return float4(Density, Texel.gba * Density);
}

float4 SampleMagic(float3 Relative)
{
	const float3 NearUv = (Relative - Magic.NearCorner.xyz) / Magic.NearSize.xyz;
	const float3 FromEdge = min(NearUv, 1.0 - NearUv);
	const float Inside = saturate(min(FromEdge.x, min(FromEdge.y, FromEdge.z)) * 14.0);

	float4 Result = float4(0.0, 0.0, 0.0, 0.0);
	if (Inside > 0.0)
	{
		Result = ReadVolume(NearVolume, NearUv) * Inside;
	}
	if (Inside < 1.0)
	{
		// Past the coarse box there is nothing known about magic, so nothing is drawn of it.
		const float3 FarUv = (Relative - Magic.FarCorner.xyz) / Magic.FarSize.xyz;
		if (all(FarUv >= 0.0) && all(FarUv <= 1.0))
		{
			Result += ReadVolume(FarVolume, FarUv) * (1.0 - Inside);
		}
	}
	return Result;
}

// Curls smaller than the grid's cells can hold, slowly turning over where they are. Magic weighs nothing,
// so they drift no way in particular: only the warp that twists them changes with time.
float Curls(float3 World, float Scale)
{
	const float3 Point = World * Scale;
	const float Turning = CameraPosition.w * 0.05;
	const float2 Warp = float2(Noise(Point.xy + 5.1 + float2(Turning, -Turning * 0.7)), Noise(Point.yz + 1.7 + float2(-Turning * 0.8, Turning * 0.6))) - 0.5;
	const float Value = Noise(Point.zx * 2.1 + Warp * 7.0);
	return 0.45 + 1.1 * Value * Value;
}

float3 ToWorld(float3 Relative)
{
	return float3(Relative.xy + CameraPosition.xy, Relative.z + CameraPosition.z);
}

/** The direction of a pixel's ray, and how far it can go before something stops it. */
float3 GetRay(Varyings Input, out float Distance)
{
	const float4 Aim = mul(InverseViewProjection, float4(Input.Screen, 0.5, 1.0));

	Distance = Reach;
	const float Depth = Texture0.SampleLevel(PointClamp, Input.Uv, 0.0).r;
	if (Depth > 0.0)
	{
		const float4 Hit = mul(InverseViewProjection, float4(Input.Screen, Depth, 1.0));
		Distance = min(length(Hit.xyz / Hit.w), Reach);
	}
	return normalize(Aim.xyz / Aim.w);
}

float4 PSMain(Varyings Input) : SV_Target
{
	// Magic glows through whatever is in the way, and so does what is inside it: the view looks right through
	// to the edge of its sight.
	float Distance;
	const float3 Direction = GetRay(Input, Distance);
	Distance = SightRadius;

	float Near = FirstStep;
	float Clear = 1.0;           // how much of what lies behind still gets through
	float3 Glow = float3(0.0, 0.0, 0.0); // the light of the haze, nearest first
	for (int Step = 0; Step < SightSteps && Near < Distance; ++Step)
	{
		const float Far = min(Near * SightGrowth, Distance);
		const float Span = Far - Near;
		const float3 Relative = Direction * (0.5 * (Near + Far));
		const float4 Sampled = SampleMagic(Relative);
		const float Density = Sampled.x;

		// Close up the fine curls show, further out only the broad ones can.
		float Swirl = 1.0;
		if (Density > ThickDensity)
		{
			const float3 World = ToWorld(Relative);
			const float Close = 1.0 - smoothstep(30.0, 140.0, Near);
			Swirl = Curls(World, 0.045);
			if (Close > 0.0)
			{
				Swirl *= lerp(1.0, Curls(World, 0.21), Close);
			}
		}

		if (Density > 1e-4)
		{
			const float Seen = 1.0 - smoothstep(SightFade, SightRadius, Near);
			const float Taken = (1.0 - exp(-Hiding(Density) * Span)) * Seen;
			Glow += Sampled.yzw / Density * Clear * Taken * (0.35 + 0.65 * Swirl);
			Clear *= 1.0 - Taken;
		}
		Near = Far;
	}

	// The magic held inside things glows through them as well, as balls of their aspect's colour, brightest
	// where the ray goes through most of one.
	for (int Index = 0; Index < 8; ++Index)
	{
		const float Radius = GlowPoints[Index].w;
		const float Along = dot(GlowPoints[Index].xyz, Direction);
		const float Off = dot(GlowPoints[Index].xyz, GlowPoints[Index].xyz) - Along * Along;
		if (Radius > 0.0 && Along > 0.0 && Along < SightRadius && Off < Radius * Radius)
		{
			const float Through = sqrt(1.0 - Off / (Radius * Radius));
			Glow += GlowColors[Index].rgb * (Through * Through) * 2.5;
		}
	}

	return float4(Glow, 1.0 - Clear);
}

float4 BendMain(Varyings Input) : SV_Target
{
	float Distance;
	const float3 Direction = GetRay(Input, Distance);

	float Near = FirstStep;
	float Path = 0.0;
	float Dense = 0.0;
	for (int Step = 0; Step < Steps && Near < Distance; ++Step)
	{
		const float Far = min(Near * Growth, Distance);
		const float Span = Far - Near;
		const float3 Relative = Direction * (0.5 * (Near + Far));
		const float4 Sampled = SampleMagic(Relative);

		// Only the swirls count: a smooth cloud would act as one big lens.
		const float Excess = saturate((Sampled.x - DewPoint) / BendRamp);
		if (Excess > 0.0)
		{
			Path += Excess * (Curls(ToWorld(Relative), 0.09) - 1.0) * Span;
			Dense += Excess * Span;
		}
		Near = Far;
	}

	return float4(Path * 0.06, Dense * 0.03, 0.0, 0.0);
}
