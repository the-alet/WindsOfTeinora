// NVIDIA Streamline (DLSS Super Resolution, Reflex) shared by both backends: loading (signature checked),
// initialisation with manual hooking and no over-the-air downloads, frame tokens, DLSS options and constants,
// Reflex. The API-specific parts (handing over the device, describing resources, the command buffer, restoring
// bindings after an evaluate) live in each backend.
#pragma once
#include <cstdint>
#include <string>

#include "rhi/rhi.h"

namespace taelia::rhi::streamline {

// Before creating the D3D12 device / the Vulkan instance. False (with a reason) if Streamline can't be used.
bool load(bool vulkan, std::string* why);
// slShutdown and unload; call before destroying the device.
void unload();
bool loaded();
// An export of sl.interposer.dll (slSetD3DDevice, slSetVulkanInfo, slGetFeatureRequirements, slUpgradeInterface,
// slGetNativeInterface, vkGetInstanceProcAddr, vkGetDeviceProcAddr, ...), or nullptr.
void* proc(const char* name);

// After the device was handed over (slSetD3DDevice / slSetVulkanInfo): which features the adapter supports.
// D3D12: the adapter LUID; Vulkan: the VkPhysicalDevice.
void checkSupport(const void* luid, uint32_t luidBytes, void* vkPhysicalDevice);
bool dlssAvailable();
bool reflexAvailable();

bool renderSize(UpscaleMode mode, uint32_t outW, uint32_t outH, uint32_t& renderW, uint32_t& renderH);

// One image handed to DLSS, described the way Streamline wants it for this API.
struct NativeImage {
    void* resource = nullptr;  // ID3D12Resource* / VkImage
    void* view = nullptr;      // Vulkan: VkImageView
    void* memory = nullptr;    // Vulkan: VkDeviceMemory (may be null)
    uint32_t state = 0;        // D3D12_RESOURCE_STATES / VkImageLayout at the time DLSS runs
    uint32_t width = 0, height = 0;
    uint32_t nativeFormat = 0;  // Vulkan: VkFormat
    uint32_t usage = 0;         // Vulkan: VkImageUsageFlags
};

// Tags the four images, sets options and constants for this frame and records the evaluate into the native
// command list / buffer. images: color, depth, motion, output.
bool evaluate(void* nativeCommandBuffer, const NativeImage images[4], const UpscaleInputs& inputs, uint64_t frameNumber);

void setLatencyMode(LatencyMode mode);
void latencySleep(uint64_t frameNumber);
void latencyMarker(LatencyMarker marker, uint64_t frameNumber);

}  // namespace taelia::rhi::streamline
