#include "Rhi/Rhi.h"

#include "Core/Log.h"
#include "Rhi/Streamline.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>

// The loader comes with the GPU driver and is opened at run time, so neither
// building nor starting the engine needs the Vulkan SDK.
#define TEINORA_VK_GLOBAL_FUNCTIONS(X) \
	X(vkCreateInstance) \
	X(vkEnumerateInstanceLayerProperties)

#define TEINORA_VK_INSTANCE_FUNCTIONS(X) \
	X(vkDestroyInstance) \
	X(vkEnumeratePhysicalDevices) \
	X(vkEnumerateDeviceExtensionProperties) \
	X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceMemoryProperties) \
	X(vkGetPhysicalDeviceFeatures2) \
	X(vkGetPhysicalDeviceProperties2) \
	X(vkCreateDevice) \
	X(vkDestroySurfaceKHR) \
	X(vkGetPhysicalDeviceSurfaceSupportKHR) \
	X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
	X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
	X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
	X(vkCreateWin32SurfaceKHR)

#define TEINORA_VK_DEVICE_FUNCTIONS(X) \
	X(vkDestroyDevice) \
	X(vkGetDeviceQueue) \
	X(vkDeviceWaitIdle) \
	X(vkQueueSubmit) \
	X(vkQueuePresentKHR) \
	X(vkCreateSwapchainKHR) \
	X(vkDestroySwapchainKHR) \
	X(vkGetSwapchainImagesKHR) \
	X(vkAcquireNextImageKHR) \
	X(vkCreateImageView) \
	X(vkDestroyImageView) \
	X(vkCreateImage) \
	X(vkDestroyImage) \
	X(vkGetImageMemoryRequirements) \
	X(vkBindImageMemory) \
	X(vkAllocateMemory) \
	X(vkFreeMemory) \
	X(vkMapMemory) \
	X(vkCreateBuffer) \
	X(vkDestroyBuffer) \
	X(vkGetBufferMemoryRequirements) \
	X(vkBindBufferMemory) \
	X(vkCreateSemaphore) \
	X(vkDestroySemaphore) \
	X(vkCreateFence) \
	X(vkDestroyFence) \
	X(vkWaitForFences) \
	X(vkResetFences) \
	X(vkCreateCommandPool) \
	X(vkDestroyCommandPool) \
	X(vkResetCommandPool) \
	X(vkAllocateCommandBuffers) \
	X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) \
	X(vkCreateDescriptorPool) \
	X(vkDestroyDescriptorPool) \
	X(vkResetDescriptorPool) \
	X(vkAllocateDescriptorSets) \
	X(vkUpdateDescriptorSets) \
	X(vkCreateDescriptorSetLayout) \
	X(vkDestroyDescriptorSetLayout) \
	X(vkCreatePipelineLayout) \
	X(vkDestroyPipelineLayout) \
	X(vkCreateShaderModule) \
	X(vkDestroyShaderModule) \
	X(vkCreateGraphicsPipelines) \
	X(vkDestroyPipeline) \
	X(vkCreateSampler) \
	X(vkDestroySampler) \
	X(vkCmdBeginRendering) \
	X(vkCmdEndRendering) \
	X(vkCmdPipelineBarrier2) \
	X(vkCmdBindPipeline) \
	X(vkCmdBindDescriptorSets) \
	X(vkCmdBindVertexBuffers) \
	X(vkCmdBindIndexBuffer) \
	X(vkCmdPushConstants) \
	X(vkCmdSetViewport) \
	X(vkCmdSetScissor) \
	X(vkCmdDraw) \
	X(vkCmdDrawIndexed) \
	X(vkCmdCopyBuffer) \
	X(vkCmdCopyBufferToImage) \
	X(vkCmdCopyImageToBuffer)

// Only there on hardware with ray tracing, so these come from the device once it exists.
#define TEINORA_VK_RAY_FUNCTIONS(X) \
	X(vkGetBufferDeviceAddress) \
	X(vkCreateAccelerationStructureKHR) \
	X(vkDestroyAccelerationStructureKHR) \
	X(vkGetAccelerationStructureBuildSizesKHR) \
	X(vkGetAccelerationStructureDeviceAddressKHR) \
	X(vkCmdBuildAccelerationStructuresKHR)

namespace Teinora::Rhi
{
	namespace
	{
		PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

#define TEINORA_VK_DECLARE(Name) PFN_##Name Name = nullptr;
		TEINORA_VK_GLOBAL_FUNCTIONS(TEINORA_VK_DECLARE)
		TEINORA_VK_INSTANCE_FUNCTIONS(TEINORA_VK_DECLARE)
		TEINORA_VK_DEVICE_FUNCTIONS(TEINORA_VK_DECLARE)
		TEINORA_VK_RAY_FUNCTIONS(TEINORA_VK_DECLARE)
#undef TEINORA_VK_DECLARE

		constexpr std::uint32_t FrameCount = 2;
		constexpr VkDeviceSize RingSize = 64ull << 20;
		constexpr VkDeviceSize ConstantRegion = 8ull << 20; // top of the ring, so constants survive upload bursts
		constexpr VkDeviceSize ConstantAlignment = 256;
		constexpr std::uint32_t TableSetsPerFrame = 2048;

		VkFormat ToVulkan(EFormat Format)
		{
			switch (Format)
			{
			case EFormat::RGBA8Unorm:  return VK_FORMAT_R8G8B8A8_UNORM;
			case EFormat::BGRA8Unorm:  return VK_FORMAT_B8G8R8A8_UNORM;
			case EFormat::RGBA8Snorm:  return VK_FORMAT_R8G8B8A8_SNORM;
			case EFormat::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
			case EFormat::RG16Float:   return VK_FORMAT_R16G16_SFLOAT;
			case EFormat::RG32Float:   return VK_FORMAT_R32G32_SFLOAT;
			case EFormat::RGB32Float:  return VK_FORMAT_R32G32B32_SFLOAT;
			case EFormat::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
			case EFormat::D32Float:    return VK_FORMAT_D32_SFLOAT;
			default:                   return VK_FORMAT_UNDEFINED;
			}
		}

		VkCompareOp ToVulkan(ECompare Compare)
		{
			switch (Compare)
			{
			case ECompare::Greater:      return VK_COMPARE_OP_GREATER;
			case ECompare::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
			case ECompare::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
			default:                     return VK_COMPARE_OP_ALWAYS;
			}
		}

		VkDeviceSize BytesPerPixel(EFormat Format)
		{
			switch (Format)
			{
			case EFormat::RGBA16Float: return 8;
			case EFormat::RG32Float:   return 8;
			case EFormat::RG16Float:   return 4;
			case EFormat::RGB32Float:  return 12;
			case EFormat::RGBA32Float: return 16;
			default:                   return 4;
			}
		}

		VkDeviceSize AlignUp(VkDeviceSize Value, VkDeviceSize Alignment)
		{
			return (Value + Alignment - 1) & ~(Alignment - 1);
		}

		struct BufferSlot
		{
			VkBuffer Buffer = VK_NULL_HANDLE;
			VkDeviceMemory Memory = VK_NULL_HANDLE;
			VkDeviceAddress Address = 0; // only for buffers made with a device address
		};

		struct AccelerationSlot
		{
			VkAccelerationStructureKHR Handle = VK_NULL_HANDLE;
			BufferSlot Storage;
			VkDeviceAddress Address = 0;
			VkDeviceSize Size = 0;
		};

		struct TextureSlot
		{
			VkImage Image = VK_NULL_HANDLE;
			VkImageView View = VK_NULL_HANDLE;
			VkDeviceMemory Memory = VK_NULL_HANDLE; // null for swapchain images
			TextureDesc Desc;
			VkImageLayout Layout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkImageUsageFlags Usage = 0;
		};

		struct PipelineSlot
		{
			VkPipeline Pipeline = VK_NULL_HANDLE;
		};

		struct UploadSpan
		{
			VkBuffer Buffer = VK_NULL_HANDLE;
			VkDeviceSize Offset = 0;
			std::uint8_t* Data = nullptr;
			VkDeviceAddress Address = 0;
		};

		struct FrameState
		{
			VkCommandPool Pool = VK_NULL_HANDLE;
			VkCommandBuffer Commands = VK_NULL_HANDLE;
			VkCommandBuffer Uploads = VK_NULL_HANDLE;
			VkFence Fence = VK_NULL_HANDLE;
			VkSemaphore ImageAcquired = VK_NULL_HANDLE;
			BufferSlot Ring;
			std::uint8_t* RingData = nullptr;
			VkDeviceSize RingOffset = 0;
			VkDeviceSize ConstantOffset = 0;
			VkDescriptorSet ConstantSet = VK_NULL_HANDLE;
			VkDescriptorPool TablePool = VK_NULL_HANDLE;
			std::vector<BufferSlot> GarbageBuffers;
			std::vector<TextureSlot> GarbageTextures;
			std::vector<AccelerationSlot> GarbageAccelerations;

			VkDescriptorSet SceneSet = VK_NULL_HANDLE;
			AccelerationSlot Scene;
			BufferSlot SceneScratch;
			VkDeviceSize SceneScratchSize = 0;
		};

		VkImageLayout ReadLayout(EFormat Format)
		{
			return Format == EFormat::D32Float ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}

