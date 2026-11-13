// Vulkan swapchain: Win32 surface, no V-Sync (IMMEDIATE > MAILBOX > FIFO), recreated when out of date.
#include <cstdio>

#include <windows.h>

#include "vk_internal.h"

namespace taelia::rhi::vk {

std::unique_ptr<Swapchain> VulkanDevice::createSwapchain(void* hwnd, uint32_t width, uint32_t height) {
    auto sc = std::make_unique<VkSwap>(this);
    std::string err;
    if (!sc->create(hwnd, width, height, &err)) {
        std::fprintf(stderr, "[vulkan] swapchain: %s\n", err.c_str());
        return nullptr;
    }
    return sc;
}

bool VkSwap::create(void* hwnd, uint32_t w, uint32_t h, std::string* error) {
    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hinstance = GetModuleHandleW(nullptr);
    si.hwnd = static_cast<HWND>(hwnd);
    if (vkCreateWin32SurfaceKHR(dev_->instance, &si, nullptr, &surface) != VK_SUCCESS) {
        *error = "vkCreateWin32SurfaceKHR failed";
        return false;
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(dev_->physical, dev_->queueFamily, surface, &supported);
    if (!supported) {
        *error = "the graphics queue cannot present to this window";
        return false;
    }
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev_->physical, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev_->physical, surface, &n, formats.data());
    bool rgba = false, bgra = false;
    for (auto& f : formats) {
        if (f.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        rgba |= f.format == VK_FORMAT_R8G8B8A8_UNORM;
        bgra |= f.format == VK_FORMAT_B8G8R8A8_UNORM;
    }
    if (!rgba && !bgra) {
        *error = "no RGBA8 / BGRA8 UNORM surface format";
        return false;
    }
    format_ = rgba ? Format::RGBA8_UNORM : Format::BGRA8_UNORM;
    vkFormat_ = rgba ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev_->physical, surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev_->physical, surface, &n, modes.data());
    presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) presentMode_ = m;
    for (VkPresentModeKHR m : modes)
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) presentMode_ = m;
    if (!build(w, h)) {
        *error = "vkCreateSwapchainKHR failed";
        return false;
    }
    return true;
}

void VkSwap::destroyImages() {
    images_.clear();
    for (VkSemaphore s : renderDone_) vkDestroySemaphore(dev_->device, s, nullptr);
    renderDone_.clear();
}

bool VkSwap::build(uint32_t w, uint32_t h) {
    VkDevice device = dev_->device;
    vkDeviceWaitIdle(device);
    destroyImages();
    acquired_ = false;
    dirty_ = false;
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev_->physical, surface, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = w < caps.minImageExtent.width ? caps.minImageExtent.width
                       : w > caps.maxImageExtent.width ? caps.maxImageExtent.width : w;
        extent.height = h < caps.minImageExtent.height ? caps.minImageExtent.height
                        : h > caps.maxImageExtent.height ? caps.maxImageExtent.height : h;
    }
    width_ = extent.width;
    height_ = extent.height;
    if (width_ == 0 || height_ == 0) {  // minimized: no images until the next resize
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
        return true;
    }
    uint32_t count = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = count;
    ci.imageFormat = vkFormat_;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = presentMode_;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain;
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR(device, &ci, nullptr, &fresh);
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = fresh;
    if (r != VK_SUCCESS) {
        swapchain = VK_NULL_HANDLE;
        return false;
    }
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
    std::vector<VkImage> imgs(n);
    vkGetSwapchainImagesKHR(device, swapchain, &n, imgs.data());
    for (uint32_t i = 0; i < n; ++i) {
        TextureDesc d;
        d.width = width_;
        d.height = height_;
        d.format = format_;
        d.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc | TextureUsage::CopyDst;
        d.name = "backbuffer " + std::to_string(i);
        auto t = std::make_unique<VkTex>(dev_, d);
        t->ownsImage = false;
        t->image = imgs[i];
        t->vkFormat = vkFormat_;
        t->vkUsage = ci.imageUsage;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = imgs[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = vkFormat_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device, &vi, nullptr, &t->rtvView);
        dev_->setName(VK_OBJECT_TYPE_IMAGE, uint64_t(imgs[i]), d.name);
        images_.push_back(std::move(t));  // state Undefined: never presented yet
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        vkCreateSemaphore(device, &sci, nullptr, &s);
        renderDone_.push_back(s);
    }
    return true;
}

bool VkSwap::acquire(VkSemaphore signal) {
    acquired_ = false;
    if (dirty_) build(width_, height_);
    for (int attempt = 0; attempt < 2 && swapchain; ++attempt) {
        const VkResult r = vkAcquireNextImageKHR(dev_->device, swapchain, UINT64_MAX, signal, VK_NULL_HANDLE, &imageIndex_);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            if (r == VK_SUBOPTIMAL_KHR) dirty_ = true;
            acquired_ = true;
            return true;
        }
        if (r != VK_ERROR_OUT_OF_DATE_KHR) {
            std::fprintf(stderr, "[vulkan] vkAcquireNextImageKHR failed (%d)\n", int(r));
            return false;
        }
        build(width_, height_);
    }
    return false;
}

void VkSwap::present(VkQueue q) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone_[imageIndex_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &imageIndex_;
    // With Streamline loaded this is its present hook (presentCommon runs every frame).
    const bool reflex = dev_->latencySupported() && dev_->latencyMode_ != LatencyMode::Off;
    if (reflex) dev_->latencyMarker(LatencyMarker::PresentStart);
    const VkResult r = vkQueuePresentKHR(q, &pi);
    if (reflex) dev_->latencyMarker(LatencyMarker::PresentEnd);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) dirty_ = true;
    else if (r != VK_SUCCESS) std::fprintf(stderr, "[vulkan] vkQueuePresentKHR failed (%d)\n", int(r));
    acquired_ = false;
}

void VkSwap::resize(uint32_t w, uint32_t h) {
    if (!surface) return;
    build(w, h);
}

VkSwap::~VkSwap() {
    if (!dev_->device) return;
    vkDeviceWaitIdle(dev_->device);
    destroyImages();
    if (swapchain) vkDestroySwapchainKHR(dev_->device, swapchain, nullptr);
    if (surface) vkDestroySurfaceKHR(dev_->instance, surface, nullptr);
}

}  // namespace taelia::rhi::vk
