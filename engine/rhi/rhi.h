// One rendering interface over D3D12 and Vulkan.
//
// Binding model (the same in both APIs, see shaders/bindless.hlsli):
//   * Bindless: every texture view and buffer gets an index into one global table; shaders index arrays with it.
//     Index 0 is reserved (null). D3D12: one shader-visible CBV/SRV/UAV heap, unbounded ranges at offset 0.
//     Vulkan: descriptor set 0, bindings 0 (sampled images), 1 (storage images), 2 (storage buffers),
//     3 (acceleration structures, space11 in D3D12), 4..9 (immutable samplers); partially bound, update-after-bind.
//   * b0 = frame constants (FRAME_CONSTANTS_BYTES, from the per-frame upload ring): D3D12 root CBV,
//     Vulkan set 1 binding 0 as a dynamic uniform buffer.
//   * b1 = push constants (<= PUSH_CONSTANTS_BYTES): D3D12 root constants, Vulkan push constants.
//   * Fixed samplers s0..s5: linear wrap, linear clamp, point clamp, aniso wrap, point wrap, shadow compare.
//   * No vertex input layouts: vertex shaders pull from bindless buffers. Draws never use base vertex/instance.
// Rules: single-threaded recording on one graphics queue, FRAMES_IN_FLIGHT frames in flight,
// resource state is tracked by the backend (barrier() only names the new state). Reverse-Z depth.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace taelia::rhi {

inline constexpr uint32_t kFramesInFlight = 2;
inline constexpr uint32_t kMaxBindless = 65536;
inline constexpr uint32_t kMaxColorTargets = 4;
inline constexpr uint32_t kPushConstantBytes = 128;
inline constexpr uint32_t kFrameConstantBytes = 8192;

enum class Api { D3D12, Vulkan };

enum class Format : uint8_t {
    Unknown,
    RGBA8_UNORM,
    RGBA8_SRGB,
    BGRA8_UNORM,
    RGB10A2_UNORM,
    R11G11B10_FLOAT,
    RGBA16_FLOAT,
    RGBA32_FLOAT,
    RG16_FLOAT,
    R16_FLOAT,
    R32_FLOAT,
    R8_UNORM,
    RG8_UNORM,
    R32_UINT,
    D32_FLOAT,
};

enum class TextureDim : uint8_t { Tex2D, Tex3D, Cube, Tex2DArray };

namespace TextureUsage {
enum : uint32_t {
    Sampled = 1u << 0,       // SRV / sampled image (gets a bindless index)
    Storage = 1u << 1,       // UAV / storage image (gets a bindless index per mip)
    RenderTarget = 1u << 2,
    DepthStencil = 1u << 3,
    CopySrc = 1u << 4,
    CopyDst = 1u << 5,
};
}

namespace BufferUsage {
enum : uint32_t {
    Storage = 1u << 0,       // ByteAddressBuffer (read) and, on GpuOnly memory, RWByteAddressBuffer
    Index = 1u << 1,
    Constant = 1u << 2,      // may be bound as b0 (frame constants)
    Indirect = 1u << 3,      // arguments for drawIndirect / drawIndexedIndirect / dispatchIndirect
    CopySrc = 1u << 4,
    CopyDst = 1u << 5,
    AccelerationInput = 1u << 6,  // vertex / index data read by acceleration-structure builds
};
}

enum class Memory : uint8_t {
    GpuOnly,   // device local
    Upload,    // CPU writes, GPU reads; persistently mapped
    Readback,  // GPU writes, CPU reads; persistently mapped
};

enum class State : uint8_t {
    Undefined,        // contents may be discarded
    ShaderRead,       // sampled / read in any shader stage
    UnorderedAccess,  // read-write in shaders (a barrier to the same state is a UAV barrier)
    RenderTarget,
    DepthWrite,
    DepthRead,
    CopySrc,
    CopyDst,
    Present,
    IndexBuffer,
    IndirectArgument,
};

