// The VMA implementation (dynamic Vulkan functions through volk's loaders).
#include <volk.h>

#pragma warning(push, 0)
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#pragma warning(pop)
