// Kept apart from rhi_common.cpp so a program linking only one backend (the smoke tests) doesn't need the other.
#include "rhi/backends.h"

namespace taelia::rhi {

std::unique_ptr<Device> Device::create(const DeviceDesc& desc, std::string* error) {
    return desc.api == Api::D3D12 ? createD3D12Device(desc, error) : createVulkanDevice(desc, error);
}

}  // namespace taelia::rhi
