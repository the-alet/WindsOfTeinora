// Vulkan backend internals (see engine/rhi/rhi.h for the contract).
#pragma once
#include <volk.h>

#pragma warning(push, 0)
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>
#pragma warning(pop)

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "rhi/backends.h"

namespace taelia::rhi::vk {

class VulkanDevice;

VkFormat toVk(Format f);

class VkTex final : public Texture {
public:
    VkTex(VulkanDevice* dev, const TextureDesc& d) : dev_(dev) { desc_ = d; }
    ~VkTex() override;
    VulkanDevice* dev_;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;  // null for swapchain images (not owned)
    VkImageView srvView = VK_NULL_HANDLE;
    std::vector<VkImageView> uavViews;
    VkImageView rtvView = VK_NULL_HANDLE;  // whole mip 0 / layer 0 attachment view
    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags vkUsage = 0;  // as created (described to Streamline)
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t layers = 1;  // array layers of the VkImage
    State state = State::Undefined;
    bool ownsImage = true;
    void setSrv(uint32_t i) { srv_ = i; }
    void addUav(uint32_t i) { uavs_.push_back(i); }
    std::vector<uint32_t>& uavList() { return uavs_; }
};

class VkBuf final : public Buffer {
public:
    VkBuf(VulkanDevice* dev, const BufferDesc& d) : dev_(dev) { desc_ = d; }
    ~VkBuf() override;
    VulkanDevice* dev_;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    State state = State::Undefined;
    void setSrv(uint32_t i) { srv_ = i; }
    void setUav(uint32_t i) { uav_ = i; }
    void setMapped(void* p) { mapped_ = p; }
};

class VkPipe final : public Pipeline {
public:
    VkPipe(VulkanDevice* dev, bool compute) : dev_(dev) { compute_ = compute; }
    ~VkPipe() override;
    VulkanDevice* dev_;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

class VkAccel final : public AccelerationStructure {
public:
    VkAccel(VulkanDevice* dev, bool top) : dev_(dev) { topLevel_ = top; }
    ~VkAccel() override;
    VulkanDevice* dev_;
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE, scratch = VK_NULL_HANDLE;  // storage and persistent build scratch
    VmaAllocation alloc = nullptr, scratchAlloc = nullptr;
    VkDeviceAddress address = 0, scratchAddress = 0;
    uint32_t maxInstances = 0;
    void setSrv(uint32_t i) { srv_ = i; }
};

// Extra stages of State::ShaderRead (acceleration-structure builds read their inputs there when supported).
extern VkPipelineStageFlags2 g_extraReadStages;

class VkCmd final : public CommandList {
public:
    explicit VkCmd(VulkanDevice* dev) : dev_(dev) {}
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
    bool upscale(const UpscaleInputs& inputs) override;

    // Binds set 0 (and set 1 at offset 0) for graphics and compute; called after vkBeginCommandBuffer.
    void begin(VkCommandBuffer cb);

    VulkanDevice* dev_;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    // Bound state, re-bound after Streamline evaluates (manual hooking doesn't restore it).
    VkPipe* pipeline_ = nullptr;
    uint32_t frameConstOffset_ = 0;
    uint32_t pushBytes_ = 0;
    uint8_t push_[kPushConstantBytes] = {};
    void restoreBindings();
};

// Records barriers for state transitions (shared by frame and immediate command buffers).
void textureBarrier(VulkanDevice* dev, VkCommandBuffer cb, VkTex* t, State to);
void bufferBarrier(VkCommandBuffer cb, VkBuf* b, State to);

class VkSwap final : public Swapchain {
public:
    VkSwap(VulkanDevice* dev) : dev_(dev) {}
    ~VkSwap() override;
    Format format() const override { return format_; }
    uint32_t width() const override { return width_; }
    uint32_t height() const override { return height_; }
    Texture* backbuffer() override { return acquired_ ? images_[imageIndex_].get() : nullptr; }
    void resize(uint32_t width, uint32_t height) override;

    bool create(void* hwnd, uint32_t w, uint32_t h, std::string* error);
    bool build(uint32_t w, uint32_t h);  // (re)creates the swapchain
    void destroyImages();
    bool acquire(VkSemaphore signal);    // sets acquired_
    void present(VkQueue q);

    VulkanDevice* dev_;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    Format format_ = Format::RGBA8_UNORM;
    VkFormat vkFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t width_ = 0, height_ = 0;
    std::vector<std::unique_ptr<VkTex>> images_;
    std::vector<VkSemaphore> renderDone_;  // one per swapchain image
    uint32_t imageIndex_ = 0;
    bool acquired_ = false;
    bool dirty_ = false;
};

class VulkanDevice final : public Device {
public:
    ~VulkanDevice() override;
    bool init(const DeviceDesc& desc, std::string* error);

    Api api() const override { return Api::Vulkan; }
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
    uint32_t debugMessageCount() const override { return debugMessages_.load(); }
    bool debugLayerActive() const override { return validation_; }
    bool rayTracingSupported() const override { return rayTracing_; }
    std::unique_ptr<AccelerationStructure> createBlas(const BlasDesc& desc) override;
    std::unique_ptr<AccelerationStructure> createTlas(uint32_t maxInstances, const std::string& name) override;
    bool upscalerSupported() const override;
    bool upscaleRenderSize(UpscaleMode mode, uint32_t outWidth, uint32_t outHeight, uint32_t& renderWidth,
                           uint32_t& renderHeight) override;
    bool latencySupported() const override;
    void setLatencyMode(LatencyMode mode) override;
    void latencySleep() override;
    void latencyMarker(LatencyMarker marker) override;

