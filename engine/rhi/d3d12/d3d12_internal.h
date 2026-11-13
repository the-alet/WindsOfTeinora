// D3D12 backend internals (see engine/rhi/rhi.h for the contract).
#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <D3D12MemAlloc.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "rhi/backends.h"

namespace taelia::rhi::d3d12 {
using Microsoft::WRL::ComPtr;
class D3D12Device;
class D3D12Swapchain;

inline constexpr uint32_t kInvalid = UINT32_MAX;
inline constexpr uint64_t kRingBytesPerFrame = 16ull << 20;
inline constexpr uint32_t kRtvCount = 1024;
inline constexpr uint32_t kDsvCount = 512;

DXGI_FORMAT toDxgi(Format f);
D3D12_RESOURCE_STATES toD3D(State s, bool isBuffer);
void setName(ID3D12Object* o, const std::string& name);
std::wstring toWide(const std::string& s);

// A descriptor heap with a free list. Index 0 is reserved when `reserveZero`.
struct DescriptorPool {
    ComPtr<ID3D12DescriptorHeap> heap;
    uint32_t inc = 0, capacity = 0, next = 0;
    bool reserveZero = false;
    std::vector<uint32_t> freeList;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase{};

    bool init(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shaderVisible, bool reserveFirst);
    uint32_t alloc();
    void free(uint32_t i);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(uint32_t i) const { return {cpuBase.ptr + size_t(i) * inc}; }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(uint32_t i) const { return {gpuBase.ptr + uint64_t(i) * inc}; }
};

class D3D12Texture final : public Texture {
public:
    D3D12Texture(D3D12Device* dev, const TextureDesc& d) : dev(dev) { desc_ = d; }
    ~D3D12Texture() override;
    D3D12Device* dev;
    ID3D12Resource* res = nullptr;          // owned by alloc, or by external
    D3D12MA::Allocation* alloc = nullptr;
    ComPtr<ID3D12Resource> external;        // swapchain buffers
    State state = State::Undefined;
    uint32_t rtv = kInvalid, dsv = kInvalid, dsvReadOnly = kInvalid;
    bool sampled = false;

    void setViews(uint32_t srv, std::vector<uint32_t> uavs) { srv_ = srv, uavs_ = std::move(uavs); }
    uint32_t arraySize() const;
};

class D3D12Buffer final : public Buffer {
public:
    D3D12Buffer(D3D12Device* dev, const BufferDesc& d) : dev(dev) { desc_ = d; }
    ~D3D12Buffer() override;
    D3D12Device* dev;
    ID3D12Resource* res = nullptr;
    D3D12MA::Allocation* alloc = nullptr;
    State state = State::Undefined;
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    bool tracked() const { return desc_.memory == Memory::GpuOnly; }  // upload/readback heaps never transition

    void setViews(uint32_t srv, uint32_t uav, void* mapped) { srv_ = srv, uav_ = uav, mapped_ = mapped; }
};

class D3D12Pipeline final : public Pipeline {
public:
    explicit D3D12Pipeline(bool compute) { compute_ = compute; }
    ComPtr<ID3D12PipelineState> pso;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    D3D12Device* dev = nullptr;
    ~D3D12Pipeline() override;
};

// Result buffer (RAYTRACING_ACCELERATION_STRUCTURE state) plus a persistent scratch buffer (UNORDERED_ACCESS).
// Builds of one structure are serialized by the queue (and by UAV barriers within a command list), so the scratch
// buffer is never in use by two builds at once.
class D3D12AccelerationStructure final : public AccelerationStructure {
public:
    D3D12AccelerationStructure(D3D12Device* dev, bool topLevel) : dev(dev) { topLevel_ = topLevel; }
    ~D3D12AccelerationStructure() override;
    D3D12Device* dev;
    D3D12MA::Allocation* alloc = nullptr;
    D3D12MA::Allocation* scratch = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS va = 0, scratchVa = 0;
    uint32_t maxInstances = 0;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
    void setSrv(uint32_t s) { srv_ = s; }
};

// Barriers with backend state tracking, shared by the frame and the immediate command lists.
void transition(ID3D12GraphicsCommandList* cl, D3D12Texture* t, State s);
void transition(ID3D12GraphicsCommandList* cl, D3D12Buffer* b, State s);

class D3D12CommandList final : public CommandList {
public:
    D3D12Device* dev = nullptr;
    ComPtr<ID3D12GraphicsCommandList> cl;
    D3D12Pipeline* pipeline = nullptr;

