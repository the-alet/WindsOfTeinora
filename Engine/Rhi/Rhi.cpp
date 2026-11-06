#include "Rhi/Rhi.h"

#include "Core/Log.h"

namespace Teinora::Rhi
{
	const char* GetBackendName(EBackend Backend)
	{
		switch (Backend)
		{
		case EBackend::D3D12:  return "Direct3D 12";
		case EBackend::Vulkan: return "Vulkan";
		default:               return "Auto";
		}
	}

	std::unique_ptr<Device> CreateDevice(const DeviceDesc& Desc)
	{
		if (Desc.Backend == EBackend::D3D12)
		{
			return CreateD3D12Device(Desc);
		}
		if (Desc.Backend == EBackend::Vulkan)
		{
			return CreateVulkanDevice(Desc);
		}

		std::unique_ptr<Device> Result = CreateD3D12Device(Desc);
		if (Result == nullptr)
		{
			Log("Direct3D 12 is unavailable, falling back to Vulkan");
			Result = CreateVulkanDevice(Desc);
		}
		return Result;
	}
}
