// NVIDIA Streamline, used only when the SDK was present at build time and its DLLs
// sit next to the program. Backends then take their entry points from the interposer.
#pragma once

#include "Rhi/Rhi.h"

namespace Teinora::Rhi
{
	/** Loads and starts Streamline for a backend. Must run before any graphics call. Returns false if unavailable. */
	bool StartStreamline(EBackend Backend);

	/** Finds an entry point the interposer replaces, such as D3D12CreateDevice. Null if Streamline is not running. */
	void* GetStreamlineExport(const char* Name);

	/** Names the ID3D12Device to use. Must follow its creation at once, before anything else touches it. */
	void SetStreamlineDevice(void* NativeDevice);

	/** What identifies the GPU in use: an adapter LUID on D3D12, a VkPhysicalDevice on Vulkan. */
	struct StreamlineAdapter
	{
		const void* Luid = nullptr;
		std::uint32_t LuidSize = 0;
		void* PhysicalDevice = nullptr;
	};

	/** Call once the device exists. Shutting Streamline down is left to the returned object's destructor. */
	std::unique_ptr<VendorFeatures> CreateStreamlineFeatures(Device& Owner, const StreamlineAdapter& Adapter);
}