    void barrier(Texture* t, State state) override;
    void barrier(Buffer* b, State state) override;
    void beginRenderPass(const RenderPass& pass) override;
    void endRenderPass() override;
    void setViewport(float x, float y, float w, float h) override;
    void setScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) override;
    void setPipeline(Pipeline* p) override;
    void pushConstants(const void* data, uint32_t size) override;
    void setFrameConstants(const Transient& t) override;
    void setIndexBuffer(Buffer* b, uint64_t offset, IndexType type) override;
    void draw(uint32_t vertexCount, uint32_t instanceCount) override;
    void drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex) override;
    void dispatch(uint32_t x, uint32_t y, uint32_t z) override;
    void copyBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) override;
    void copyBufferToTexture(Texture* dst, uint32_t mip, uint32_t layer, Buffer* src, uint64_t srcOffset) override;
    void copyTexture(Texture* dst, Texture* src) override;
    void beginMarker(const char* name) override;
    void endMarker() override;
    void drawIndirect(Buffer* args, uint64_t offset, uint32_t drawCount) override;
    void drawIndexedIndirect(Buffer* args, uint64_t offset, uint32_t drawCount) override;
    void dispatchIndirect(Buffer* args, uint64_t offset) override;
    void buildBlas(AccelerationStructure* blas, const BlasDesc& desc) override;
    void buildTlas(AccelerationStructure* tlas, const TlasInstance* instances, uint32_t count) override;

    bool upscale(const UpscaleInputs& inputs) override;  // d3d12_streamline.cpp

    ComPtr<ID3D12GraphicsCommandList4> cl4;  // queried lazily for acceleration-structure builds
    // Bindings set through this interface, re-applied after work recorded by others (Streamline / NGX).
    uint32_t push[kPushConstantBytes / 4] = {};
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants = 0;
    void restoreBindings();
};

class D3D12Device final : public Device {
public:
    ~D3D12Device() override;
    bool init(const DeviceDesc& desc, std::string* error);

    Api api() const override { return Api::D3D12; }
    const std::string& adapterName() const override { return adapterName_; }
    std::unique_ptr<Swapchain> createSwapchain(void* hwnd, uint32_t width, uint32_t height) override;
    std::unique_ptr<Texture> createTexture(const TextureDesc& d) override;
    std::unique_ptr<Buffer> createBuffer(const BufferDesc& d, const void* initialData) override;
    void uploadTexture(Texture* t, const void* data, size_t size) override;
    std::unique_ptr<Pipeline> createGraphicsPipeline(const GraphicsPipelineDesc& d, std::string* error) override;
    std::unique_ptr<Pipeline> createComputePipeline(const ComputePipelineDesc& d, std::string* error) override;
    CommandList* beginFrame(Swapchain* swapchain) override;
    void endFrame(Swapchain* swapchain) override;
    void waitIdle() override;
    uint64_t frameNumber() const override { return frameNumber_; }
    Transient allocTransient(uint64_t size) override;
    bool readbackRGBA8(Texture* t, std::vector<uint8_t>& out) override;
    uint32_t debugMessageCount() const override { return debugMessages.load(); }
    bool debugLayerActive() const override { return debugActive_; }
    bool rayTracingSupported() const override;
    std::unique_ptr<AccelerationStructure> createBlas(const BlasDesc& desc) override;
    std::unique_ptr<AccelerationStructure> createTlas(uint32_t maxInstances, const std::string& name) override;
    bool upscalerSupported() const override;
    bool upscaleRenderSize(UpscaleMode mode, uint32_t outWidth, uint32_t outHeight, uint32_t& renderWidth,
                           uint32_t& renderHeight) override;
    bool latencySupported() const override;
    void setLatencyMode(LatencyMode mode) override;
    void latencySleep() override;
    void latencyMarker(LatencyMarker marker) override;