enum class IndexType : uint8_t { U16, U32 };
enum class Topology : uint8_t { TriangleList, TriangleStrip, LineList, PointList };
enum class Cull : uint8_t { None, Back, Front };
enum class Compare : uint8_t { Never, Less, LessEqual, Equal, Greater, GreaterEqual, Always };
enum class Blend : uint8_t {
    Opaque,
    Alpha,           // src*a + dst*(1-a)
    Premultiplied,   // src + dst*(1-a)
    Additive,        // src + dst
};
enum class LoadOp : uint8_t { Load, Clear, DontCare };

struct TextureDesc {
    uint32_t width = 1, height = 1, depthOrLayers = 1, mips = 1;
    Format format = Format::RGBA8_UNORM;
    TextureDim dim = TextureDim::Tex2D;
    uint32_t usage = TextureUsage::Sampled;
    float clearColor[4] = {0, 0, 0, 0};  // optimized clear for render targets
    float clearDepth = 0.0f;              // reverse-Z: far = 0
    std::string name;
};

struct BufferDesc {
    uint64_t size = 0;
    uint32_t usage = BufferUsage::Storage;
    Memory memory = Memory::GpuOnly;
    std::string name;
};

class Texture {
public:
    virtual ~Texture() = default;
    const TextureDesc& desc() const { return desc_; }
    uint32_t width() const { return desc_.width; }
    uint32_t height() const { return desc_.height; }
    // Bindless index of the whole-resource read view (0 if not Sampled).
    uint32_t srv() const { return srv_; }
    // Bindless index of the read-write view of one mip (0 if not Storage).
    uint32_t uav(uint32_t mip = 0) const { return mip < uavs_.size() ? uavs_[mip] : 0; }

protected:
    TextureDesc desc_;
    uint32_t srv_ = 0;
    std::vector<uint32_t> uavs_;
};

class Buffer {
public:
    virtual ~Buffer() = default;
    const BufferDesc& desc() const { return desc_; }
    uint64_t size() const { return desc_.size; }
    // Bindless index as ByteAddressBuffer (0 if not Storage).
    uint32_t srv() const { return srv_; }
    // Bindless index as RWByteAddressBuffer (0 unless Storage on GpuOnly memory).
    uint32_t uav() const { return uav_; }
    // Persistently mapped pointer for Upload / Readback memory, else nullptr.
    void* mapped() const { return mapped_; }

protected:
    BufferDesc desc_;
    uint32_t srv_ = 0, uav_ = 0;
    void* mapped_ = nullptr;
};

struct GraphicsPipelineDesc {
    std::string vs, ps;  // shader names "file.Entry": loads shaders/file.Entry.dxil or .spv next to the exe
    Format colorFormats[kMaxColorTargets] = {};
    uint32_t colorCount = 0;
    Format depthFormat = Format::Unknown;
    Blend blend[kMaxColorTargets] = {};
    Topology topology = Topology::TriangleList;
    Cull cull = Cull::None;
    bool frontCounterClockwise = false;  // front faces are clockwise in screen space (D3D convention)
    bool wireframe = false;
    bool depthTest = false, depthWrite = false;
    Compare depthCompare = Compare::GreaterEqual;  // reverse-Z
    float depthBias = 0, slopeScaledDepthBias = 0;
    std::string name;
};

struct ComputePipelineDesc {
    std::string cs;  // "file.Entry"
    std::string name;
};

class Pipeline {
public:
    virtual ~Pipeline() = default;
    bool isCompute() const { return compute_; }

protected:
    bool compute_ = false;
};

// A slice of the per-frame upload ring, valid until this frame slot comes round again.
struct Transient {
    void* cpu = nullptr;
    Buffer* buffer = nullptr;  // the ring buffer (Upload memory, Storage|Index|Constant)
    uint64_t offset = 0;       // byte offset inside it (256-aligned)
    uint64_t size = 0;
};

struct ColorAttachment {
    Texture* texture = nullptr;
    LoadOp load = LoadOp::Load;
    float clear[4] = {0, 0, 0, 0};
};

struct DepthAttachment {
    Texture* texture = nullptr;
    LoadOp load = LoadOp::Load;
    float clear = 0.0f;
    bool readOnly = false;  // DepthRead instead of DepthWrite
};

