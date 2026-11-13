// Vulkan side of NVIDIA Streamline (DLSS Super Resolution, Reflex) with manual hooking: volk routed through
// Streamline's vkGetInstanceProcAddr (present / swapchain / surface hooks), instance and device created natively
// with the features' requirements, slSetVulkanInfo, resource description and binding restore after an evaluate.
#include <cstdio>
#include <string>

#include <windows.h>

#include "rhi/streamline.h"
#include "vk_internal.h"

#pragma warning(push, 0)
#include <sl.h>
#include <sl_consts.h>
#include <sl_helpers_vk.h>
#pragma warning(pop)

namespace taelia::rhi::vk {

namespace {

void addUnique(std::vector<std::string>& list, const char* name) {
    for (auto& s : list)
        if (s == name) return;
    list.emplace_back(name);
}

template <class T>
void orFeatures(T& dst, const T& src) {
    T all = src;
    for (VkBool32* p = reinterpret_cast<VkBool32*>(reinterpret_cast<char*>(&all) + offsetof(T, pNext) + sizeof(void*));
         p < reinterpret_cast<VkBool32*>(&all + 1); ++p)
        *p = VK_TRUE;
    mergeFeatures(dst, src, all);
}

VkImageLayout layoutOf(const VkTex* t) {
    if (t->state == State::UnorderedAccess) return VK_IMAGE_LAYOUT_GENERAL;
    if (t->state == State::ShaderRead)
        return t->aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                                                      : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (t->state == State::DepthRead) return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    return VK_IMAGE_LAYOUT_GENERAL;
}

}  // namespace

bool slLoad(VulkanDevice* dev, SlRequirements& req) {
    std::string why;
    if (!streamline::load(true, &why)) {
        std::fprintf(stderr, "[vulkan] Streamline not used: %s\n", why.c_str());
        return false;
    }
    auto slGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(streamline::proc("vkGetInstanceProcAddr"));
    auto getRequirements = reinterpret_cast<PFun_slGetFeatureRequirements*>(streamline::proc("slGetFeatureRequirements"));
    HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
    auto native = vulkan ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                               reinterpret_cast<void*>(GetProcAddress(vulkan, "vkGetInstanceProcAddr")))
                         : nullptr;
    if (!slGetInstanceProcAddr || !getRequirements || !native || !streamline::proc("slSetVulkanInfo")) {
        std::fprintf(stderr, "[vulkan] Streamline not used: %s\n",
                     native ? "sl.interposer.dll lacks Vulkan exports" : "no vulkan-1.dll");
        streamline::unload();
        if (vulkan) FreeLibrary(vulkan);
        return false;
    }
    dev->vulkanModule_ = vulkan;
    dev->nativeGetInstanceProcAddr_ = native;
    // Instance and device are created natively (we add the requirements ourselves); Streamline's entry points
    // only work once slSetVulkanInfo has told it about them, so volk switches over in slHandOver.
    volkInitializeCustom(native);

    for (sl::Feature f : {sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL}) {
        sl::FeatureRequirements r{};
        if (getRequirements(f, r) != sl::Result::eOk) continue;
        for (uint32_t i = 0; i < r.vkNumInstanceExtensions; ++i) addUnique(req.instanceExtensions, r.vkInstanceExtensions[i]);
        for (uint32_t i = 0; i < r.vkNumDeviceExtensions; ++i) addUnique(req.deviceExtensions, r.vkDeviceExtensions[i]);
        orFeatures(req.features12, sl::getVkPhysicalDeviceVulkan12Features(r.vkNumFeatures12, r.vkFeatures12));
        orFeatures(req.features13, sl::getVkPhysicalDeviceVulkan13Features(r.vkNumFeatures13, r.vkFeatures13));
        req.graphicsQueues += r.vkNumGraphicsQueuesRequired;
        req.computeQueues += r.vkNumComputeQueuesRequired;
    }
    dev->streamline_ = true;
    return true;
}