    // Streamline (d3d12_streamline.cpp): the device is handed over after creation; swapchains are upgraded to
    // SL proxies right after creation so its present path (presentCommon) runs every frame.
    bool slActive = false;
    void initStreamline();
    void upgradeSwapchain(ComPtr<IDXGISwapChain1>& sc);

    // Ray tracing / indirect internals (d3d12_raytracing.cpp).
    enum class IndirectKind { Draw, DrawIndexed, Dispatch };
    ID3D12CommandSignature* commandSignature(IndirectKind kind);
    ComPtr<ID3D12CommandSignature> cmdSigs[3];

    // Internals used by the resource classes.
    void defer(std::function<void()> fn);   // runs once the GPU is done with everything submitted so far
    void collectGarbage(bool all);
    ID3D12GraphicsCommandList* beginImmediate();
    void endImmediate();                    // submits and waits
    void waitFence(uint64_t value);
    bool createTextureViews(D3D12Texture* t);

    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    uint64_t fenceValue = 0;
    D3D12MA::Allocator* allocator = nullptr;
    ComPtr<ID3D12RootSignature> rootSig;
    DescriptorPool bindless, rtvs, dsvs;
    bool tearing = false;
    std::atomic<uint32_t> debugMessages{0};

private:
    struct Garbage {
        uint64_t fence;
        std::function<void()> fn;
    };
    bool createRootSignature(std::string* error);

    std::string adapterName_;
    std::string shaderDir_;
    bool debugActive_ = false;
    ComPtr<ID3D12InfoQueue1> infoQueue_;
    DWORD callbackCookie_ = 0;

    ComPtr<ID3D12CommandAllocator> frameAlloc_[kFramesInFlight];
    uint64_t frameFence_[kFramesInFlight] = {};
    D3D12CommandList cmd_;
    ComPtr<ID3D12CommandAllocator> immAlloc_;
    ComPtr<ID3D12GraphicsCommandList> immList_;
    uint64_t frameNumber_ = 0;
    bool inFrame_ = false;
    std::vector<Garbage> garbage_;

    std::unique_ptr<Buffer> ring_;
    uint64_t ringBase_ = 0, ringOffset_ = 0;
    std::unique_ptr<Buffer> zeroConstants_;

    friend class D3D12CommandList;
    friend class D3D12Swapchain;

public:
    D3D12_GPU_VIRTUAL_ADDRESS zeroConstantsVA() const;
    const std::string& shaderDir() const { return shaderDir_; }
};

class D3D12Swapchain final : public Swapchain {
public:
    D3D12Swapchain(D3D12Device* dev) : dev(dev) {}
    ~D3D12Swapchain() override;
    bool init(HWND hwnd, uint32_t w, uint32_t h);
    Format format() const override { return Format::RGBA8_UNORM; }
    uint32_t width() const override { return width_; }
    uint32_t height() const override { return height_; }
    Texture* backbuffer() override;
    void resize(uint32_t width, uint32_t height) override;
    void present();

    D3D12Device* dev;
    ComPtr<IDXGISwapChain3> swapchain;
    static constexpr uint32_t kBuffers = 3;
    std::unique_ptr<D3D12Texture> buffers[kBuffers];
    uint32_t width_ = 0, height_ = 0;

private:
    void createBuffers();
    void releaseBuffers();
};

}  // namespace taelia::rhi::d3d12