struct RenderPass {
    ColorAttachment color[kMaxColorTargets];
    uint32_t colorCount = 0;
    DepthAttachment depth;
};

// ---- Ray tracing: inline ray queries (RayQuery in any shader stage), no ray-tracing pipelines.
// One triangle geometry of a bottom-level structure. Positions are float3 at vertexStride.
struct BlasGeometry {
    Buffer* vertices = nullptr;      // GpuOnly, AccelerationInput usage, in ShaderRead state when built
    uint64_t vertexOffset = 0;
    uint32_t vertexStride = 12;
    uint32_t vertexCount = 0;
    Buffer* indices = nullptr;       // GpuOnly, AccelerationInput usage, in ShaderRead state when built
    uint64_t indexOffset = 0;
    uint32_t indexCount = 0;
    IndexType indexType = IndexType::U32;
    bool opaque = true;              // false: queries see CANDIDATE_NON_OPAQUE_TRIANGLE (alpha test in the shader)
};

struct BlasDesc {
    std::vector<BlasGeometry> geometries;  // GeometryIndex() in shaders is the position in this list
    std::string name;
};

struct TlasInstance {
    float transform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // row-major 3x4, object -> camera-relative world
    uint32_t customIndex = 0;        // 24 bits: InstanceID() in shaders
    uint8_t mask = 0xFF;
    bool doubleSided = true;         // no back-face culling for this instance
    bool forceOpaque = false;        // treat every geometry as opaque
    class AccelerationStructure* blas = nullptr;
};

class AccelerationStructure {
public:
    virtual ~AccelerationStructure() = default;
    // Bindless index of a top-level structure: g_tlas[srv()] in shaders. 0 for bottom-level ones.
    uint32_t srv() const { return srv_; }
    bool isTopLevel() const { return topLevel_; }

protected:
    uint32_t srv_ = 0;
    bool topLevel_ = false;
};

class CommandList {
public:
    virtual ~CommandList() = default;

    // Transitions to `state` from the tracked current state (no-op if equal, except UnorderedAccess -> UAV barrier).
    virtual void barrier(Texture* t, State state) = 0;
    virtual void barrier(Buffer* b, State state) = 0;

    // Attachments must already be in RenderTarget / DepthWrite (or DepthRead) state. Sets viewport and scissor
    // to the full size of the first attachment.
    virtual void beginRenderPass(const RenderPass& pass) = 0;
    virtual void endRenderPass() = 0;
    virtual void setViewport(float x, float y, float w, float h) = 0;   // y down, top-left origin, depth 0..1
    virtual void setScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) = 0;

    virtual void setPipeline(Pipeline* p) = 0;
    // b1. Size <= kPushConstantBytes, multiple of 4. Persists across setPipeline within the command list.
    virtual void pushConstants(const void* data, uint32_t size) = 0;
    // b0 for the following draws/dispatches. `t` must come from Device::allocTransient (size <= kFrameConstantBytes).
    virtual void setFrameConstants(const Transient& t) = 0;
    virtual void setIndexBuffer(Buffer* b, uint64_t offset, IndexType type) = 0;

    virtual void draw(uint32_t vertexCount, uint32_t instanceCount = 1) = 0;
    virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0) = 0;
    virtual void dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1) = 0;

    // Copies; resources must be in CopySrc / CopyDst.
    virtual void copyBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) = 0;
    // Tightly packed rows (bytesPerRow = width * texel size, any alignment handled by the backend).
    virtual void copyBufferToTexture(Texture* dst, uint32_t mip, uint32_t layer, Buffer* src, uint64_t srcOffset) = 0;
    virtual void copyTexture(Texture* dst, Texture* src) = 0;  // same size and format, mip 0 / layer 0

    virtual void beginMarker(const char* name) = 0;
    virtual void endMarker() = 0;

    // Indirect work. Argument buffers: GpuOnly with Indirect usage, in State::IndirectArgument, tightly packed:
    //   draw:        {vertexCount, instanceCount, firstVertex = 0, firstInstance = 0}                16 bytes
    //   drawIndexed: {indexCount, instanceCount, firstIndex, vertexOffset = 0, firstInstance = 0}    20 bytes
    //   dispatch:    {x, y, z}                                                                        12 bytes
    virtual void drawIndirect(Buffer* args, uint64_t offset, uint32_t drawCount = 1) = 0;
    virtual void drawIndexedIndirect(Buffer* args, uint64_t offset, uint32_t drawCount = 1) = 0;
    virtual void dispatchIndirect(Buffer* args, uint64_t offset) = 0;

    // Builds a bottom-level structure from the same desc it was created with. Outside render passes.
    // Later buildTlas calls in this command list see the result (the backend inserts the barrier).
    virtual void buildBlas(AccelerationStructure* blas, const BlasDesc& desc) = 0;
    // Rebuilds a top-level structure from `count` instances (count <= the size it was created with). Outside
    // render passes. Draws and dispatches recorded after it see the result (the backend inserts the barrier).
    virtual void buildTlas(AccelerationStructure* tlas, const TlasInstance* instances, uint32_t count) = 0;

    // Upscales inputs.color into inputs.output with DLSS Super Resolution (outside render passes). Returns false
    // if the upscaler isn't available. Afterwards the backend's own bindings (heaps, root signature / descriptor
    // sets, pipeline, push constants) are restored, so recording continues as before.
    virtual bool upscale(const struct UpscaleInputs& inputs) = 0;
};