		class VulkanDevice final : public Device
		{
		public:
			~VulkanDevice() override
			{
				if (Logical == VK_NULL_HANDLE)
				{
					if (Instance != VK_NULL_HANDLE)
					{
						vkDestroyInstance(Instance, nullptr);
					}
					return;
				}

				vkDeviceWaitIdle(Logical);
				Vendor.reset(); // Streamline must shut down before the device goes
				DestroySwapchain();

				for (FrameState& Frame : Frames)
				{
					FreeGarbage(Frame);
					FreeAcceleration(Frame.Scene);
					FreeBuffer(Frame.SceneScratch);
					FreeBuffer(Frame.Ring);
					vkDestroyDescriptorPool(Logical, Frame.TablePool, nullptr);
					vkDestroySemaphore(Logical, Frame.ImageAcquired, nullptr);
					vkDestroyFence(Logical, Frame.Fence, nullptr);
					vkDestroyCommandPool(Logical, Frame.Pool, nullptr);
				}
				for (AccelerationSlot& Slot : Accelerations)
				{
					FreeAcceleration(Slot);
				}
				for (BufferSlot& Slot : Buffers)
				{
					FreeBuffer(Slot);
				}
				for (TextureSlot& Slot : Textures)
				{
					FreeTexture(Slot);
				}
				for (PipelineSlot& Slot : Pipelines)
				{
					vkDestroyPipeline(Logical, Slot.Pipeline, nullptr);
				}
				for (const VkSampler Sampler : Samplers)
				{
					vkDestroySampler(Logical, Sampler, nullptr);
				}
				FreeBuffer(EmptyBuffer);
				vkDestroyDescriptorPool(Logical, ConstantPool, nullptr);
				vkDestroyDescriptorPool(Logical, ScenePool, nullptr);
				vkDestroyPipelineLayout(Logical, PipelineLayout, nullptr);
				vkDestroyDescriptorSetLayout(Logical, ConstantLayout, nullptr);
				vkDestroyDescriptorSetLayout(Logical, TableLayout, nullptr);
				vkDestroyDescriptorSetLayout(Logical, SceneLayout, nullptr);
				vkDestroyDevice(Logical, nullptr);
				vkDestroySurfaceKHR(Instance, Surface, nullptr);
				if (DebugMessenger != VK_NULL_HANDLE)
				{
					vkDestroyDebugUtilsMessenger(Instance, DebugMessenger, nullptr);
				}
				vkDestroyInstance(Instance, nullptr);
			}

			bool Init(const DeviceDesc& Desc)
			{
				SwapWidth = Desc.Width;
				SwapHeight = Desc.Height;

				if (!LoadLoader(Desc) || !CreateInstance(Desc) || !CreateSurface(Desc) || !PickPhysicalDevice() || !CreateLogicalDevice())
				{
					return false;
				}
				if (!CreateLayouts() || !CreateFrames() || !CreateSwapchain())
				{
					return false;
				}

				TextureDesc WhiteDesc;
				const std::uint32_t WhitePixel = 0xFFFFFFFFu;
				WhiteTexture = CreateTexture(WhiteDesc, &WhitePixel);

				if (GetStreamlineExport("slInit") != nullptr)
				{
					StreamlineAdapter Adapter;
					Adapter.PhysicalDevice = Physical;
					Vendor = CreateStreamlineFeatures(*this, Adapter);
				}
				return true;
			}

			EBackend GetBackend() const override { return EBackend::Vulkan; }
			const std::string& GetAdapterName() const override { return AdapterName; }
			VendorFeatures* GetVendorFeatures() override { return Vendor.get(); }

			NativeTexture AccessForExternal(TextureHandle Handle, bool bWrite) override
			{
				TextureSlot& Slot = Textures[Handle.Id - 1];
				Transition(Commands, Slot, bWrite ? VK_IMAGE_LAYOUT_GENERAL : ReadLayout(Slot.Desc.Format));

				NativeTexture Result;
				Result.Resource = Slot.Image;
				Result.Memory = Slot.Memory;
				Result.View = Slot.View;
				Result.State = static_cast<std::uint32_t>(Slot.Layout);
				Result.Format = static_cast<std::uint32_t>(ToVulkan(Slot.Desc.Format));
				Result.Usage = Slot.Usage;
				Result.Width = Slot.Desc.Width;
				Result.Height = Slot.Desc.Height;
				return Result;
			}

			void* GetNativeCommandList() override { return Commands; }

			void RestoreAfterExternal() override
			{
				BindConstantSet();
			}

