// Graphics API abstraction. Everything above this header, renderer and game alike,
// is the same code whichever backend runs; only RhiD3D12.cpp and RhiVulkan.cpp
// know the APIs. Shaders are one HLSL source compiled to both DXIL and SPIR-V.
//
// Binding model, identical on both backends:
//   b0, b1     per-draw constants copied into transient memory (SetConstants)
//   b2         push constants, up to MaxPushConstantBytes (SetPushConstants)
//   t0-t5      sampled textures (SetTextures)
//   t6         the scene for ray queries (SetRayScene)
//   t7 onwards the vertex buffers of that scene, up to MaxRayBuffers
//   s0-s3      fixed samplers: linear clamp, anisotropic wrap, point clamp, shadow compare
//
// Clip space follows D3D on both backends: Y up, depth in [0, 1].
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Teinora::Rhi
{
	inline constexpr std::uint32_t MaxPushConstantBytes = 64;
	inline constexpr std::uint32_t MaxConstantBytes = 8192; // half of the least that Vulkan promises a uniform buffer may be
	inline constexpr int MaxTextureBindings = 6;
	inline constexpr std::uint32_t MaxRayBuffers = 2048;

	enum class EBackend : std::uint8_t
	{
		Auto,
		D3D12,
		Vulkan
	};

	const char* GetBackendName(EBackend Backend);

	enum class EFormat : std::uint8_t
	{
		Unknown,
		RGBA8Unorm,
		BGRA8Unorm,
		RGBA8Snorm,
		RGBA16Float,
		RG16Float,
		RG32Float,
		RGB32Float,
		RGBA32Float,
		D32Float
	};

	struct BufferHandle
	{
		std::uint32_t Id = 0;
		explicit operator bool() const { return Id != 0; }
	};

	struct TextureHandle
	{
		std::uint32_t Id = 0;
		explicit operator bool() const { return Id != 0; }
		bool operator==(const TextureHandle&) const = default;
	};

	struct PipelineHandle
	{
		std::uint32_t Id = 0;
		explicit operator bool() const { return Id != 0; }
	};

	struct AccelerationHandle
	{
		std::uint32_t Id = 0;
		explicit operator bool() const { return Id != 0; }
	};

	/** Triangles that rays can hit, placed in the scene. */
	struct RayInstance
	{
		float Transform[3][4] = {}; // rows of the matrix from the geometry to the scene
		AccelerationHandle Geometry;
		std::uint32_t Index = 0;    // 24 bits, which a shader reads back from a hit
		bool bCutOut = false;       // a ray only stops here where the shader decides it does, for what has holes in it
	};

	/** For the repeating sampler, which textures viewed at a shallow angle go through. */
	inline constexpr unsigned MaxAnisotropy = 16;

	struct TextureDesc
	{
		std::uint32_t Width = 1;
		std::uint32_t Height = 1;
		EFormat Format = EFormat::RGBA8Unorm;
		std::uint32_t MipLevels = 1;
		std::uint32_t Layers = 1;   // above one, shaders see a Texture2DArray
		std::uint32_t Depth = 1;    // above one, shaders see a Texture3D
		bool bRenderTarget = false; // colour or depth target, depending on the format
		bool bUnorderedAccess = false; // written directly by a compute workload, such as an upscaler
	};

	enum class ECull : std::uint8_t
	{
		None,
		Back,
		Front
	};

	enum class EBlend : std::uint8_t
	{
		Opaque,
		Alpha,
		Additive
	};

	enum class ECompare : std::uint8_t
	{
		Always,
		Greater,
		GreaterEqual,
		LessEqual
	};

	struct ShaderBytes
	{
		const void* Data = nullptr;
		std::size_t Size = 0;
	};

	/** An attribute's position in the list is its TEXCOORD index in the vertex shader input. */
	struct VertexAttribute
	{
		EFormat Format = EFormat::Unknown;
		std::uint32_t Offset = 0;
		std::uint32_t Stream = 0;
	};

	struct VertexStream
	{
		std::uint32_t Stride = 0;
		bool bPerInstance = false;
	};

	/** Triangles wind counter-clockwise on screen when seen from the front. */
	struct PipelineDesc
	{
		ShaderBytes Vertex;
		ShaderBytes Pixel; // empty for depth-only pipelines
		std::vector<VertexStream> Streams;
		std::vector<VertexAttribute> Attributes;
		EFormat ColorFormat = EFormat::Unknown;
		EFormat DepthFormat = EFormat::Unknown;
		ECull Cull = ECull::Back;
		EBlend Blend = EBlend::Opaque;
		bool bDepthTest = false;
		bool bDepthWrite = false;
		ECompare DepthCompare = ECompare::Always;
		std::int32_t DepthBias = 0;
		float SlopeDepthBias = 0.0f;
	};

	/**
	 * Attachments and everything sampled during a pass. Barriers are not legal inside
	 * a Vulkan rendering scope, so sampled textures are declared here up front.
	 */
	struct RenderPass
	{
		TextureHandle Color;
		TextureHandle Depth;
		TextureHandle Reads[MaxTextureBindings];
		bool bClearColor = false;
		float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		bool bClearDepth = false;
		float ClearDepth = 0.0f;
		bool bDepthReadOnly = false; // lets the attached depth be sampled in the same pass
	};

	struct DeviceDesc
	{
		EBackend Backend = EBackend::Auto;
		void* NativeWindow = nullptr;
		std::uint32_t Width = 1;
		std::uint32_t Height = 1;
		bool bValidation = false;
		bool bVendorFeatures = true; // use vendor libraries such as NVIDIA Streamline when present
	};

	enum class EUpscaleMode : std::uint8_t
	{
		Off,
		Native, // anti-aliasing only, at full resolution
		Quality,
		Balanced,
		Performance,
		UltraPerformance
	};

	enum class ELowLatencyMode : std::uint8_t
	{
		Off,
		On,
		Boost
	};

	/** Matrices are in the renderer's convention and free of jitter; jitter is in pixels. */
	struct UpscaleInputs
	{
		EUpscaleMode Mode = EUpscaleMode::Off;
		TextureHandle Color;
		TextureHandle Depth;  // reversed
		TextureHandle Motion; // current to previous pixel, in UV units, camera motion included
		TextureHandle Output;
		std::uint32_t RenderWidth = 0;
		std::uint32_t RenderHeight = 0;
		std::uint32_t OutputWidth = 0;
		std::uint32_t OutputHeight = 0;
		float ViewToClip[4][4] = {};
		float ClipToView[4][4] = {};
		float ClipToPrevClip[4][4] = {};
		float PrevClipToClip[4][4] = {};
		float JitterX = 0.0f;
		float JitterY = 0.0f;
		float Forward[3] = {};
		float Right[3] = {};
		float Up[3] = {};
		float NearPlane = 0.0f;
		float VerticalFov = 0.0f;
		float Aspect = 1.0f;
		bool bReset = false; // the previous frame is unrelated to this one
	};

	/**
	 * Optional services from a GPU vendor's library. The engine works the same without
	 * them: Device::GetVendorFeatures returns null where none are available.
	 */
	class VendorFeatures
	{
	public:
		virtual ~VendorFeatures() = default;

		virtual const char* GetUpscalerName() const = 0;
		virtual bool HasUpscaling() const = 0;
		virtual bool HasLowLatency() const = 0;
		virtual void SetLowLatency(ELowLatencyMode Mode) = 0;

		// Marks the phases of a frame, for latency reduction and tracking.
		virtual void BeginSimulation() = 0;
		virtual void EndSimulation() = 0;
		virtual void BeginRenderSubmit() = 0;
		virtual void EndRenderSubmit() = 0;
		virtual void BeginPresent() = 0;
		virtual void EndPresent() = 0;

		/** Gives the resolution to render at for an output size. Returns false if the mode cannot run. */
		virtual bool GetRenderSize(EUpscaleMode Mode, std::uint32_t OutputWidth, std::uint32_t OutputHeight, std::uint32_t& OutWidth, std::uint32_t& OutHeight) = 0;

		/** Records the upscale. Call outside a pass, then Device::RestoreAfterExternal. */
		virtual void Upscale(const UpscaleInputs& Inputs) = 0;
	};

	/** What an external library needs to use a texture directly. */
	struct NativeTexture
	{
		void* Resource = nullptr; // ID3D12Resource or VkImage
		void* Memory = nullptr;   // VkDeviceMemory
		void* View = nullptr;     // VkImageView
		std::uint32_t State = 0;  // D3D12_RESOURCE_STATES or VkImageLayout
		std::uint32_t Format = 0; // VkFormat
		std::uint32_t Usage = 0;  // VkImageUsageFlags
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
	};

	class Device
	{
	public:
		virtual ~Device() = default;

		virtual EBackend GetBackend() const = 0;
		virtual const std::string& GetAdapterName() const = 0;
		virtual VendorFeatures* GetVendorFeatures() = 0;

		// For external libraries working on the current frame.
		virtual NativeTexture AccessForExternal(TextureHandle Texture, bool bWrite) = 0;
		virtual void* GetNativeCommandList() = 0;
		virtual void RestoreAfterExternal() = 0;

		// Resources. All of these may be called at any time; uploads are queued to run
		// ahead of the current frame, and destruction waits for the GPU to finish with them.
		virtual BufferHandle CreateBuffer(std::size_t Size, const void* Data) = 0;
		virtual void DestroyBuffer(BufferHandle Buffer) = 0;

		/** Pixels are tightly packed: every mip of the first layer, largest first, then the next layer. */
		virtual TextureHandle CreateTexture(const TextureDesc& Desc, const void* Pixels = nullptr) = 0;

		/** Replaces all the pixels of a texture made with pixels, ahead of the current frame. */
		virtual void UpdateTexture(TextureHandle Texture, const void* Pixels) = 0;
		virtual void DestroyTexture(TextureHandle Texture) = 0;
		virtual PipelineHandle CreatePipeline(const PipelineDesc& Desc) = 0;

		// Ray queries, on hardware that has them. Geometry is the first VertexCount vertices of
		// a buffer, unindexed, with a position as three floats at the start of every Stride bytes.
		virtual bool SupportsRayTracing() const = 0;
		virtual AccelerationHandle CreateAcceleration(BufferHandle Vertices, std::uint32_t Stride, std::uint32_t VertexCount) = 0;
		virtual void DestroyAcceleration(AccelerationHandle Geometry) = 0;

		/** Builds what rays are traced against this frame, and binds it with the buffers hits are read from. Outside a pass. */
		virtual void SetRayScene(const RayInstance* Instances, std::uint32_t Count, const BufferHandle* VertexBuffers, std::uint32_t BufferCount) = 0;

		virtual void Resize(std::uint32_t Width, std::uint32_t Height) = 0;
		virtual void WaitIdle() = 0;

		// Frame and pass structure.
		virtual void BeginFrame() = 0;
		virtual void EndFrame() = 0;
		virtual TextureHandle GetBackbuffer() const = 0;
		virtual EFormat GetBackbufferFormat() const = 0;
		virtual void BeginPass(const RenderPass& Pass) = 0;
		virtual void EndPass() = 0;

		// State and drawing, inside a pass. SetViewport also sets the scissor.
		virtual void SetPipeline(PipelineHandle Pipeline) = 0;
		virtual void SetViewport(float X, float Y, float Width, float Height) = 0;
		virtual void SetVertexBuffer(std::uint32_t Stream, BufferHandle Buffer) = 0;
		virtual void* SetTransientVertexBuffer(std::uint32_t Stream, std::size_t Size) = 0;
		virtual void SetIndexBuffer(BufferHandle Buffer) = 0;
		virtual void SetConstants(std::uint32_t Slot, const void* Data, std::size_t Size) = 0;
		virtual void SetPushConstants(const void* Data, std::size_t Size) = 0;
		virtual void SetTextures(const TextureHandle* Textures, int Count) = 0;
		virtual void Draw(std::uint32_t Vertices, std::uint32_t Instances = 1, std::uint32_t FirstVertex = 0) = 0;
		virtual void DrawIndexed(std::uint32_t Indices, std::uint32_t Instances = 1) = 0;

		/** Copies the next presented frame to memory, for TakeCapture. */
		virtual void RequestCapture() = 0;

		/** Returns the captured frame as tightly packed RGBA8, once. */
		virtual bool TakeCapture(std::vector<std::uint8_t>& OutPixels, std::uint32_t& OutWidth, std::uint32_t& OutHeight) = 0;
	};

	/** Auto tries D3D12 and then Vulkan. Returns null when nothing can be created. */
	std::unique_ptr<Device> CreateDevice(const DeviceDesc& Desc);

	std::unique_ptr<Device> CreateD3D12Device(const DeviceDesc& Desc);
	std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& Desc);
}
