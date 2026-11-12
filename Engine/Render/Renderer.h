// The frame: shadows, opaque scene, sky, water, particles, tone mapping, anti-aliasing
// and UI. Everything goes through Rhi::Device, so this file is the same on Direct3D 12
// and Vulkan. Lighting is direct sun and moon plus hemispheric ambient. Shadows come
// from cascades, or from rays on hardware that can trace them, which also gives the
// water true reflections.
#pragma once

#include "Core/Math.h"
#include "Platform/Window.h"
#include "Render/SceneTypes.h"
#include "Rhi/Rhi.h"

#include <cstdint>
#include <vector>

namespace Teinora
{
	struct ChunkDraw
	{
		Rhi::BufferHandle Buffer;
		Rhi::AccelerationHandle Geometry; // for rays, on hardware that traces them: the solid vertices
		Rhi::BufferHandle LeafBuffer;     // the same for the leaf cards, on their own, because rays test them leaf by leaf
		Rhi::AccelerationHandle LeafGeometry;
		std::uint32_t VertexCount = 0;
		std::uint32_t SolidVertexCount = 0;  // leading vertices that are whole triangles
		std::uint32_t ShadowVertexCount = 0; // leading vertices that cast shadows, which takes in the leaf cards; grass follows them
		Vec3 Origin;
		float Size = 0.0f;
		float MinZ = 0.0f;
		float MaxZ = 0.0f;
	};

	struct RenderSettings
	{
		int ShadowQuality = 2;  // 0 off, 1 low, 2 medium, 3 high
		int GeometryDetail = 1; // 0 low, 1 medium, 2 high, 3 ultra
		int RenderScale = 100;  // percent of the window resolution, when not upscaling
		int Upscaling = 0;      // a Rhi::EUpscaleMode
		int LowLatency = 0;     // a Rhi::ELowLatencyMode
		int RayTracing = 2;     // 0 off, 1 shadows, 2 shadows and reflections
		bool bEyeFocus = true;  // blurs the view away from the point of gaze
		bool bFxaa = true;
		bool bClouds = true;
	};

	/** A body of water to draw. */
	struct WaterDraw
	{
		double Level = 0.0;
		double WaveShare = 0.0; // of the sea's waves
		double Radius = 0.0;    // zero for one with no edge: the ground alone bounds it
		Vec3 Center;
		double Stretch = 1.0;
		double Angle = 0.0;
	};

	/** The map of clouds the renderer takes: cells to a side, the same as StormField::Side, and what its values go up to. Also in Shaders/Clouds.hlsli. */
	inline constexpr int CloudMapSide = 48;
	inline constexpr float CloudMapTop = 1.3f;

	struct SceneView
	{
		Vec3 CameraPosition;
		Vec3 CameraForward{ 0.0, 1.0, 0.0 };
		Vec3 GazeDirection{ 0.0, 1.0, 0.0 }; // where the eye looks, which may lead the camera
		double VerticalFov = 1.2;
		double Time = 0.0;
		double HourOfDay = 12.0;
		double DayCount = 0.0;    // whole days gone by, for what changes in the sky from day to day
		Vec3 WindVelocity;
		Vec3 WindOffset;          // how far the air has travelled so far
		Vec3 CloudDrift;          // how far the wind aloft has carried the clouds, and in Z how far they have changed
		double ViewDistance = 2500.0;
		const std::vector<ChunkDraw>* Chunks = nullptr;
		const std::vector<ObjectDraw>* Objects = nullptr;
		const std::vector<WaterDraw>* Waters = nullptr;
		const std::vector<PlantMove>* PlantMoves = nullptr;
		const std::vector<HotLight>* HotLights = nullptr;
		const std::vector<PuffDraw>* Puffs = nullptr;
		const std::vector<BoltDraw>* Bolts = nullptr;
		Vec3 FlashPoint;           // where lightning is lighting up the clouds
		double FlashStrength = 0.0;
		double CameraDepth = 0.0;  // how far under water the camera is
		double WaveHeight = 1.0;   // share of the usual height of the waves
		double CloudCover = 1.0;   // share of the usual amount of cloud
		double Rain = 0.0;         // how hard it rains, from 0 to 1
		double Storm = 0.0;        // how stormy the weather is, from 0 to 1
		double RainSlant = 0.0;    // how far the wind blows the rain across the view, as the wind across it over the rain's fall
		double Overcast = 0.0;     // how closed the sky around the camera is with cloud, from 0 to 1: it greys the light and the haze
		const std::vector<float>* CloudMap = nullptr; // how much cloud there is over each cell, CloudMapSide by CloudMapSide, X fastest; none for the usual cover everywhere
		double CloudMapCornerX = 0.0; // where the map's first cell's low corner is in the world
		double CloudMapCornerY = 0.0;
		double CloudMapCell = 0.0;    // metres across each cell
		double LiquidCornerX = 0.0; // the patch of moving water, see SetLiquid
		double LiquidCornerY = 0.0;
		double LiquidSpan = 0.0;
		float MagicView = 0.0f;   // from 0, plain sight, to 1: the world grey and dim, and each thing glowing with the magic it holds
		bool bMagicBends = false; // some magic nearby is dense enough to bend light
	};

