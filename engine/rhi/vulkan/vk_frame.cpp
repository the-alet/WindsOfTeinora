// Vulkan device: teardown, bindless descriptor writes, deferred deletion, immediate submits, frame loop, upload ring.
#include <cstdio>

#include <windows.h>

#include "vk_internal.h"

namespace taelia::rhi::vk {

VulkanDevice::~VulkanDevice() {
    if (!device) {
        slUnload(this);
        if (messenger) vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        if (vulkanModule_) FreeLibrary(static_cast<HMODULE>(vulkanModule_));
        return;
    }
    vkDeviceWaitIdle(device);
    slUnload(this);  // before anything is destroyed, once the GPU is idle
    ring_.reset();
    for (auto& fn : pendingGarbage_) fn();
    pendingGarbage_.clear();
    collectGarbage(true);
    for (auto& f : frames_) {
        if (f.pool) vkDestroyCommandPool(device, f.pool, nullptr);
        if (f.acquire) vkDestroySemaphore(device, f.acquire, nullptr);
    }
    if (immPool) vkDestroyCommandPool(device, immPool, nullptr);
    if (immFence) vkDestroyFence(device, immFence, nullptr);
    if (timeline) vkDestroySemaphore(device, timeline, nullptr);
    if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    if (setLayout0) vkDestroyDescriptorSetLayout(device, setLayout0, nullptr);
    if (setLayout1) vkDestroyDescriptorSetLayout(device, setLayout1, nullptr);
    for (VkSampler s : samplers)
        if (s) vkDestroySampler(device, s, nullptr);
    if (allocator) vmaDestroyAllocator(allocator);
    vkDestroyDevice(device, nullptr);
    if (messenger) vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
    if (vulkanModule_) FreeLibrary(static_cast<HMODULE>(vulkanModule_));
}

// ---------------------------------------------------------------- bindless indices and descriptors
uint32_t VulkanDevice::allocIndex() {
    if (!freeIndices_.empty()) {
        const uint32_t i = freeIndices_.back();
        freeIndices_.pop_back();
        return i;
    }
    if (nextIndex_ >= bindlessCount) {
        std::fputs("[vulkan] out of bindless descriptor indices\n", stderr);
        return 0;
    }
    return nextIndex_++;
}

void VulkanDevice::writeImageDescriptor(uint32_t index, VkImageView view, bool storage) {
    if (!index) return;
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view,
                             storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set0;
    w.dstBinding = storage ? 1 : 0;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

void VulkanDevice::writeBufferDescriptor(uint32_t index, VkBuffer buffer) {
    if (!index) return;
    VkDescriptorBufferInfo bi{buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set0;
    w.dstBinding = 2;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

void VulkanDevice::setName(VkObjectType type, uint64_t handle, const std::string& name) {
    if (!debugUtils_ || name.empty() || !handle) return;
    VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name.c_str();
    vkSetDebugUtilsObjectNameEXT(device, &ni);
}

// ---------------------------------------------------------------- deferred deletion
void VulkanDevice::retire(std::function<void()> fn) {
    if (recording_)
        pendingGarbage_.push_back(std::move(fn));  // may be used by the frame being recorded
    else
        garbage_.push_back({submitted_, std::move(fn)});
}

void VulkanDevice::collectGarbage(bool all) {
    uint64_t done = 0;
    if (!all) vkGetSemaphoreCounterValue(device, timeline, &done);
    std::vector<Garbage> keep, run;
    for (auto& g : garbage_) (all || g.value <= done ? run : keep).push_back(std::move(g));
    garbage_ = std::move(keep);
    for (auto& g : run) g.fn();
}

// ---------------------------------------------------------------- immediate submits
VkCommandBuffer VulkanDevice::beginImmediate() {
    vkResetCommandPool(device, immPool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(immCb, &bi);
    return immCb;
}

void VulkanDevice::endImmediate() {
    vkEndCommandBuffer(immCb);
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = immCb;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    vkQueueSubmit2(queue, 1, &si, immFence);
    vkWaitForFences(device, 1, &immFence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &immFence);
}

// ---------------------------------------------------------------- frame loop
CommandList* VulkanDevice::beginFrame(Swapchain* swapchain) {
    const uint32_t slot = uint32_t(frameNumber_ % kFramesInFlight);
    Frame& f = frames_[slot];
    if (f.timelineValue) {
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &timeline;
        wi.pValues = &f.timelineValue;
        vkWaitSemaphores(device, &wi, UINT64_MAX);
    }
    collectGarbage(false);
    ringHead_ = uint64_t(slot) * kRingBytesPerFrame;

    vkResetCommandPool(device, f.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cb, &bi);
    cmd_.begin(f.cb);
    recording_ = true;
    if (swapchain) static_cast<VkSwap*>(swapchain)->acquire(f.acquire);
    return &cmd_;
}

void VulkanDevice::endFrame(Swapchain* swapchain) {
    const uint32_t slot = uint32_t(frameNumber_ % kFramesInFlight);
    Frame& f = frames_[slot];
    VkSwap* sc = static_cast<VkSwap*>(swapchain);
    const bool present = sc && sc->acquired_;
    if (present) textureBarrier(this, f.cb, sc->images_[sc->imageIndex_].get(), State::Present);
    vkEndCommandBuffer(f.cb);

    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f.cb;
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.acquire;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signal[2]{};
    signal[0].sType = signal[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal[0].semaphore = timeline;
    signal[0].value = ++submitted_;
    signal[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    if (present) {
        signal[1].semaphore = sc->renderDone_[sc->imageIndex_];
        signal[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = present ? 1 : 0;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = present ? 2 : 1;
    si.pSignalSemaphoreInfos = signal;
    const VkResult r = vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) std::fprintf(stderr, "[vulkan] vkQueueSubmit2 failed (%d)\n", int(r));
    f.timelineValue = submitted_;
    for (auto& fn : pendingGarbage_) garbage_.push_back({submitted_, std::move(fn)});
    pendingGarbage_.clear();
    recording_ = false;
    if (present) sc->present(queue);
    ++frameNumber_;
}

void VulkanDevice::waitIdle() {
    vkDeviceWaitIdle(device);
    if (!recording_) collectGarbage(true);
}

Transient VulkanDevice::allocTransient(uint64_t size) {
    const uint32_t slot = uint32_t(frameNumber_ % kFramesInFlight);
    const uint64_t end = uint64_t(slot + 1) * kRingBytesPerFrame;
    const uint64_t offset = (ringHead_ + 255) & ~uint64_t(255);
    if (offset + size > end) {
        std::fprintf(stderr, "[vulkan] upload ring full (%llu bytes asked)\n", (unsigned long long)size);
        return {};
    }
    ringHead_ = offset + size;
    Transient t;
    t.cpu = static_cast<uint8_t*>(ring_->mapped()) + offset;
    t.buffer = ring_.get();
    t.offset = offset;
    t.size = size;
    return t;
}

}  // namespace taelia::rhi::vk
