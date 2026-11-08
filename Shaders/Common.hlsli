// Shared by every shader. The same source builds to DXIL and SPIR-V; the bindings
// follow the model described in Engine/Rhi/Rhi.h. FrameData mirrors the C++ struct.
#ifndef TEINORA_COMMON_HLSLI
#define TEINORA_COMMON_HLSLI

[[vk::binding(0, 0)]] cbuffer FrameData : register(b0)
{
	float4x4 ViewProjection;
	float4x4 InverseViewProjection;
	float4x4 ShadowMatrices[3];
	float4 CascadeEnds;    // view depth where each cascade ends; w is the shadow texel size, 0 when off
	float4 SunDirection;   // towards the yellow-orange sun
	float4 SunColor;
	float4 Sun2Direction;  // towards the blue-green sun
	float4 Sun2Color;
	float4 PlanetDirection;
	float4 PlanetColor;    // the light it sends down
	float4 PlanetAxis;     // w is how far it has turned
	float4 SkyZenith;
	float4 SkyHorizon;
	float4 CameraPosition; // xy wrapped to 4096 m, z real; w is the time in seconds
	float4 Wind;           // xy velocity, zw distance the air has travelled, wrapped to 4096 m
	float4 ScreenSize;     // render target size in xy, its inverse in zw
	float4 Options;        // x exposure, y clouds on, z near plane, w fog density
	float4 Detail;         // x scales texture gradients, finer when an upscaler adds the pixels back; y is which sun the cascades follow
	float4 StarPole;       // what the sphere of stars turns around; w is how far it has turned
	float4 Conditions;     // x how deep the camera is under water, y height of the waves as a share of the usual, z how much of the sky has cloud, w how many plants are moving
	float4 LiquidArea;     // xy corner of the patch of moving water, relative to the camera; z one over its span; w 1 when there is anything in it
	float4 PlantPoints[8]; // what has been moved of a plant, relative to the camera; w how far around that the move reaches
	float4 PlantMoves[8];  // and by how much
	float4 HotPoints[8];   // what burns near the camera, relative to it; w how tall its flames are
	float4 HotLights[8];   // the light that gives; w how far it reaches, 0 past the last of them
	float4 BoltFlash;      // where lightning is lighting up the clouds, relative to the camera; w how brightly
	float4 HotShapes[8];   // x how broad the flames of each are, as a share of their height
	float4 CloudDrift;     // xy how far the wind aloft has carried the clouds, wrapped to 4096 m; z how far they have changed, from 0 to 1
	float4 SkyShift;       // what to add to a direction from the sky's middle to see it from the camera, see SeenFrom in SceneTypes.h
	float4 Coronas[2];     // the light of each sun's corona, which the planet in front of a sun does not take away
};

[[vk::binding(1, 0)]] cbuffer PassData : register(b1)
{
	float4x4 PassViewProjection;
};

[[vk::binding(2, 0)]] SamplerState LinearClamp : register(s0);
[[vk::binding(3, 0)]] SamplerState LinearWrap : register(s1);
[[vk::binding(4, 0)]] SamplerState PointClamp : register(s2);
[[vk::binding(5, 0)]] SamplerComparisonState ShadowCompare : register(s3);

[[vk::binding(0, 1)]] Texture2D<float4> Texture0 : register(t0);
[[vk::binding(1, 1)]] Texture2D<float4> Texture1 : register(t1);

// The air is a shell around a round world, as thick as a fortieth of the world's radius.

/** How many times more air there is along a ray going up at this sine of elevation than straight up: from 1 to 9. */
float GetAirMass(float Up)
{
	const float Along = Up * 40.0;
	return sqrt(Along * Along + 81.0) - Along;
}

/** From 0 by day to 1 once the sky has gone dark enough for stars. */
float GetNight()
{
	return 1.0 - smoothstep(0.03, 0.25, dot(SkyZenith.rgb, float3(1.0, 1.0, 1.0)));
}

float3 SkyColor(float3 Direction)
{
	const float Up = saturate(Direction.z);
	float3 Color = lerp(SkyZenith.rgb, SkyHorizon.rgb, (GetAirMass(Up) - 1.0) * 0.125);

	const float SunAmount = saturate(dot(Direction, SunDirection.xyz));
	Color += SunColor.rgb * (pow(SunAmount, 6.0) * 0.04 + pow(SunAmount, 48.0) * 0.12);
	const float Sun2Amount = saturate(dot(Direction, Sun2Direction.xyz));
	Color += Sun2Color.rgb * (pow(Sun2Amount, 6.0) * 0.04 + pow(Sun2Amount, 48.0) * 0.12);

	// The air holds on to a little of the planet's light at night, more along the horizon and around the planet.
	const float Night = GetNight();
	if (Night > 0.0)
	{
		const float Toward = saturate(dot(Direction, PlanetDirection.xyz));
		Color += (PlanetColor.rgb * 0.08 + float3(0.004, 0.006, 0.012)) * (0.55 + 0.1 * GetAirMass(Up)) * (1.0 + 1.5 * pow(Toward, 6.0)) * Night;
	}

	// Below the horizon the sky fades into distant haze rather than a hard line.
	return lerp(Color, SkyHorizon.rgb * 0.75, saturate(-Direction.z * 6.0));
}

float3 ApplyFog(float3 Color, float3 Relative)
{
	const float Distance = length(Relative);
	const float3 Direction = Relative / max(Distance, 0.001);

	// Exponential height fog, thicker in the valleys.
	const float Height = max(CameraPosition.z + Relative.z * 0.5, 0.0);
	const float Density = Options.w * exp(-Height * 0.002) * (1.0 + GetNight()); // the night air shows more of what it holds
	const float Amount = 1.0 - exp(-Distance * Density);

	return lerp(Color, SkyColor(float3(Direction.xy, max(Direction.z, 0.02))), Amount);
}

// Must match Teinora::Sim::WaterHeight. A whole number of cycles fits in 4096 m,
// which is what makes the wrapped camera position seamless.
static const int WaveCount = 6;
static const float3 Waves[WaveCount] = {
	float3(150.0, 110.0, 0.30), float3(-90.0, 210.0, 0.20), float3(330.0, -240.0, 0.09),
	float3(61.0, -173.0, 0.14), float3(-437.0, -129.0, 0.05), float3(241.0, 563.0, 0.035) };
static const float WaveScale = 6.283185307 / 4096.0;

float WaterHeight(float2 Position, float Time)
{
	float Height = 0.0;
	for (int Index = 0; Index < WaveCount; ++Index)
	{
		const float Number = WaveScale * length(Waves[Index].xy);
		const float Phase = WaveScale * dot(Waves[Index].xy, Position) - sqrt(9.81 * Number) * Time;
		Height += Waves[Index].z * sin(Phase);
	}
	return Height * Conditions.y;
}

float3 WaterNormal(float2 Position, float Time)
{
	float2 Slope = float2(0.0, 0.0);
	for (int Index = 0; Index < WaveCount; ++Index)
	{
		const float Number = WaveScale * length(Waves[Index].xy);
		const float Phase = WaveScale * dot(Waves[Index].xy, Position) - sqrt(9.81 * Number) * Time;
		Slope += WaveScale * Waves[Index].xy * Waves[Index].z * cos(Phase);
	}
	return normalize(float3(-Slope * Conditions.y, 1.0));
}

#endif