// ---- Upscaling (DLSS Super Resolution) and latency (Reflex) through NVIDIA Streamline, when available.
enum class UpscaleMode : uint8_t { Off, DLAA, Quality, Balanced, Performance, UltraPerformance };
enum class LatencyMode : uint8_t { Off, On, Boost };
enum class LatencyMarker : uint8_t { SimulationStart, SimulationEnd, RenderSubmitStart, RenderSubmitEnd, PresentStart, PresentEnd };

struct UpscaleInputs {
    UpscaleMode mode = UpscaleMode::Quality;
    Texture* color = nullptr;    // HDR lit scene at render resolution (RGBA16F), State::ShaderRead
    Texture* depth = nullptr;    // reverse-Z depth at render resolution, State::ShaderRead
    Texture* motion = nullptr;   // RG16F, State::ShaderRead: per pixel (previous uv - current uv), camera motion included
    Texture* output = nullptr;   // RGBA16F with Storage usage at output resolution, State::UnorderedAccess
    uint32_t renderWidth = 0, renderHeight = 0;  // the region of the inputs in use (top-left)
    float jitterX = 0, jitterY = 0;              // this frame's sub-pixel jitter in render pixels (-0.5..0.5)
    bool reset = false;                          // camera cut: drop history
    // Row-major, our clip = M * v convention, camera-relative world space. Reverse-Z, infinite far plane.
    float viewToClip[16] = {}, clipToView[16] = {}, clipToPrevClip[16] = {}, prevClipToClip[16] = {};
    float cameraRight[3] = {1, 0, 0}, cameraUp[3] = {0, 1, 0}, cameraForward[3] = {0, 0, 1};
    float nearZ = 0.05f, farZ = 100000.0f, fovY = 1.0f, aspect = 1.0f;
};

class Swapchain {
public:
    virtual ~Swapchain() = default;
    virtual Format format() const = 0;
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
    // The image to render into this frame (call after Device::beginFrame). Its state is Present on return.
    virtual Texture* backbuffer() = 0;
    virtual void resize(uint32_t width, uint32_t height) = 0;  // waits for the GPU
};

struct DeviceDesc {
    Api api = Api::D3D12;
    bool debug = false;          // D3D12 debug layer / Vulkan validation (if found)
    std::string shaderDir;       // folder with the .dxil / .spv files
    // Load NVIDIA Streamline (sl.interposer.dll and its plugins next to the exe, signature verified) for DLSS and
    // Reflex. If it can't load or the GPU can't use it, the device works as usual and reports them unsupported.
    bool streamline = false;
};