	class Renderer
	{
	public:
		Renderer(Rhi::Device& InDevice, std::uint32_t WindowWidth, std::uint32_t WindowHeight, const RenderSettings& InSettings);
		~Renderer();

		Renderer(const Renderer&) = delete;
		Renderer& operator=(const Renderer&) = delete;

		void Resize(std::uint32_t WindowWidth, std::uint32_t WindowHeight);
		void ApplySettings(const RenderSettings& NewSettings);
		void Render(const SceneView& View);

		/** Gives the terrain its textures: colours and normals, as array textures with full mip chains. */
		void SetSurfaceTextures(std::uint32_t Size, std::uint32_t Mips, std::uint32_t Layers, const void* Colors, const void* Normals);

		/** Gives the sky its clouds: a cube of tiling RGBA8 noise, broad shapes in red and fine ones in green. */
		void SetCloudNoise(const void* Pixels, std::uint32_t Size);

		/** Gives the night sky its stars. */
		void SetStarMap(const SkyStar* Stars, std::uint32_t Count);

		/** Gives the water surfaces what is moving on them: Side by Side pairs of floats, height and foam. */
		void SetLiquid(const float* Pixels, std::uint32_t Side);

		/**
		 * Gives the renderer a box of magic as RGBA8, encoded as MagicVolume describes.
		 * Level 0 is the fine grid, 1 the coarse box around it.
		 */
		void SetMagicVolume(int Level, const void* Pixels, std::uint32_t Width, std::uint32_t Height, std::uint32_t Depth, const Vec3& Corner, const Vec3& Size);

		/** Triangles submitted during the last frame, shadow passes included. */
		std::uint64_t GetTriangleCount() const { return TriangleCount; }

		/** The upscaler at work, or null when the scene is drawn at its final size. */
		const char* GetUpscalerName() const { return IsUpscaling() ? Vendor->GetUpscalerName() : nullptr; }
		bool IsTracingRays() const;
		std::uint32_t GetRenderWidth() const { return TargetWidth; }
		std::uint32_t GetRenderHeight() const { return TargetHeight; }

		// 2D overlay in window pixels, drawn with the next Render call.
		void DrawRect(float X, float Y, float Width, float Height, const Float4& Color);
		void DrawText(float X, float Y, float Size, const char* Text, const Float4& Color);
		float MeasureText(float Size, const char* Text) const;

	private:
		struct FrameData;
		struct UiVertex
		{
			float Position[2];
			float Uv[2];
			std::uint8_t Color[4];
		};

		void CreatePipelines();
		void CreateMeshes();
		void CreateWaterMesh();
		void CreateFontTexture();
		void CreateSizedTargets();
		void DestroySizedTargets();
		bool IsUpscaling() const;
		void CreateShadowMap();
		void BuildInstances(const SceneView& View);
		void DrawChunks(const SceneView& View, const Float4x4& Matrix, bool bShadowPass, Rhi::PipelineHandle Solid, Rhi::PipelineHandle Leaves);
		void DrawObjects();
		void CopySceneBehindWater();
		void RenderShadows(const SceneView& View, const FrameData& Frame, const Float4x4* Cascades);
		void RenderStars(bool bDark);
		void RenderScene(const SceneView& View, const Float4x4& ViewProjection);
		void RenderTransparent(const SceneView& View);
		void DrawPuffs(const SceneView& View);
		void DrawBolts(const SceneView& View);
		void RenderUpscale(const FrameData& Frame, const Float4x4& Steady, const Float4x4& SteadyProjection, const SceneView& View, const float* Jitter, const Vec3* Axes, double Aspect);
		void RenderMagicPass(const SceneView& View, Rhi::TextureHandle Target, Rhi::PipelineHandle Pipeline);
		void RenderBlur();
		void RenderFinal(bool bMagicBends, const Float2& FocusCenter);