void slHandOver(VulkanDevice* dev, uint32_t slGraphicsQueueIndex, uint32_t slComputeQueueIndex) {
    if (!dev->streamline_) return;
    auto setVulkanInfo = reinterpret_cast<PFun_slSetVulkanInfo*>(streamline::proc("slSetVulkanInfo"));
    sl::VulkanInfo info{};
    info.device = dev->device;
    info.instance = dev->instance;
    info.physicalDevice = dev->physical;
    info.graphicsQueueFamily = info.computeQueueFamily = dev->queueFamily;
    info.graphicsQueueIndex = slGraphicsQueueIndex;
    info.computeQueueIndex = slComputeQueueIndex;
    const sl::Result r = setVulkanInfo(info);
    if (r != sl::Result::eOk) {
        std::fprintf(stderr, "[vulkan] slSetVulkanInfo failed (%d): Streamline not used\n", int(r));
        slUnload(dev);
        return;
    }
    // From here on every Vulkan call goes through Streamline's vkGetInstanceProcAddr / vkGetDeviceProcAddr: the
    // hooked ones (present, swapchain, surface, vkDeviceWaitIdle) are its proxies, the rest vulkan-1.dll's.
    volkInitializeCustom(reinterpret_cast<PFN_vkGetInstanceProcAddr>(streamline::proc("vkGetInstanceProcAddr")));
    volkLoadInstanceOnly(dev->instance);
    volkLoadDevice(dev->device);
    streamline::checkSupport(nullptr, 0, dev->physical);
}

void slUnload(VulkanDevice* dev) {
    if (!dev->streamline_) return;
    streamline::unload();
    dev->streamline_ = false;
    // Streamline's proxies are gone: every entry point from vulkan-1.dll again.
    volkInitializeCustom(dev->nativeGetInstanceProcAddr_);
    if (dev->instance) volkLoadInstanceOnly(dev->instance);
    if (dev->device) volkLoadDevice(dev->device);
}

// ---------------------------------------------------------------- device queries
bool VulkanDevice::upscalerSupported() const { return streamline_ && streamline::dlssAvailable(); }

bool VulkanDevice::upscaleRenderSize(UpscaleMode mode, uint32_t outWidth, uint32_t outHeight, uint32_t& renderWidth,
                                     uint32_t& renderHeight) {
    renderWidth = outWidth;
    renderHeight = outHeight;
    return upscalerSupported() && streamline::renderSize(mode, outWidth, outHeight, renderWidth, renderHeight);
}

bool VulkanDevice::latencySupported() const { return streamline_ && streamline::reflexAvailable(); }

void VulkanDevice::setLatencyMode(LatencyMode mode) {
    if (!latencySupported()) return;
    latencyMode_ = mode;
    streamline::setLatencyMode(mode);
}

void VulkanDevice::latencySleep() {
    if (latencySupported()) streamline::latencySleep(frameNumber_);
}

void VulkanDevice::latencyMarker(LatencyMarker marker) {
    if (latencySupported()) streamline::latencyMarker(marker, frameNumber_);
}

// ---------------------------------------------------------------- DLSS
bool VkCmd::upscale(const UpscaleInputs& in) {
    if (!dev_->upscalerSupported() || !in.color || !in.depth || !in.motion || !in.output) return false;
    VkTex* tex[4] = {static_cast<VkTex*>(in.color), static_cast<VkTex*>(in.depth), static_cast<VkTex*>(in.motion),
                     static_cast<VkTex*>(in.output)};
    streamline::NativeImage images[4];
    for (int i = 0; i < 4; ++i) {
        const VkTex* t = tex[i];
        VkImageView view = t->srvView ? t->srvView : (!t->uavViews.empty() ? t->uavViews[0] : t->rtvView);
        images[i].resource = reinterpret_cast<void*>(t->image);
        images[i].view = reinterpret_cast<void*>(view);
        images[i].state = uint32_t(layoutOf(t));
        images[i].width = t->desc().width;
        images[i].height = t->desc().height;
        images[i].nativeFormat = uint32_t(t->vkFormat);
        images[i].usage = uint32_t(t->vkUsage);
    }
    const bool ok = streamline::evaluate(reinterpret_cast<void*>(cb), images, in, dev_->frameNumber());
    restoreBindings();
    return ok;
}

void VkCmd::restoreBindings() {
    const VkDescriptorSet sets[2] = {dev_->set0, dev_->set1};
    for (VkPipelineBindPoint bp : {VK_PIPELINE_BIND_POINT_GRAPHICS, VK_PIPELINE_BIND_POINT_COMPUTE})
        vkCmdBindDescriptorSets(cb, bp, dev_->pipelineLayout, 0, 2, sets, 1, &frameConstOffset_);
    if (pipeline_)
        vkCmdBindPipeline(cb, pipeline_->isCompute() ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline_->pipeline);
    if (pushBytes_) vkCmdPushConstants(cb, dev_->pipelineLayout, VK_SHADER_STAGE_ALL, 0, pushBytes_, push_);
}

}  // namespace taelia::rhi::vk