			BufferHandle CreateBuffer(std::size_t Size, const void* Data) override
			{
				VkBufferUsageFlags Usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
				if (bRayTracing)
				{
					Usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
				}

				BufferSlot Slot = MakeBuffer(Size, Usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
				if (Slot.Buffer == VK_NULL_HANDLE)
				{
					Log("Vulkan: failed to create a buffer of %zu bytes", Size);
					return {};
				}

				const UploadSpan Upload = AllocateUpload(Size, 16);
				std::memcpy(Upload.Data, Data, Size);

				const VkCommandBuffer UploadCommands = CurrentFrame().Uploads;
				const VkBufferCopy Region = { Upload.Offset, 0, Size };
				vkCmdCopyBuffer(UploadCommands, Upload.Buffer, Slot.Buffer, 1, &Region);

				VkMemoryBarrier2 Barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
				Barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
				Barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
				Barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
				Barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
				if (bRayTracing)
				{
					Barrier.dstStageMask |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
					Barrier.dstAccessMask |= VK_ACCESS_2_SHADER_READ_BIT;
				}
				VkDependencyInfo Dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				Dependency.memoryBarrierCount = 1;
				Dependency.pMemoryBarriers = &Barrier;
				vkCmdPipelineBarrier2(UploadCommands, &Dependency);

				return { Store(Buffers, FreeBuffers, Slot) };
			}

			void DestroyBuffer(BufferHandle Handle) override
			{
				CurrentFrame().GarbageBuffers.push_back(Buffers[Handle.Id - 1]);
				Buffers[Handle.Id - 1] = {};
				FreeBuffers.push_back(Handle.Id - 1);
			}

			TextureHandle CreateTexture(const TextureDesc& Desc, const void* Pixels) override
			{
				const bool bDepth = Desc.Format == EFormat::D32Float;

				TextureSlot Slot;
				Slot.Desc = Desc;

				VkImageCreateInfo ImageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
				ImageInfo.imageType = Desc.Depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
				ImageInfo.format = ToVulkan(Desc.Format);
				ImageInfo.extent = { Desc.Width, Desc.Height, Desc.Depth };
				ImageInfo.mipLevels = Desc.MipLevels;
				ImageInfo.arrayLayers = Desc.Layers;
				ImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
				ImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
				ImageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
				if (Desc.bRenderTarget)
				{
					ImageInfo.usage |= bDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
				}
				if (Desc.bUnorderedAccess)
				{
					ImageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
				}
				Slot.Usage = ImageInfo.usage;
				if (vkCreateImage(Logical, &ImageInfo, nullptr, &Slot.Image) != VK_SUCCESS)
				{
					Log("Vulkan: failed to create a %ux%u texture", Desc.Width, Desc.Height);
					return {};
				}

				VkMemoryRequirements Requirements;
				vkGetImageMemoryRequirements(Logical, Slot.Image, &Requirements);
				Slot.Memory = AllocateMemory(Requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
				vkBindImageMemory(Logical, Slot.Image, Slot.Memory, 0);
				Slot.View = MakeView(Slot.Image, ImageInfo.format, bDepth, Desc.MipLevels, Desc.Layers, Desc.Depth > 1);

				if (Pixels != nullptr)
				{
					RecordPixels(Slot, Pixels);
				}

				return { Store(Textures, FreeTextures, Slot) };
			}

			void UpdateTexture(TextureHandle Handle, const void* Pixels) override
			{
				RecordPixels(Textures[Handle.Id - 1], Pixels);
			}

			void DestroyTexture(TextureHandle Handle) override
			{
				CurrentFrame().GarbageTextures.push_back(Textures[Handle.Id - 1]);
				Textures[Handle.Id - 1] = {};
				FreeTextures.push_back(Handle.Id - 1);
			}

			PipelineHandle CreatePipeline(const PipelineDesc& Desc) override
			{
				const auto MakeModule = [this](const ShaderBytes& Shader)
				{
					VkShaderModuleCreateInfo Info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
					Info.codeSize = Shader.Size;
					Info.pCode = static_cast<const std::uint32_t*>(Shader.Data);
					VkShaderModule Module = VK_NULL_HANDLE;
					vkCreateShaderModule(Logical, &Info, nullptr, &Module);
					return Module;
				};

				VkPipelineShaderStageCreateInfo Stages[2] = {};
				std::uint32_t StageCount = 1;
				Stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
				Stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
				Stages[0].module = MakeModule(Desc.Vertex);
				Stages[0].pName = "main";
				if (Desc.Pixel.Data != nullptr)
				{
					StageCount = 2;
					Stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
					Stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
					Stages[1].module = MakeModule(Desc.Pixel);
					Stages[1].pName = "main";
				}

				std::vector<VkVertexInputBindingDescription> Bindings;
				for (std::size_t Index = 0; Index < Desc.Streams.size(); ++Index)
				{
					Bindings.push_back({ static_cast<std::uint32_t>(Index), Desc.Streams[Index].Stride,
						Desc.Streams[Index].bPerInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX });
				}
				std::vector<VkVertexInputAttributeDescription> Attributes;
				for (std::size_t Index = 0; Index < Desc.Attributes.size(); ++Index)
				{
					const VertexAttribute& Attribute = Desc.Attributes[Index];
					Attributes.push_back({ static_cast<std::uint32_t>(Index), Attribute.Stream, ToVulkan(Attribute.Format), Attribute.Offset });
				}

				VkPipelineVertexInputStateCreateInfo VertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
				VertexInput.vertexBindingDescriptionCount = static_cast<std::uint32_t>(Bindings.size());
				VertexInput.pVertexBindingDescriptions = Bindings.data();
				VertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(Attributes.size());
				VertexInput.pVertexAttributeDescriptions = Attributes.data();

				VkPipelineInputAssemblyStateCreateInfo Assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
				Assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

				VkPipelineViewportStateCreateInfo ViewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
				ViewportState.viewportCount = 1;
				ViewportState.scissorCount = 1;

				// With the flipped viewport, on-screen winding matches D3D's.
				VkPipelineRasterizationStateCreateInfo Raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
				Raster.polygonMode = VK_POLYGON_MODE_FILL;
				Raster.cullMode = Desc.Cull == ECull::None ? VK_CULL_MODE_NONE : (Desc.Cull == ECull::Back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT);
				Raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
				Raster.depthBiasEnable = Desc.DepthBias != 0 || Desc.SlopeDepthBias != 0.0f;
				Raster.depthBiasConstantFactor = static_cast<float>(Desc.DepthBias);
				Raster.depthBiasSlopeFactor = Desc.SlopeDepthBias;
				Raster.lineWidth = 1.0f;

				VkPipelineMultisampleStateCreateInfo Multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
				Multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

				VkPipelineDepthStencilStateCreateInfo Depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
				Depth.depthTestEnable = Desc.bDepthTest ? VK_TRUE : VK_FALSE;
				Depth.depthWriteEnable = Desc.bDepthWrite ? VK_TRUE : VK_FALSE;
				Depth.depthCompareOp = ToVulkan(Desc.DepthCompare);

				VkPipelineColorBlendAttachmentState BlendAttachment = {};
				BlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
				if (Desc.Blend != EBlend::Opaque)
				{
					BlendAttachment.blendEnable = VK_TRUE;
					BlendAttachment.srcColorBlendFactor = Desc.Blend == EBlend::Alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
					BlendAttachment.dstColorBlendFactor = Desc.Blend == EBlend::Alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
					BlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
					BlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
					BlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
					BlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
				}

				const bool bHasColor = Desc.ColorFormat != EFormat::Unknown;
				VkPipelineColorBlendStateCreateInfo Blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
				Blend.attachmentCount = bHasColor ? 1 : 0;
				Blend.pAttachments = &BlendAttachment;

				const VkDynamicState DynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
				VkPipelineDynamicStateCreateInfo Dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
				Dynamic.dynamicStateCount = 2;
				Dynamic.pDynamicStates = DynamicStates;

				const VkFormat ColorFormat = ToVulkan(Desc.ColorFormat);
				VkPipelineRenderingCreateInfo Rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
				Rendering.colorAttachmentCount = bHasColor ? 1 : 0;
				Rendering.pColorAttachmentFormats = &ColorFormat;
				Rendering.depthAttachmentFormat = ToVulkan(Desc.DepthFormat);

				VkGraphicsPipelineCreateInfo Info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
				Info.pNext = &Rendering;
				Info.stageCount = StageCount;
				Info.pStages = Stages;
				Info.pVertexInputState = &VertexInput;
				Info.pInputAssemblyState = &Assembly;
				Info.pViewportState = &ViewportState;
				Info.pRasterizationState = &Raster;
				Info.pMultisampleState = &Multisample;
				Info.pDepthStencilState = &Depth;
				Info.pColorBlendState = &Blend;
				Info.pDynamicState = &Dynamic;
				Info.layout = PipelineLayout;

				PipelineSlot Slot;
				const VkResult Result = vkCreateGraphicsPipelines(Logical, VK_NULL_HANDLE, 1, &Info, nullptr, &Slot.Pipeline);
				for (std::uint32_t Index = 0; Index < StageCount; ++Index)
				{
					vkDestroyShaderModule(Logical, Stages[Index].module, nullptr);
				}
				if (Result != VK_SUCCESS)
				{
					Log("Vulkan: vkCreateGraphicsPipelines failed: %d", static_cast<int>(Result));
					return {};
				}
				return { Store(Pipelines, FreePipelines, Slot) };
			}

			bool SupportsRayTracing() const override { return bRayTracing; }

			AccelerationHandle CreateAcceleration(BufferHandle Vertices, std::uint32_t Stride, std::uint32_t VertexCount) override
			{
				VkAccelerationStructureGeometryKHR Geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
				Geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
				Geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
				VkAccelerationStructureGeometryTrianglesDataKHR& Triangles = Geometry.geometry.triangles;
				Triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
				Triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
				Triangles.vertexData.deviceAddress = Buffers[Vertices.Id - 1].Address;
				Triangles.vertexStride = Stride;
				Triangles.maxVertex = VertexCount - 1;
				Triangles.indexType = VK_INDEX_TYPE_NONE_KHR;

				VkAccelerationStructureBuildGeometryInfoKHR Build = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
				Build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
				Build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
				Build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
				Build.geometryCount = 1;
				Build.pGeometries = &Geometry;

				const std::uint32_t TriangleCount = VertexCount / 3;
				VkAccelerationStructureBuildSizesInfoKHR Sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
				vkGetAccelerationStructureBuildSizesKHR(Logical, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &Build, &TriangleCount, &Sizes);

				const AccelerationSlot Slot = MakeAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, Sizes.accelerationStructureSize);
				const BufferSlot Scratch = MakeScratch(Sizes.buildScratchSize);
				Build.dstAccelerationStructure = Slot.Handle;
				Build.scratchData.deviceAddress = AlignUp(Scratch.Address, ScratchAlignment);

				const VkAccelerationStructureBuildRangeInfoKHR Range = { TriangleCount, 0, 0, 0 };
				const VkAccelerationStructureBuildRangeInfoKHR* Ranges = &Range;

				FrameState& Frame = CurrentFrame();
				vkCmdBuildAccelerationStructuresKHR(Frame.Uploads, 1, &Build, &Ranges);
				RecordFullBarrier(Frame.Uploads);

				Frame.GarbageBuffers.push_back(Scratch);
				return { Store(Accelerations, FreeAccelerations, Slot) };
			}

			void DestroyAcceleration(AccelerationHandle Handle) override
			{
				CurrentFrame().GarbageAccelerations.push_back(Accelerations[Handle.Id - 1]);
				Accelerations[Handle.Id - 1] = {};
				FreeAccelerations.push_back(Handle.Id - 1);
			}

			void SetRayScene(const RayInstance* Instances, std::uint32_t Count, const BufferHandle* VertexBuffers, std::uint32_t BufferCount) override
			{
				FrameState& Frame = CurrentFrame();

				const UploadSpan Upload = AllocateUpload(std::max<VkDeviceSize>(Count, 1) * sizeof(VkAccelerationStructureInstanceKHR), 16);
				for (std::uint32_t Index = 0; Index < Count; ++Index)
				{
					const RayInstance& Item = Instances[Index];
					VkAccelerationStructureInstanceKHR Desc = {};
					std::memcpy(Desc.transform.matrix, Item.Transform, sizeof(Desc.transform.matrix));
					Desc.instanceCustomIndex = Item.Index;
					Desc.mask = 0xFF;
					Desc.flags = Item.bCutOut ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : 0u;
					Desc.accelerationStructureReference = Accelerations[Item.Geometry.Id - 1].Address;
					std::memcpy(Upload.Data + Index * sizeof(Desc), &Desc, sizeof(Desc));
				}

				VkAccelerationStructureGeometryKHR Geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
				Geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
				Geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
				Geometry.geometry.instances.data.deviceAddress = Upload.Address;

				VkAccelerationStructureBuildGeometryInfoKHR Build = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
				Build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
				Build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
				Build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
				Build.geometryCount = 1;
				Build.pGeometries = &Geometry;

				VkAccelerationStructureBuildSizesInfoKHR Sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
				vkGetAccelerationStructureBuildSizesKHR(Logical, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &Build, &Count, &Sizes);

				// Each frame slot keeps its own scene, grown with some room to spare.
				if (Frame.Scene.Size < Sizes.accelerationStructureSize)
				{
					if (Frame.Scene.Handle != VK_NULL_HANDLE)
					{
						Frame.GarbageAccelerations.push_back(Frame.Scene);
					}
					Frame.Scene = MakeAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, Sizes.accelerationStructureSize + Sizes.accelerationStructureSize / 2);
				}
				if (Frame.SceneScratchSize < Sizes.buildScratchSize)
				{
					if (Frame.SceneScratch.Buffer != VK_NULL_HANDLE)
					{
						Frame.GarbageBuffers.push_back(Frame.SceneScratch);
					}
					Frame.SceneScratchSize = Sizes.buildScratchSize + Sizes.buildScratchSize / 2;
					Frame.SceneScratch = MakeScratch(Frame.SceneScratchSize);
				}

				Build.dstAccelerationStructure = Frame.Scene.Handle;
				Build.scratchData.deviceAddress = AlignUp(Frame.SceneScratch.Address, ScratchAlignment);

				const VkAccelerationStructureBuildRangeInfoKHR Range = { Count, 0, 0, 0 };
				const VkAccelerationStructureBuildRangeInfoKHR* Ranges = &Range;
				vkCmdBuildAccelerationStructuresKHR(Commands, 1, &Build, &Ranges);
				RecordFullBarrier(Commands);

				VkWriteDescriptorSetAccelerationStructureKHR SceneInfo = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
				SceneInfo.accelerationStructureCount = 1;
				SceneInfo.pAccelerationStructures = &Frame.Scene.Handle;

				// Every element is written, since an unused one must not keep naming a freed buffer.
				std::vector<VkDescriptorBufferInfo> BufferInfos(MaxRayBuffers, VkDescriptorBufferInfo{ EmptyBuffer.Buffer, 0, VK_WHOLE_SIZE });
				for (std::uint32_t Index = 0; Index < BufferCount; ++Index)
				{
					BufferInfos[Index].buffer = Buffers[VertexBuffers[Index].Id - 1].Buffer;
				}

				VkWriteDescriptorSet Writes[2] = {};
				Writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				Writes[0].pNext = &SceneInfo;
				Writes[0].dstSet = Frame.SceneSet;
				Writes[0].dstBinding = 0;
				Writes[0].descriptorCount = 1;
				Writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
				Writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				Writes[1].dstSet = Frame.SceneSet;
				Writes[1].dstBinding = 1;
				Writes[1].descriptorCount = MaxRayBuffers;
				Writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				Writes[1].pBufferInfo = BufferInfos.data();
				vkUpdateDescriptorSets(Logical, 2, Writes, 0, nullptr);

				vkCmdBindDescriptorSets(Commands, VK_PIPELINE_BIND_POINT_GRAPHICS, PipelineLayout, 2, 1, &Frame.SceneSet, 0, nullptr);
			}