class Device {
public:
    virtual ~Device() = default;
    static std::unique_ptr<Device> create(const DeviceDesc& desc, std::string* error);

    virtual Api api() const = 0;
    virtual const std::string& adapterName() const = 0;

    // Windowed output. `hwnd` is an HWND. Present without V-Sync (tearing / immediate).
    virtual std::unique_ptr<Swapchain> createSwapchain(void* hwnd, uint32_t width, uint32_t height) = 0;

    // Resources are released when the GPU is done with them (deferred by FRAMES_IN_FLIGHT frames).
    // Buffers with initial data on GpuOnly memory are uploaded synchronously.
    virtual std::unique_ptr<Texture> createTexture(const TextureDesc& d) = 0;
    virtual std::unique_ptr<Buffer> createBuffer(const BufferDesc& d, const void* initialData = nullptr) = 0;
    // Uploads tightly packed texel data to mip 0 (all layers / depth slices) synchronously; leaves it ShaderRead.
    virtual void uploadTexture(Texture* t, const void* data, size_t size) = 0;
    virtual std::unique_ptr<Pipeline> createGraphicsPipeline(const GraphicsPipelineDesc& d, std::string* error) = 0;
    virtual std::unique_ptr<Pipeline> createComputePipeline(const ComputePipelineDesc& d, std::string* error) = 0;

    // Frame loop: beginFrame waits for this frame slot's previous use, resets the upload ring and returns the
    // command list. If `swapchain` is given, its next image is acquired. endFrame submits; with a swapchain it
    // transitions the backbuffer to Present and presents.
    virtual CommandList* beginFrame(Swapchain* swapchain) = 0;
    virtual void endFrame(Swapchain* swapchain) = 0;
    virtual void waitIdle() = 0;
    virtual uint64_t frameNumber() const = 0;

    // Per-frame upload ring (256-byte aligned). Valid until the same frame slot begins again.
    virtual Transient allocTransient(uint64_t size) = 0;

    // Copies a 2D texture (mip 0) to CPU memory as tightly packed RGBA8 (converting from BGRA8 if needed).
    // Call outside beginFrame/endFrame. Waits for the GPU. Restores the texture's state.
    virtual bool readbackRGBA8(Texture* t, std::vector<uint8_t>& out) = 0;

    // Errors and warnings reported by the D3D12 debug layer / Vulkan validation since creation (each is also
    // printed to stderr and the debugger). 0 when debug is off or the layer isn't available.
    virtual uint32_t debugMessageCount() const = 0;
    // True if debug was asked for and the layer is really active.
    virtual bool debugLayerActive() const = 0;

    // Inline ray tracing (DXR 1.1 ray queries / VK_KHR_ray_query with VK_KHR_acceleration_structure).
    virtual bool rayTracingSupported() const = 0;
    // Sized for this desc (counts and formats must match the later builds); built with CommandList::buildBlas.
    virtual std::unique_ptr<AccelerationStructure> createBlas(const BlasDesc& desc) = 0;
    // Room for up to maxInstances; gets a bindless index (srv). Built with CommandList::buildTlas.
    virtual std::unique_ptr<AccelerationStructure> createTlas(uint32_t maxInstances, const std::string& name) = 0;

    // DLSS Super Resolution: available, and the render size it wants for a mode and an output size.
    virtual bool upscalerSupported() const = 0;
    virtual bool upscaleRenderSize(UpscaleMode mode, uint32_t outWidth, uint32_t outHeight, uint32_t& renderWidth,
                                   uint32_t& renderHeight) = 0;
    // Reflex: lower latency without V-Sync. Each frame: latencySleep() first (before input and simulation),
    // then the markers around the work; the backend issues PresentStart/End itself around its present.
    virtual bool latencySupported() const = 0;
    virtual void setLatencyMode(LatencyMode mode) = 0;
    virtual void latencySleep() = 0;
    virtual void latencyMarker(LatencyMarker marker) = 0;
};

// Helpers shared by both backends.
uint32_t formatBytesPerTexel(Format f);
bool isDepthFormat(Format f);
const char* apiName(Api a);

}  // namespace taelia::rhi
