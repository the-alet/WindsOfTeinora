// Entry points of the two backends (each lives in its own library).
#pragma once
#include "rhi/rhi.h"

namespace taelia::rhi {
std::unique_ptr<Device> createD3D12Device(const DeviceDesc& desc, std::string* error);
std::unique_ptr<Device> createVulkanDevice(const DeviceDesc& desc, std::string* error);

// Reads a compiled shader ("file.Entry" + ".dxil" or ".spv") from desc.shaderDir.
bool loadShaderBinary(const std::string& shaderDir, const std::string& name, const char* extension,
                      std::vector<uint8_t>& out, std::string* error);
}  // namespace taelia::rhi