		Rhi::Device& Device;
		Rhi::VendorFeatures* Vendor = nullptr;
		RenderSettings Settings;
		std::uint32_t WindowWidth = 1;
		std::uint32_t WindowHeight = 1;
		std::uint32_t TargetWidth = 1;
		std::uint32_t TargetHeight = 1;
		std::uint32_t PostWidth = 1;   // after upscaling; the same as Target when there is none
		std::uint32_t PostHeight = 1;

		Rhi::PipelineHandle TerrainPipeline;
		Rhi::PipelineHandle TerrainRayPipeline;
		Rhi::PipelineHandle TerrainShadowPipeline;
		Rhi::PipelineHandle ObjectPipeline;
		Rhi::PipelineHandle ObjectRayPipeline;
		Rhi::PipelineHandle ObjectShadowPipeline;
		Rhi::PipelineHandle SkyPipeline;
		Rhi::PipelineHandle StarPipeline;
		Rhi::PipelineHandle WaterPipeline;
		Rhi::PipelineHandle WaterRayPipeline;
		Rhi::PipelineHandle FlamePipeline;
		Rhi::PipelineHandle PuffPipeline;
		Rhi::PipelineHandle BoltPipeline;
		Rhi::PipelineHandle FoliagePipeline;
		Rhi::PipelineHandle FoliageRayPipeline;
		Rhi::PipelineHandle FoliageShadowPipeline;
		Rhi::PipelineHandle UnderwaterPipeline;
		Rhi::PipelineHandle CopyPipeline;
		Rhi::PipelineHandle MagicBendPipeline;
		Rhi::PipelineHandle TonemapPipeline;
		Rhi::PipelineHandle BlurPipeline;
		Rhi::PipelineHandle MotionPipeline;
		Rhi::PipelineHandle FinalPipeline;
		Rhi::PipelineHandle UiPipeline;

		Rhi::TextureHandle SceneColor;
		Rhi::TextureHandle SceneDepth;
		Rhi::TextureHandle StarTarget;
		Rhi::TextureHandle ToneMapped;
		Rhi::TextureHandle SurfaceColors;
		Rhi::TextureHandle SurfaceNormals;
		Rhi::TextureHandle CloudNoise;
		Rhi::TextureHandle LiquidTexture;
		Rhi::TextureHandle SceneBehindWater;
		std::vector<std::uint16_t> LiquidBytes;
		Rhi::TextureHandle MagicVolumes[2];
		Vec3 MagicCorners[2];
		Vec3 MagicSizes[2];
		Rhi::TextureHandle MagicBendTarget;
		Rhi::TextureHandle MotionVectors;
		Rhi::TextureHandle Upscaled;
		Rhi::TextureHandle BlurredView;
		Rhi::TextureHandle BlurScratch;
		Rhi::TextureHandle ShadowMap;
		Rhi::TextureHandle FontTexture;
		std::uint32_t ShadowResolution = 0;

		Rhi::BufferHandle PrimitiveBuffers[static_cast<int>(EPrimitive::Count)];
		std::uint32_t PrimitiveVertices[static_cast<int>(EPrimitive::Count)] = {};
		Rhi::AccelerationHandle PrimitiveGeometry[static_cast<int>(EPrimitive::Count)];
		std::vector<Rhi::RayInstance> RayInstances;
		std::vector<Rhi::BufferHandle> RayBuffers;
		Rhi::BufferHandle WaterVertices;
		Rhi::BufferHandle WaterIndices;
		std::uint32_t WaterIndexCount = 0;
		Rhi::BufferHandle StarBuffer;
		std::uint32_t StarCount = 0;

		std::vector<float> Instances[static_cast<int>(EPrimitive::Count)];
		std::uint64_t TriangleCount = 0;

		std::uint32_t JitterIndex = 0;
		bool bHistoryValid = false;
		Float4x4 PreviousSteady;
		Vec3 PreviousCamera;

		FontAtlas Font;
		float FontSize = 0.0f;
		std::vector<UiVertex> UiVertices;
	};
}