			void Resize(std::uint32_t Width, std::uint32_t Height) override
			{
				SwapWidth = Width;
				SwapHeight = Height;
				WaitIdle();
				DestroySwapchain();
				CreateSwapchain();
			}

			void WaitIdle() override
			{
				vkDeviceWaitIdle(Logical);
			}

			void BeginFrame() override
			{
				FrameState& Frame = CurrentFrame();

				VkResult Result = vkAcquireNextImageKHR(Logical, Swapchain, UINT64_MAX, Frame.ImageAcquired, VK_NULL_HANDLE, &ImageIndex);
				if (Result == VK_ERROR_OUT_OF_DATE_KHR)
				{
					Resize(SwapWidth, SwapHeight);
					Result = vkAcquireNextImageKHR(Logical, Swapchain, UINT64_MAX, Frame.ImageAcquired, VK_NULL_HANDLE, &ImageIndex);
				}
				if (Result != VK_SUCCESS && Result != VK_SUBOPTIMAL_KHR)
				{
					Log("Vulkan: vkAcquireNextImageKHR failed: %d", static_cast<int>(Result));
				}

				if (Vendor != nullptr)
				{
					Vendor->BeginRenderSubmit();
				}

				Commands = Frame.Commands;
				VkCommandBufferBeginInfo Begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
				Begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
				vkBeginCommandBuffer(Commands, &Begin);

				vkResetDescriptorPool(Logical, Frame.TablePool, 0);
				ConstantOffsets[0] = 0;
				ConstantOffsets[1] = 0;
				BindConstantSet();
			}

			void EndFrame() override
			{
				FrameState& Frame = CurrentFrame();
				TextureSlot& Back = Textures[Backbuffers[ImageIndex].Id - 1];

				if (bCaptureRequested)
				{
					RecordCapture(Back);
				}
				Transition(Commands, Back, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

				vkEndCommandBuffer(Commands);
				vkEndCommandBuffer(Frame.Uploads);

				const VkCommandBuffer Buffers2[] = { Frame.Uploads, Commands };
				const VkPipelineStageFlags WaitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
				VkSubmitInfo Submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
				Submit.waitSemaphoreCount = 1;
				Submit.pWaitSemaphores = &Frame.ImageAcquired;
				Submit.pWaitDstStageMask = &WaitStage;
				Submit.commandBufferCount = 2;
				Submit.pCommandBuffers = Buffers2;
				Submit.signalSemaphoreCount = 1;
				Submit.pSignalSemaphores = &RenderFinished[ImageIndex];
				vkQueueSubmit(Queue, 1, &Submit, Frame.Fence);

				if (Vendor != nullptr)
				{
					Vendor->EndRenderSubmit();
					Vendor->BeginPresent();
				}

				VkPresentInfoKHR Present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
				Present.waitSemaphoreCount = 1;
				Present.pWaitSemaphores = &RenderFinished[ImageIndex];
				Present.swapchainCount = 1;
				Present.pSwapchains = &Swapchain;
				Present.pImageIndices = &ImageIndex;
				const VkResult Result = vkQueuePresentKHR(Queue, &Present);
				if (Vendor != nullptr)
				{
					Vendor->EndPresent();
				}
				if (Result != VK_SUCCESS && Result != VK_SUBOPTIMAL_KHR && Result != VK_ERROR_OUT_OF_DATE_KHR)
				{
					Log("Vulkan: vkQueuePresentKHR failed: %d", static_cast<int>(Result));
				}

				if (bCaptureRequested)
				{
					vkWaitForFences(Logical, 1, &Frame.Fence, VK_TRUE, UINT64_MAX);
					ResolveCapture();
				}

				FrameIndex = (FrameIndex + 1) % FrameCount;
				FrameState& Next = CurrentFrame();
				vkWaitForFences(Logical, 1, &Next.Fence, VK_TRUE, UINT64_MAX);
				vkResetFences(Logical, 1, &Next.Fence);
				FreeGarbage(Next);
				vkResetCommandPool(Logical, Next.Pool, 0);
				Next.RingOffset = 0;
				Next.ConstantOffset = RingSize - ConstantRegion;
				BeginUploads(Next);
				Commands = VK_NULL_HANDLE;
			}

			TextureHandle GetBackbuffer() const override { return Backbuffers[ImageIndex]; }
			EFormat GetBackbufferFormat() const override { return EFormat::BGRA8Unorm; }

			void BeginPass(const RenderPass& Pass) override
			{
				for (const TextureHandle Read : Pass.Reads)
				{
					if (Read)
					{
						TextureSlot& Slot = Textures[Read.Id - 1];
						Transition(Commands, Slot, ReadLayout(Slot.Desc.Format));
					}
				}

				VkRenderingAttachmentInfo Color = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
				VkRenderingAttachmentInfo Depth = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
				std::uint32_t Width = 1;
				std::uint32_t Height = 1;

				if (Pass.Color)
				{
					TextureSlot& Slot = Textures[Pass.Color.Id - 1];
					Transition(Commands, Slot, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
					Color.imageView = Slot.View;
					Color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
					Color.loadOp = Pass.bClearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
					Color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
					std::memcpy(Color.clearValue.color.float32, Pass.ClearColor, sizeof(Pass.ClearColor));
					Width = Slot.Desc.Width;
					Height = Slot.Desc.Height;
				}
				if (Pass.Depth)
				{
					TextureSlot& Slot = Textures[Pass.Depth.Id - 1];
					const VkImageLayout Layout = Pass.bDepthReadOnly ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
					Transition(Commands, Slot, Layout);
					Depth.imageView = Slot.View;
					Depth.imageLayout = Layout;
					Depth.loadOp = Pass.bClearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
					Depth.storeOp = Pass.bDepthReadOnly ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
					Depth.clearValue.depthStencil.depth = Pass.ClearDepth;
					Width = Slot.Desc.Width;
					Height = Slot.Desc.Height;
				}

				VkRenderingInfo Info = { VK_STRUCTURE_TYPE_RENDERING_INFO };
				Info.renderArea.extent = { Width, Height };
				Info.layerCount = 1;
				Info.colorAttachmentCount = Pass.Color ? 1 : 0;
				Info.pColorAttachments = &Color;
				Info.pDepthAttachment = Pass.Depth ? &Depth : nullptr;
				vkCmdBeginRendering(Commands, &Info);

				SetViewport(0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height));
			}

			void EndPass() override
			{
				vkCmdEndRendering(Commands);
			}

			void SetPipeline(PipelineHandle Handle) override
			{
				vkCmdBindPipeline(Commands, VK_PIPELINE_BIND_POINT_GRAPHICS, Pipelines[Handle.Id - 1].Pipeline);
			}

			void SetViewport(float X, float Y, float Width, float Height) override
			{
				// A negative height gives the same Y-up clip space as D3D.
				const VkViewport Viewport = { X, Y + Height, Width, -Height, 0.0f, 1.0f };
				const VkRect2D Scissor = { { static_cast<std::int32_t>(X), static_cast<std::int32_t>(Y) }, { static_cast<std::uint32_t>(Width), static_cast<std::uint32_t>(Height) } };
				vkCmdSetViewport(Commands, 0, 1, &Viewport);
				vkCmdSetScissor(Commands, 0, 1, &Scissor);
			}

			void SetVertexBuffer(std::uint32_t Stream, BufferHandle Handle) override
			{
				const VkDeviceSize Offset = 0;
				vkCmdBindVertexBuffers(Commands, Stream, 1, &Buffers[Handle.Id - 1].Buffer, &Offset);
			}

			void* SetTransientVertexBuffer(std::uint32_t Stream, std::size_t Size) override
			{
				const UploadSpan Span = AllocateUpload(Size, 16);
				vkCmdBindVertexBuffers(Commands, Stream, 1, &Span.Buffer, &Span.Offset);
				return Span.Data;
			}

			void SetIndexBuffer(BufferHandle Handle) override
			{
				vkCmdBindIndexBuffer(Commands, Buffers[Handle.Id - 1].Buffer, 0, VK_INDEX_TYPE_UINT32);
			}

			void SetConstants(std::uint32_t Slot, const void* Data, std::size_t Size) override
			{
				FrameState& Frame = CurrentFrame();
				const VkDeviceSize Offset = Frame.ConstantOffset;
				Frame.ConstantOffset = AlignUp(Offset + Size, ConstantAlignment);
				std::memcpy(Frame.RingData + Offset, Data, Size);
				ConstantOffsets[Slot] = static_cast<std::uint32_t>(Offset);
				BindConstantSet();
			}

			void SetPushConstants(const void* Data, std::size_t Size) override
			{
				vkCmdPushConstants(Commands, PipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, static_cast<std::uint32_t>(Size), Data);
			}

			void SetTextures(const TextureHandle* Handles, int Count) override
			{
				VkDescriptorSetAllocateInfo Allocate = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
				Allocate.descriptorPool = CurrentFrame().TablePool;
				Allocate.descriptorSetCount = 1;
				Allocate.pSetLayouts = &TableLayout;
				VkDescriptorSet Set = VK_NULL_HANDLE;
				vkAllocateDescriptorSets(Logical, &Allocate, &Set);

				VkDescriptorImageInfo Images[MaxTextureBindings] = {};
				for (int Index = 0; Index < MaxTextureBindings; ++Index)
				{
					const TextureHandle Handle = Index < Count && Handles[Index] ? Handles[Index] : WhiteTexture;
					const TextureSlot& Slot = Textures[Handle.Id - 1];
					Images[Index].imageView = Slot.View;
					Images[Index].imageLayout = ReadLayout(Slot.Desc.Format);
				}

				VkWriteDescriptorSet Write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
				Write.dstSet = Set;
				Write.descriptorCount = MaxTextureBindings;
				Write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
				Write.pImageInfo = Images;
				vkUpdateDescriptorSets(Logical, 1, &Write, 0, nullptr);

				vkCmdBindDescriptorSets(Commands, VK_PIPELINE_BIND_POINT_GRAPHICS, PipelineLayout, 1, 1, &Set, 0, nullptr);
			}

			void Draw(std::uint32_t Vertices, std::uint32_t Instances, std::uint32_t FirstVertex) override
			{
				vkCmdDraw(Commands, Vertices, Instances, FirstVertex, 0);
			}

			void DrawIndexed(std::uint32_t Indices, std::uint32_t Instances) override
			{
				vkCmdDrawIndexed(Commands, Indices, Instances, 0, 0, 0);
			}

			void RequestCapture() override { bCaptureRequested = true; }

			bool TakeCapture(std::vector<std::uint8_t>& OutPixels, std::uint32_t& OutWidth, std::uint32_t& OutHeight) override
			{
				if (CapturePixels.empty())
				{
					return false;
				}
				OutPixels = std::move(CapturePixels);
				CapturePixels.clear();
				OutWidth = SwapWidth;
				OutHeight = SwapHeight;
				return true;
			}

		private:
			template <typename SlotType>
			static std::uint32_t Store(std::vector<SlotType>& Slots, std::vector<std::uint32_t>& FreeSlots, const SlotType& Slot)
			{
				if (!FreeSlots.empty())
				{
					const std::uint32_t Index = FreeSlots.back();
					FreeSlots.pop_back();
					Slots[Index] = Slot;
					return Index + 1;
				}
				Slots.push_back(Slot);
				return static_cast<std::uint32_t>(Slots.size());
			}

			FrameState& CurrentFrame() { return Frames[FrameIndex]; }

			bool LoadLoader(const DeviceDesc& Desc)
			{
				const HMODULE Library = LoadLibraryW(L"vulkan-1.dll");
				if (Library == nullptr)
				{
					Log("Vulkan: vulkan-1.dll is not installed");
					return false;
				}
				vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(Library, "vkGetInstanceProcAddr")));