    // Internals.
    uint32_t allocIndex();
    void writeImageDescriptor(uint32_t index, VkImageView view, bool storage);
    void writeBufferDescriptor(uint32_t index, VkBuffer buffer);
    void writeAccelDescriptor(uint32_t index, VkAccelerationStructureKHR as);
    VkDeviceAddress bufferAddress(VkBuffer b) const;
    // Storage + scratch buffers and the structure itself; false on failure (partial results freed by ~VkAccel).
    bool createAccel(VkAccel* a, VkAccelerationStructureTypeKHR type, VkDeviceSize asSize, VkDeviceSize scratchSize,
                     const std::string& name);
    // Runs `fn` once the GPU is done with everything recorded or submitted so far.
    void retire(std::function<void()> fn);
    void freeIndex(uint32_t i) { if (i) freeIndices_.push_back(i); }
    void collectGarbage(bool all);
    void setName(VkObjectType type, uint64_t handle, const std::string& name);
    VkCommandBuffer beginImmediate();
    void endImmediate();  // submits and waits
    VkShaderModule loadShader(const std::string& name, std::string* error);

    std::string adapterName_;
    std::string shaderDir_;
    bool validation_ = false;
    bool debugUtils_ = false;
    bool rayTracing_ = false;  // VK_KHR_acceleration_structure + VK_KHR_ray_query + bufferDeviceAddress
    bool streamline_ = false;  // Streamline loaded and handed this device (vk_streamline.cpp)
    LatencyMode latencyMode_ = LatencyMode::Off;
    void* vulkanModule_ = nullptr;  // vulkan-1.dll when Streamline routes the loader
    PFN_vkGetInstanceProcAddr nativeGetInstanceProcAddr_ = nullptr;
    VkDeviceSize scratchAlign_ = 256;
    VkDeviceAddress ringAddress_ = 0;
    std::atomic<uint32_t> debugMessages_{0};

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VmaAllocator allocator = nullptr;

    VkSampler samplers[6] = {};
    VkDescriptorSetLayout setLayout0 = VK_NULL_HANDLE, setLayout1 = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set0 = VK_NULL_HANDLE, set1 = VK_NULL_HANDLE;
    uint32_t bindlessCount = kMaxBindless;
    uint32_t nextIndex_ = 1;
    std::vector<uint32_t> freeIndices_;

    // Frame loop.
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkSemaphore acquire = VK_NULL_HANDLE;
        uint64_t timelineValue = 0;
    };
    Frame frames_[kFramesInFlight];
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t submitted_ = 0;  // last timeline value signaled by a submit
    uint64_t frameNumber_ = 0;
    bool recording_ = false;
    VkCmd cmd_{this};

    // Deferred deletion.
    struct Garbage {
        uint64_t value;
        std::function<void()> fn;
    };
    std::vector<Garbage> garbage_;
    std::vector<std::function<void()>> pendingGarbage_;  // retired while recording: wait for this frame's submit

    // Immediate (synchronous) submits.
    VkCommandPool immPool = VK_NULL_HANDLE;
    VkCommandBuffer immCb = VK_NULL_HANDLE;
    VkFence immFence = VK_NULL_HANDLE;

    // Upload ring: one buffer, one region per frame slot.
    static constexpr uint64_t kRingBytesPerFrame = 16ull << 20;
    std::unique_ptr<VkBuf> ring_;
    uint64_t ringHead_ = 0;
};


// ---- NVIDIA Streamline (vk_streamline.cpp).
// What the loaded Streamline features need from the instance and device.
struct SlRequirements {
    std::vector<std::string> instanceExtensions, deviceExtensions;
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    uint32_t graphicsQueues = 0, computeQueues = 0;
};
// Loads Streamline (volk from vulkan-1.dll for now) and collects what DLSS, Reflex and PCL need from the instance
// and device. False (volk untouched) if unavailable.
bool slLoad(VulkanDevice* dev, SlRequirements& req);

// dst |= want & supported, for every VkBool32 of a Vulkan feature struct (after sType / pNext).
template <class T>
void mergeFeatures(T& dst, const T& want, const T& supported) {
    constexpr size_t first = offsetof(T, pNext) + sizeof(void*);
    auto* d = reinterpret_cast<VkBool32*>(reinterpret_cast<char*>(&dst) + first);
    auto* w = reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(&want) + first);
    auto* s = reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(&supported) + first);
    for (size_t i = 0; i < (sizeof(T) - first) / sizeof(VkBool32); ++i) d[i] |= w[i] & s[i];
}
// After vkCreateDevice: slSetVulkanInfo, volk reloaded through Streamline (hooked present / swapchain / surface),
// support check. Streamline's queues follow ours in the device's family.
void slHandOver(VulkanDevice* dev, uint32_t slGraphicsQueueIndex, uint32_t slComputeQueueIndex);
// slShutdown and unload (after vkDeviceWaitIdle); volk is reloaded from vulkan-1.dll.
void slUnload(VulkanDevice* dev);

}  // namespace taelia::rhi::vk