				// Streamline stands in for the loader, adding what its features need to the instance and device.
				if (Desc.bVendorFeatures && StartStreamline(EBackend::Vulkan))
				{
					vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetStreamlineExport("vkGetInstanceProcAddr"));
				}

				if (vkGetInstanceProcAddr == nullptr)
				{
					return false;
				}
#define TEINORA_VK_LOAD(Name) Name = reinterpret_cast<PFN_##Name>(vkGetInstanceProcAddr(VK_NULL_HANDLE, #Name));
				TEINORA_VK_GLOBAL_FUNCTIONS(TEINORA_VK_LOAD)
#undef TEINORA_VK_LOAD
				return vkCreateInstance != nullptr;
			}

			bool CreateInstance(const DeviceDesc& Desc)
			{
				VkApplicationInfo Application = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
				Application.pApplicationName = "Winds of Teinora";
				Application.pEngineName = "Teinora";
				Application.apiVersion = VK_API_VERSION_1_3;

				const char* Extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
				const char* Layer = "VK_LAYER_KHRONOS_validation";
				const bool bValidate = Desc.bValidation && HasValidationLayer(Layer);

				VkDebugUtilsMessengerCreateInfoEXT Messenger = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
				Messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
				Messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
				Messenger.pfnUserCallback = &OnValidationMessage;

				VkInstanceCreateInfo Info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
				Info.pApplicationInfo = &Application;
				Info.enabledExtensionCount = bValidate ? 3 : 2;
				Info.ppEnabledExtensionNames = Extensions;
				if (bValidate)
				{
					Info.pNext = &Messenger; // also covers instance creation and destruction
					Info.enabledLayerCount = 1;
					Info.ppEnabledLayerNames = &Layer;
				}

				const VkResult Result = vkCreateInstance(&Info, nullptr, &Instance);
				if (Result != VK_SUCCESS)
				{
					Log("Vulkan: vkCreateInstance failed: %d", static_cast<int>(Result));
					return false;
				}

#define TEINORA_VK_LOAD(Name) Name = reinterpret_cast<PFN_##Name>(vkGetInstanceProcAddr(Instance, #Name));
				TEINORA_VK_INSTANCE_FUNCTIONS(TEINORA_VK_LOAD)
				TEINORA_VK_DEVICE_FUNCTIONS(TEINORA_VK_LOAD)
#undef TEINORA_VK_LOAD

				if (bValidate)
				{
					const auto CreateMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(Instance, "vkCreateDebugUtilsMessengerEXT"));
					vkDestroyDebugUtilsMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(Instance, "vkDestroyDebugUtilsMessengerEXT"));
					CreateMessenger(Instance, &Messenger, nullptr, &DebugMessenger);
					Log("Vulkan: validation layer enabled");
				}
				return true;
			}

			static VKAPI_ATTR VkBool32 VKAPI_CALL OnValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
				const VkDebugUtilsMessengerCallbackDataEXT* Data, void*)
			{
				Log("Vulkan validation: %s", Data->pMessage);
				return VK_FALSE;
			}

			static bool HasValidationLayer(const char* Name)
			{
				std::uint32_t Count = 0;
				vkEnumerateInstanceLayerProperties(&Count, nullptr);
				std::vector<VkLayerProperties> Layers(Count);
				vkEnumerateInstanceLayerProperties(&Count, Layers.data());
				for (const VkLayerProperties& Layer : Layers)
				{
					if (std::strcmp(Layer.layerName, Name) == 0)
					{
						return true;
					}
				}
				Log("Vulkan: validation layer not found, set VK_LAYER_PATH");
				return false;
			}

			bool CreateSurface(const DeviceDesc& Desc)
			{
				VkWin32SurfaceCreateInfoKHR Info = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
				Info.hinstance = GetModuleHandleW(nullptr);
				Info.hwnd = static_cast<HWND>(Desc.NativeWindow);
				return vkCreateWin32SurfaceKHR(Instance, &Info, nullptr, &Surface) == VK_SUCCESS;
			}

			bool PickPhysicalDevice()
			{
				std::uint32_t Count = 0;
				vkEnumeratePhysicalDevices(Instance, &Count, nullptr);
				std::vector<VkPhysicalDevice> Devices(Count);
				vkEnumeratePhysicalDevices(Instance, &Count, Devices.data());

				int BestScore = -1;
				for (const VkPhysicalDevice Candidate : Devices)
				{
					VkPhysicalDeviceProperties Properties;
					vkGetPhysicalDeviceProperties(Candidate, &Properties);
					if (Properties.apiVersion < VK_API_VERSION_1_3)
					{
						continue;
					}

					VkPhysicalDeviceVulkan13Features Features13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
					VkPhysicalDeviceFeatures2 Features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
					Features.pNext = &Features13;
					vkGetPhysicalDeviceFeatures2(Candidate, &Features);
					if (Features13.dynamicRendering == VK_FALSE || Features13.synchronization2 == VK_FALSE)
					{
						continue;
					}

					std::uint32_t FamilyCount = 0;
					vkGetPhysicalDeviceQueueFamilyProperties(Candidate, &FamilyCount, nullptr);
					std::vector<VkQueueFamilyProperties> Families(FamilyCount);
					vkGetPhysicalDeviceQueueFamilyProperties(Candidate, &FamilyCount, Families.data());

					for (std::uint32_t Family = 0; Family < FamilyCount; ++Family)
					{
						VkBool32 bPresent = VK_FALSE;
						vkGetPhysicalDeviceSurfaceSupportKHR(Candidate, Family, Surface, &bPresent);
						if ((Families[Family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && bPresent == VK_TRUE)
						{
							const int Score = Properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
							if (Score > BestScore)
							{
								BestScore = Score;
								Physical = Candidate;
								QueueFamily = Family;
								AdapterName = Properties.deviceName;
							}
							break;
						}
					}
				}

				if (Physical == VK_NULL_HANDLE)
				{
					Log("Vulkan: no GPU with Vulkan 1.3, dynamic rendering and presentation");
					return false;
				}
				vkGetPhysicalDeviceMemoryProperties(Physical, &MemoryProperties);
				return true;
			}

			bool HasDeviceExtension(const char* Name) const
			{
				std::uint32_t Count = 0;
				vkEnumerateDeviceExtensionProperties(Physical, nullptr, &Count, nullptr);
				std::vector<VkExtensionProperties> Extensions(Count);
				vkEnumerateDeviceExtensionProperties(Physical, nullptr, &Count, Extensions.data());
				for (const VkExtensionProperties& Extension : Extensions)
				{
					if (std::strcmp(Extension.extensionName, Name) == 0)
					{
						return true;
					}
				}
				return false;
			}

			bool CreateLogicalDevice()
			{
				const float Priority = 1.0f;
				VkDeviceQueueCreateInfo QueueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
				QueueInfo.queueFamilyIndex = QueueFamily;
				QueueInfo.queueCount = 1;
				QueueInfo.pQueuePriorities = &Priority;

				VkPhysicalDeviceVulkan13Features Features13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
				Features13.dynamicRendering = VK_TRUE;
				Features13.synchronization2 = VK_TRUE;
				Features13.privateData = VK_TRUE; // used by DLSS, and always there in Vulkan 1.3
				Features13.shaderDemoteToHelperInvocation = VK_TRUE; // what clip() in a shader turns into, also always there

				VkPhysicalDeviceRayQueryFeaturesKHR HasRayQuery = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
				VkPhysicalDeviceAccelerationStructureFeaturesKHR HasAcceleration = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &HasRayQuery };
				VkPhysicalDeviceVulkan12Features Has12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &HasAcceleration };
				VkPhysicalDeviceFeatures2 Supported = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &Has12 };
				vkGetPhysicalDeviceFeatures2(Physical, &Supported);

				VkPhysicalDeviceAccelerationStructurePropertiesKHR RayLimits = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
				VkPhysicalDeviceProperties2 Properties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &RayLimits };
				vkGetPhysicalDeviceProperties2(Physical, &Properties);
				const VkPhysicalDeviceLimits& Limits = Properties.properties.limits;

				VkPhysicalDeviceFeatures Enabled = {};
				Enabled.samplerAnisotropy = Supported.features.samplerAnisotropy;
				if (Enabled.samplerAnisotropy == VK_TRUE)
				{
					SamplerAnisotropy = std::min(static_cast<float>(Rhi::MaxAnisotropy), Limits.maxSamplerAnisotropy);
				}

				std::vector<const char*> Extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
				VkDeviceCreateInfo Info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
				Info.pNext = &Features13;
				Info.pEnabledFeatures = &Enabled;
				Info.queueCreateInfoCount = 1;
				Info.pQueueCreateInfos = &QueueInfo;

				// Each optional group of features joins the front of the chain behind Features13.
				const auto AddFeatures = [&Features13](auto& Features)
				{
					Features.pNext = Features13.pNext;
					Features13.pNext = &Features;
				};

				// Streamline adds VK_NV_low_latency2 for Reflex, which depends on present ids.
				VkPhysicalDevicePresentIdFeaturesKHR PresentId = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR };
				if (GetStreamlineExport("slInit") != nullptr && HasDeviceExtension(VK_KHR_PRESENT_ID_EXTENSION_NAME))
				{
					PresentId.presentId = VK_TRUE;
					AddFeatures(PresentId);
					Extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
				}

				VkPhysicalDeviceRayQueryFeaturesKHR RayQuery = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
				VkPhysicalDeviceAccelerationStructureFeaturesKHR Acceleration = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
				VkPhysicalDeviceVulkan12Features Features12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
				bRayTracing = HasRayQuery.rayQuery == VK_TRUE && HasAcceleration.accelerationStructure == VK_TRUE
					&& Has12.bufferDeviceAddress == VK_TRUE && Has12.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE
					&& Limits.maxPerStageDescriptorStorageBuffers >= MaxRayBuffers && Limits.maxDescriptorSetStorageBuffers >= MaxRayBuffers
					&& HasDeviceExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME) && HasDeviceExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
					&& HasDeviceExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
				if (bRayTracing)
				{
					ScratchAlignment = RayLimits.minAccelerationStructureScratchOffsetAlignment;
					RayQuery.rayQuery = VK_TRUE;
					Acceleration.accelerationStructure = VK_TRUE;
					Features12.bufferDeviceAddress = VK_TRUE;
					Features12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
					Enabled.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
					AddFeatures(RayQuery);
					AddFeatures(Acceleration);
					AddFeatures(Features12);
					Extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
					Extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
					Extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
				}

				Info.enabledExtensionCount = static_cast<std::uint32_t>(Extensions.size());
				Info.ppEnabledExtensionNames = Extensions.data();

				const VkResult Result = vkCreateDevice(Physical, &Info, nullptr, &Logical);
				if (Result != VK_SUCCESS)
				{
					Log("Vulkan: vkCreateDevice failed: %d", static_cast<int>(Result));
					return false;
				}
				vkGetDeviceQueue(Logical, QueueFamily, 0, &Queue);

				if (bRayTracing)
				{
					const auto GetDeviceFunction = reinterpret_cast<PFN_vkGetDeviceProcAddr>(vkGetInstanceProcAddr(Instance, "vkGetDeviceProcAddr"));
#define TEINORA_VK_LOAD(Name) Name = reinterpret_cast<PFN_##Name>(GetDeviceFunction(Logical, #Name)); bRayTracing = bRayTracing && Name != nullptr;
					TEINORA_VK_RAY_FUNCTIONS(TEINORA_VK_LOAD)
#undef TEINORA_VK_LOAD
				}
				return true;
			}

			VkSampler MakeSampler(VkFilter Filter, VkSamplerAddressMode Address, bool bCompare, float Anisotropy = 1.0f)
			{
				VkSamplerCreateInfo Info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
				Info.anisotropyEnable = Anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
				Info.maxAnisotropy = Anisotropy;
				Info.magFilter = Filter;
				Info.minFilter = Filter;
				Info.mipmapMode = Filter == VK_FILTER_LINEAR && !bCompare ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
				Info.addressModeU = Address;
				Info.addressModeV = Address;
				Info.addressModeW = Address;
				Info.compareEnable = bCompare ? VK_TRUE : VK_FALSE;
				Info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
				Info.maxLod = VK_LOD_CLAMP_NONE;
				VkSampler Sampler = VK_NULL_HANDLE;
				vkCreateSampler(Logical, &Info, nullptr, &Sampler);
				return Sampler;
			}

			bool CreateLayouts()
			{
				Samplers[0] = MakeSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false);
				Samplers[1] = MakeSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, false, SamplerAnisotropy);
				Samplers[2] = MakeSampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false);
				Samplers[3] = MakeSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, true);

				VkDescriptorSetLayoutBinding ConstantBindings[6] = {};
				for (std::uint32_t Index = 0; Index < 6; ++Index)
				{
					ConstantBindings[Index].binding = Index;
					ConstantBindings[Index].descriptorCount = 1;
					ConstantBindings[Index].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
					if (Index < 2)
					{
						ConstantBindings[Index].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
					}
					else
					{
						ConstantBindings[Index].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
						ConstantBindings[Index].pImmutableSamplers = &Samplers[Index - 2];
					}
				}

				VkDescriptorSetLayoutCreateInfo LayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
				LayoutInfo.bindingCount = 6;
				LayoutInfo.pBindings = ConstantBindings;
				if (vkCreateDescriptorSetLayout(Logical, &LayoutInfo, nullptr, &ConstantLayout) != VK_SUCCESS)
				{
					return false;
				}

				VkDescriptorSetLayoutBinding TableBindings[MaxTextureBindings] = {};
				for (std::uint32_t Index = 0; Index < MaxTextureBindings; ++Index)
				{
					TableBindings[Index].binding = Index;
					TableBindings[Index].descriptorCount = 1;
					TableBindings[Index].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
					TableBindings[Index].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
				}
				LayoutInfo.bindingCount = MaxTextureBindings;
				LayoutInfo.pBindings = TableBindings;
				if (vkCreateDescriptorSetLayout(Logical, &LayoutInfo, nullptr, &TableLayout) != VK_SUCCESS)
				{
					return false;
				}

				if (bRayTracing)
				{
					VkDescriptorSetLayoutBinding SceneBindings[2] = {};
					SceneBindings[0].binding = 0;
					SceneBindings[0].descriptorCount = 1;
					SceneBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
					SceneBindings[0].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
					SceneBindings[1].binding = 1;
					SceneBindings[1].descriptorCount = MaxRayBuffers;
					SceneBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
					SceneBindings[1].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
					LayoutInfo.bindingCount = 2;
					LayoutInfo.pBindings = SceneBindings;
					if (vkCreateDescriptorSetLayout(Logical, &LayoutInfo, nullptr, &SceneLayout) != VK_SUCCESS)
					{
						return false;
					}
				}

				const VkDescriptorSetLayout SetLayouts[] = { ConstantLayout, TableLayout, SceneLayout };
				const VkPushConstantRange Push = { VK_SHADER_STAGE_ALL_GRAPHICS, 0, MaxPushConstantBytes };
				VkPipelineLayoutCreateInfo PipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
				PipelineLayoutInfo.setLayoutCount = bRayTracing ? 3 : 2;
				PipelineLayoutInfo.pSetLayouts = SetLayouts;
				PipelineLayoutInfo.pushConstantRangeCount = 1;
				PipelineLayoutInfo.pPushConstantRanges = &Push;
				return vkCreatePipelineLayout(Logical, &PipelineLayoutInfo, nullptr, &PipelineLayout) == VK_SUCCESS;
			}

			std::uint32_t FindMemoryType(std::uint32_t TypeBits, VkMemoryPropertyFlags Flags) const
			{
				for (std::uint32_t Index = 0; Index < MemoryProperties.memoryTypeCount; ++Index)
				{
					if ((TypeBits & (1u << Index)) != 0 && (MemoryProperties.memoryTypes[Index].propertyFlags & Flags) == Flags)
					{
						return Index;
					}
				}
				return UINT32_MAX;
			}

			VkDeviceMemory AllocateMemory(const VkMemoryRequirements& Requirements, VkMemoryPropertyFlags Flags, bool bAddressed = false)
			{
				VkMemoryAllocateFlagsInfo Addressed = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
				Addressed.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

				VkMemoryAllocateInfo Info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
				Info.pNext = bAddressed ? &Addressed : nullptr;
				Info.allocationSize = Requirements.size;
				Info.memoryTypeIndex = FindMemoryType(Requirements.memoryTypeBits, Flags);
				VkDeviceMemory Memory = VK_NULL_HANDLE;
				if (vkAllocateMemory(Logical, &Info, nullptr, &Memory) != VK_SUCCESS)
				{
					Log("Vulkan: failed to allocate %llu bytes", static_cast<unsigned long long>(Requirements.size));
				}
				return Memory;
			}

			BufferSlot MakeBuffer(VkDeviceSize Size, VkBufferUsageFlags Usage, VkMemoryPropertyFlags Flags)
			{
				BufferSlot Slot;
				VkBufferCreateInfo Info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
				Info.size = Size;
				Info.usage = Usage;
				if (vkCreateBuffer(Logical, &Info, nullptr, &Slot.Buffer) != VK_SUCCESS)
				{
					return {};
				}
				const bool bAddressed = (Usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
				VkMemoryRequirements Requirements;
				vkGetBufferMemoryRequirements(Logical, Slot.Buffer, &Requirements);
				Slot.Memory = AllocateMemory(Requirements, Flags, bAddressed);
				vkBindBufferMemory(Logical, Slot.Buffer, Slot.Memory, 0);
				if (bAddressed)
				{
					VkBufferDeviceAddressInfo AddressInfo = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
					AddressInfo.buffer = Slot.Buffer;
					Slot.Address = vkGetBufferDeviceAddress(Logical, &AddressInfo);
				}
				return Slot;
			}

			/** Room for a build, with enough extra to start at the alignment the hardware asks for. */
			BufferSlot MakeScratch(VkDeviceSize Size)
			{
				return MakeBuffer(Size + ScratchAlignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			}

			AccelerationSlot MakeAcceleration(VkAccelerationStructureTypeKHR Type, VkDeviceSize Size)
			{
				AccelerationSlot Slot;
				Slot.Size = Size;
				Slot.Storage = MakeBuffer(Size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

				VkAccelerationStructureCreateInfoKHR Info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
				Info.buffer = Slot.Storage.Buffer;
				Info.size = Size;
				Info.type = Type;
				vkCreateAccelerationStructureKHR(Logical, &Info, nullptr, &Slot.Handle);

				VkAccelerationStructureDeviceAddressInfoKHR AddressInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
				AddressInfo.accelerationStructure = Slot.Handle;
				Slot.Address = vkGetAccelerationStructureDeviceAddressKHR(Logical, &AddressInfo);
				return Slot;
			}

			void FreeBuffer(BufferSlot& Slot)
			{
				if (Slot.Buffer != VK_NULL_HANDLE)
				{
					vkDestroyBuffer(Logical, Slot.Buffer, nullptr);
					vkFreeMemory(Logical, Slot.Memory, nullptr);
					Slot = {};
				}
			}

			void FreeAcceleration(AccelerationSlot& Slot)
			{
				if (Slot.Handle != VK_NULL_HANDLE)
				{
					vkDestroyAccelerationStructureKHR(Logical, Slot.Handle, nullptr);
					FreeBuffer(Slot.Storage);
					Slot = {};
				}
			}

			void FreeTexture(TextureSlot& Slot)
			{
				if (Slot.View != VK_NULL_HANDLE)
				{
					vkDestroyImageView(Logical, Slot.View, nullptr);
				}
				if (Slot.Memory != VK_NULL_HANDLE)
				{
					vkDestroyImage(Logical, Slot.Image, nullptr);
					vkFreeMemory(Logical, Slot.Memory, nullptr);
				}
				Slot = {};
			}

			void FreeGarbage(FrameState& Frame)
			{
				for (BufferSlot& Slot : Frame.GarbageBuffers)
				{
					FreeBuffer(Slot);
				}
				for (TextureSlot& Slot : Frame.GarbageTextures)
				{
					FreeTexture(Slot);
				}
				for (AccelerationSlot& Slot : Frame.GarbageAccelerations)
				{
					FreeAcceleration(Slot);
				}
				Frame.GarbageBuffers.clear();
				Frame.GarbageTextures.clear();
				Frame.GarbageAccelerations.clear();
			}

			/** Copies pixels in on the upload commands, which leave the texture ready to sample. */
			void RecordPixels(TextureSlot& Slot, const void* Pixels)
			{
				const TextureDesc& Desc = Slot.Desc;

				std::vector<VkBufferImageCopy> Regions;
				VkDeviceSize Bytes = 0;
				for (std::uint32_t Layer = 0; Layer < Desc.Layers; ++Layer)
				{
					for (std::uint32_t Mip = 0; Mip < Desc.MipLevels; ++Mip)
					{
						VkBufferImageCopy Region = {};
						Region.bufferOffset = Bytes;
						Region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, Mip, Layer, 1 };
						Region.imageExtent = { std::max(Desc.Width >> Mip, 1u), std::max(Desc.Height >> Mip, 1u), Desc.Depth };
						Regions.push_back(Region);
						Bytes += static_cast<VkDeviceSize>(Region.imageExtent.width) * Region.imageExtent.height * Desc.Depth * BytesPerPixel(Desc.Format);
					}
				}

				const UploadSpan Upload = AllocateUpload(Bytes, 16);
				std::memcpy(Upload.Data, Pixels, static_cast<std::size_t>(Bytes));
				for (VkBufferImageCopy& Region : Regions)
				{
					Region.bufferOffset += Upload.Offset;
				}

				const VkCommandBuffer UploadCommands = CurrentFrame().Uploads;
				Transition(UploadCommands, Slot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				vkCmdCopyBufferToImage(UploadCommands, Upload.Buffer, Slot.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(Regions.size()), Regions.data());
				Transition(UploadCommands, Slot, ReadLayout(Desc.Format));
			}

			VkImageView MakeView(VkImage Image, VkFormat Format, bool bDepth, std::uint32_t Mips = 1, std::uint32_t Layers = 1, bool bVolume = false)
			{
				VkImageViewCreateInfo Info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				Info.image = Image;
				Info.viewType = bVolume ? VK_IMAGE_VIEW_TYPE_3D : (Layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
				Info.format = Format;
				Info.subresourceRange = { static_cast<VkImageAspectFlags>(bDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, Mips, 0, Layers };
				VkImageView View = VK_NULL_HANDLE;
				vkCreateImageView(Logical, &Info, nullptr, &View);
				return View;
			}

			bool CreateFrames()
			{
				VkDescriptorPoolSize ConstantSizes[] = {
					{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * FrameCount },
					{ VK_DESCRIPTOR_TYPE_SAMPLER, 4 * FrameCount } };
				VkDescriptorPoolCreateInfo PoolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
				PoolInfo.maxSets = FrameCount;
				PoolInfo.poolSizeCount = 2;
				PoolInfo.pPoolSizes = ConstantSizes;
				if (vkCreateDescriptorPool(Logical, &PoolInfo, nullptr, &ConstantPool) != VK_SUCCESS)
				{
					return false;
				}

				if (bRayTracing)
				{
					const VkDescriptorPoolSize SceneSizes[] = {
						{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, FrameCount },
						{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FrameCount * MaxRayBuffers } };
					PoolInfo.pPoolSizes = SceneSizes;
					if (vkCreateDescriptorPool(Logical, &PoolInfo, nullptr, &ScenePool) != VK_SUCCESS)
					{
						return false;
					}
					EmptyBuffer = MakeBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
				}

				for (FrameState& Frame : Frames)
				{
					VkCommandPoolCreateInfo CommandPoolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
					CommandPoolInfo.queueFamilyIndex = QueueFamily;
					if (vkCreateCommandPool(Logical, &CommandPoolInfo, nullptr, &Frame.Pool) != VK_SUCCESS)
					{
						return false;
					}

					VkCommandBuffer Allocated[2];
					VkCommandBufferAllocateInfo CommandInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
					CommandInfo.commandPool = Frame.Pool;
					CommandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
					CommandInfo.commandBufferCount = 2;
					if (vkAllocateCommandBuffers(Logical, &CommandInfo, Allocated) != VK_SUCCESS)
					{
						return false;
					}
					Frame.Commands = Allocated[0];
					Frame.Uploads = Allocated[1];

					// The first wait for a frame must not block, so its fence starts signalled.
					VkFenceCreateInfo FenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
					FenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
					VkSemaphoreCreateInfo SemaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
					if (vkCreateFence(Logical, &FenceInfo, nullptr, &Frame.Fence) != VK_SUCCESS
						|| vkCreateSemaphore(Logical, &SemaphoreInfo, nullptr, &Frame.ImageAcquired) != VK_SUCCESS)
					{
						return false;
					}

					Frame.Ring = MakeBuffer(RingSize, GetUploadUsage() | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
						VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
					if (Frame.Ring.Buffer == VK_NULL_HANDLE)
					{
						return false;
					}
					vkMapMemory(Logical, Frame.Ring.Memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&Frame.RingData));
					Frame.ConstantOffset = RingSize - ConstantRegion;

					VkDescriptorSetAllocateInfo SetInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
					SetInfo.descriptorPool = ConstantPool;
					SetInfo.descriptorSetCount = 1;
					SetInfo.pSetLayouts = &ConstantLayout;
					if (vkAllocateDescriptorSets(Logical, &SetInfo, &Frame.ConstantSet) != VK_SUCCESS)
					{
						return false;
					}

					const VkDescriptorBufferInfo BufferInfo = { Frame.Ring.Buffer, 0, MaxConstantBytes };
					VkWriteDescriptorSet Writes[2] = {};
					for (std::uint32_t Index = 0; Index < 2; ++Index)
					{
						Writes[Index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
						Writes[Index].dstSet = Frame.ConstantSet;
						Writes[Index].dstBinding = Index;
						Writes[Index].descriptorCount = 1;
						Writes[Index].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
						Writes[Index].pBufferInfo = &BufferInfo;
					}
					vkUpdateDescriptorSets(Logical, 2, Writes, 0, nullptr);

					if (bRayTracing)
					{
						SetInfo.descriptorPool = ScenePool;
						SetInfo.pSetLayouts = &SceneLayout;
						if (vkAllocateDescriptorSets(Logical, &SetInfo, &Frame.SceneSet) != VK_SUCCESS)
						{
							return false;
						}
					}

					const VkDescriptorPoolSize TableSize = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, TableSetsPerFrame * MaxTextureBindings };
					PoolInfo.maxSets = TableSetsPerFrame;
					PoolInfo.poolSizeCount = 1;
					PoolInfo.pPoolSizes = &TableSize;
					if (vkCreateDescriptorPool(Logical, &PoolInfo, nullptr, &Frame.TablePool) != VK_SUCCESS)
					{
						return false;
					}
				}

				// The upload buffer is always recording, ready for resources made between frames.
				vkResetFences(Logical, 1, &CurrentFrame().Fence);
				BeginUploads(CurrentFrame());
				return true;
			}

			void BeginUploads(FrameState& Frame)
			{
				VkCommandBufferBeginInfo Begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
				Begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
				vkBeginCommandBuffer(Frame.Uploads, &Begin);
			}

			bool CreateSwapchain()
			{
				VkSurfaceCapabilitiesKHR Capabilities;
				vkGetPhysicalDeviceSurfaceCapabilitiesKHR(Physical, Surface, &Capabilities);

				if (Capabilities.currentExtent.width != UINT32_MAX)
				{
					SwapWidth = Capabilities.currentExtent.width;
					SwapHeight = Capabilities.currentExtent.height;
				}

				std::uint32_t ModeCount = 0;
				vkGetPhysicalDeviceSurfacePresentModesKHR(Physical, Surface, &ModeCount, nullptr);
				std::vector<VkPresentModeKHR> Modes(ModeCount);
				vkGetPhysicalDeviceSurfacePresentModesKHR(Physical, Surface, &ModeCount, Modes.data());

				// Frames are not synchronised to the display. FIFO is only the fallback,
				// because it is the one mode every driver has to offer.
				VkPresentModeKHR Mode = VK_PRESENT_MODE_FIFO_KHR;
				for (const VkPresentModeKHR Candidate : Modes)
				{
					if (Candidate == VK_PRESENT_MODE_IMMEDIATE_KHR || (Candidate == VK_PRESENT_MODE_MAILBOX_KHR && Mode == VK_PRESENT_MODE_FIFO_KHR))
					{
						Mode = Candidate;
					}
				}

				VkSwapchainCreateInfoKHR Info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
				Info.surface = Surface;
				Info.minImageCount = std::max(3u, Capabilities.minImageCount);
				if (Capabilities.maxImageCount != 0)
				{
					Info.minImageCount = std::min(Info.minImageCount, Capabilities.maxImageCount);
				}
				Info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
				Info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
				Info.imageExtent = { SwapWidth, SwapHeight };
				Info.imageArrayLayers = 1;
				Info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
				Info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
				Info.preTransform = Capabilities.currentTransform;
				Info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
				Info.presentMode = Mode;
				Info.clipped = VK_TRUE;

				const VkResult Result = vkCreateSwapchainKHR(Logical, &Info, nullptr, &Swapchain);
				if (Result != VK_SUCCESS)
				{
					Log("Vulkan: vkCreateSwapchainKHR failed: %d", static_cast<int>(Result));
					return false;
				}

				std::uint32_t ImageCount = 0;
				vkGetSwapchainImagesKHR(Logical, Swapchain, &ImageCount, nullptr);
				std::vector<VkImage> Images(ImageCount);
				vkGetSwapchainImagesKHR(Logical, Swapchain, &ImageCount, Images.data());

				Backbuffers.clear();
				RenderFinished.clear();
				for (const VkImage Image : Images)
				{
					TextureSlot Slot;
					Slot.Image = Image;
					Slot.View = MakeView(Image, VK_FORMAT_B8G8R8A8_UNORM, false);
					Slot.Desc.Width = SwapWidth;
					Slot.Desc.Height = SwapHeight;
					Slot.Desc.Format = EFormat::BGRA8Unorm;
					Slot.Desc.bRenderTarget = true;
					Backbuffers.push_back({ Store(Textures, FreeTextures, Slot) });

					VkSemaphoreCreateInfo SemaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
					VkSemaphore Semaphore = VK_NULL_HANDLE;
					vkCreateSemaphore(Logical, &SemaphoreInfo, nullptr, &Semaphore);
					RenderFinished.push_back(Semaphore);
				}
				return true;
			}

			void DestroySwapchain()
			{
				for (const TextureHandle Handle : Backbuffers)
				{
					FreeTexture(Textures[Handle.Id - 1]);
					FreeTextures.push_back(Handle.Id - 1);
				}
				for (const VkSemaphore Semaphore : RenderFinished)
				{
					vkDestroySemaphore(Logical, Semaphore, nullptr);
				}
				Backbuffers.clear();
				RenderFinished.clear();
				vkDestroySwapchainKHR(Logical, Swapchain, nullptr);
				Swapchain = VK_NULL_HANDLE;
			}

			UploadSpan AllocateUpload(VkDeviceSize Size, VkDeviceSize Alignment)
			{
				FrameState& Frame = CurrentFrame();
				const VkDeviceSize Offset = AlignUp(Frame.RingOffset, Alignment);
				if (Offset + Size <= RingSize - ConstantRegion)
				{
					Frame.RingOffset = Offset + Size;
					return { Frame.Ring.Buffer, Offset, Frame.RingData + Offset, Frame.Ring.Address + Offset };
				}

				// Bursts larger than the ring get a buffer of their own for this frame.
				const BufferSlot Extra = MakeBuffer(Size, GetUploadUsage(), VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
				std::uint8_t* Data = nullptr;
				vkMapMemory(Logical, Extra.Memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&Data));
				Frame.GarbageBuffers.push_back(Extra);
				return { Extra.Buffer, 0, Data, Extra.Address };
			}

			VkBufferUsageFlags GetUploadUsage() const
			{
				VkBufferUsageFlags Usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
				if (bRayTracing)
				{
					Usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
				}
				return Usage;
			}

			void BindConstantSet()
			{
				vkCmdBindDescriptorSets(Commands, VK_PIPELINE_BIND_POINT_GRAPHICS, PipelineLayout, 0, 1, &CurrentFrame().ConstantSet, 2, ConstantOffsets);
			}

			/** Everything recorded so far finishes before anything after it starts, this buffer and later ones alike. */
			void RecordFullBarrier(VkCommandBuffer Target)
			{
				VkMemoryBarrier2 Barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
				Barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				Barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
				Barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				Barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
				VkDependencyInfo Dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				Dependency.memoryBarrierCount = 1;
				Dependency.pMemoryBarriers = &Barrier;
				vkCmdPipelineBarrier2(Target, &Dependency);
			}

			// Barriers are rare here, so one conservative form covers every transition.
			void Transition(VkCommandBuffer Target, TextureSlot& Slot, VkImageLayout Layout)
			{
				if (Slot.Layout == Layout)
				{
					return;
				}

				const bool bDepth = Slot.Desc.Format == EFormat::D32Float;
				VkImageMemoryBarrier2 Barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
				Barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				Barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
				Barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				Barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
				Barrier.oldLayout = Slot.Layout;
				Barrier.newLayout = Layout;
				Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				Barrier.image = Slot.Image;
				Barrier.subresourceRange = { static_cast<VkImageAspectFlags>(bDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };

				VkDependencyInfo Dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				Dependency.imageMemoryBarrierCount = 1;
				Dependency.pImageMemoryBarriers = &Barrier;
				vkCmdPipelineBarrier2(Target, &Dependency);
				Slot.Layout = Layout;
			}

			void RecordCapture(TextureSlot& Back)
			{
				const VkDeviceSize Bytes = static_cast<VkDeviceSize>(SwapWidth) * SwapHeight * 4;
				CaptureBuffer = MakeBuffer(Bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

				Transition(Commands, Back, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

				VkBufferImageCopy Region = {};
				Region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				Region.imageExtent = { SwapWidth, SwapHeight, 1 };
				vkCmdCopyImageToBuffer(Commands, Back.Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, CaptureBuffer.Buffer, 1, &Region);
			}

			void ResolveCapture()
			{
				const std::size_t Bytes = static_cast<std::size_t>(SwapWidth) * SwapHeight * 4;
				std::uint8_t* Mapped = nullptr;
				vkMapMemory(Logical, CaptureBuffer.Memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&Mapped));

				CapturePixels.resize(Bytes);
				for (std::size_t Offset = 0; Offset < Bytes; Offset += 4)
				{
					CapturePixels[Offset + 0] = Mapped[Offset + 2];
					CapturePixels[Offset + 1] = Mapped[Offset + 1];
					CapturePixels[Offset + 2] = Mapped[Offset + 0];
					CapturePixels[Offset + 3] = 255;
				}
				FreeBuffer(CaptureBuffer);
				bCaptureRequested = false;
			}

			std::unique_ptr<VendorFeatures> Vendor;
			VkInstance Instance = VK_NULL_HANDLE;
			VkDebugUtilsMessengerEXT DebugMessenger = VK_NULL_HANDLE;
			PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessenger = nullptr;
			VkSurfaceKHR Surface = VK_NULL_HANDLE;
			VkPhysicalDevice Physical = VK_NULL_HANDLE;
			VkDevice Logical = VK_NULL_HANDLE;
			VkQueue Queue = VK_NULL_HANDLE;
			std::uint32_t QueueFamily = 0;
			VkPhysicalDeviceMemoryProperties MemoryProperties = {};
			std::string AdapterName;

			VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
			std::uint32_t SwapWidth = 0;
			std::uint32_t SwapHeight = 0;
			std::uint32_t ImageIndex = 0;
			std::vector<TextureHandle> Backbuffers;
			std::vector<VkSemaphore> RenderFinished;

			VkSampler Samplers[4] = {};
			float SamplerAnisotropy = 1.0f;
			VkDescriptorSetLayout ConstantLayout = VK_NULL_HANDLE;
			VkDescriptorSetLayout TableLayout = VK_NULL_HANDLE;
			VkDescriptorSetLayout SceneLayout = VK_NULL_HANDLE;
			VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
			VkDescriptorPool ConstantPool = VK_NULL_HANDLE;
			VkDescriptorPool ScenePool = VK_NULL_HANDLE;

			bool bRayTracing = false;
			VkDeviceSize ScratchAlignment = 1;
			BufferSlot EmptyBuffer; // stands in for the scene's vertex buffers that are not in use

			FrameState Frames[FrameCount];
			std::uint32_t FrameIndex = 0;
			VkCommandBuffer Commands = VK_NULL_HANDLE;
			std::uint32_t ConstantOffsets[2] = {};

			std::vector<BufferSlot> Buffers;
			std::vector<TextureSlot> Textures;
			std::vector<PipelineSlot> Pipelines;
			std::vector<AccelerationSlot> Accelerations;
			std::vector<std::uint32_t> FreeAccelerations;
			std::vector<std::uint32_t> FreeBuffers;
			std::vector<std::uint32_t> FreeTextures;
			std::vector<std::uint32_t> FreePipelines;
			TextureHandle WhiteTexture;

			bool bCaptureRequested = false;
			BufferSlot CaptureBuffer;
			std::vector<std::uint8_t> CapturePixels;
		};
	}

	std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& Desc)
	{
		auto Result = std::make_unique<VulkanDevice>();
		if (!Result->Init(Desc))
		{
			return nullptr;
		}
		return Result;
	}
}
