#include "Rhi/Rhi.h"

#include "Core/Log.h"
#include "Rhi/Streamline.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

namespace Teinora::Rhi
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		constexpr UINT FrameCount = 2;
		constexpr UINT BackbufferCount = 3;
		constexpr UINT64 RingSize = 64ull << 20;
		constexpr UINT64 ConstantRegion = 8ull << 20; // top of the ring, so constants survive upload bursts
		constexpr UINT DescriptorsPerFrame = 8192;
		constexpr UINT CpuDescriptors = 1024;
		constexpr UINT TargetDescriptors = 256;

		constexpr UINT RootPush = 0;
		constexpr UINT RootConstants = 1; // and 2
		constexpr UINT RootTextures = 3;
		constexpr UINT RootScene = 4;
		constexpr UINT RootSceneVertices = 5;

		constexpr D3D12_RESOURCE_STATES DepthReadState = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		constexpr D3D12_RESOURCE_STATES ColorReadState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		constexpr D3D12_RESOURCE_STATES BufferReadState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER
			| D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

		DXGI_FORMAT ToDxgi(EFormat Format)
		{
			switch (Format)
			{
			case EFormat::RGBA8Unorm:  return DXGI_FORMAT_R8G8B8A8_UNORM;
			case EFormat::BGRA8Unorm:  return DXGI_FORMAT_B8G8R8A8_UNORM;
			case EFormat::RGBA8Snorm:  return DXGI_FORMAT_R8G8B8A8_SNORM;
			case EFormat::RGBA16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
			case EFormat::RG16Float:   return DXGI_FORMAT_R16G16_FLOAT;
			case EFormat::RG32Float:   return DXGI_FORMAT_R32G32_FLOAT;
			case EFormat::RGB32Float:  return DXGI_FORMAT_R32G32B32_FLOAT;
			case EFormat::RGBA32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
			case EFormat::D32Float:    return DXGI_FORMAT_D32_FLOAT;
			default:                   return DXGI_FORMAT_UNKNOWN;
			}
		}

		UINT BytesPerPixel(EFormat Format)
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

		D3D12_COMPARISON_FUNC ToD3D(ECompare Compare)
		{
			switch (Compare)
			{
			case ECompare::Greater:      return D3D12_COMPARISON_FUNC_GREATER;
			case ECompare::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
			case ECompare::LessEqual:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
			default:                     return D3D12_COMPARISON_FUNC_ALWAYS;
			}
		}

		UINT64 AlignUp(UINT64 Value, UINT64 Alignment)
		{
			return (Value + Alignment - 1) & ~(Alignment - 1);
		}

		std::string ToUtf8(const wchar_t* Text)
		{
			const int Size = WideCharToMultiByte(CP_UTF8, 0, Text, -1, nullptr, 0, nullptr, nullptr);
			std::string Result(static_cast<std::size_t>(Size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, Text, -1, Result.data(), Size, nullptr, nullptr);
			Result.resize(static_cast<std::size_t>(Size - 1));
			return Result;
		}

		/** Hands out CPU descriptors from a fixed heap. */
		class DescriptorPool
		{
		public:
			bool Init(ID3D12Device* Device, D3D12_DESCRIPTOR_HEAP_TYPE Type, UINT Count)
			{
				D3D12_DESCRIPTOR_HEAP_DESC Desc = {};
				Desc.Type = Type;
				Desc.NumDescriptors = Count;
				if (FAILED(Device->CreateDescriptorHeap(&Desc, IID_PPV_ARGS(&Heap))))
				{
					return false;
				}
				Start = Heap->GetCPUDescriptorHandleForHeapStart();
				Increment = Device->GetDescriptorHandleIncrementSize(Type);
				for (UINT Index = Count; Index > 0; --Index)
				{
					FreeSlots.push_back(Index - 1);
				}
				return true;
			}

			D3D12_CPU_DESCRIPTOR_HANDLE Allocate()
			{
				const UINT Slot = FreeSlots.back();
				FreeSlots.pop_back();
				return { Start.ptr + static_cast<SIZE_T>(Slot) * Increment };
			}

			void Free(D3D12_CPU_DESCRIPTOR_HANDLE Handle)
			{
				FreeSlots.push_back(static_cast<UINT>((Handle.ptr - Start.ptr) / Increment));
			}

		private:
			ComPtr<ID3D12DescriptorHeap> Heap;
			D3D12_CPU_DESCRIPTOR_HANDLE Start = {};
			UINT Increment = 0;
			std::vector<UINT> FreeSlots;
		};

		struct BufferSlot
		{
			ComPtr<ID3D12Resource> Resource;
			UINT64 Size = 0;
		};

		struct TextureSlot
		{
			ComPtr<ID3D12Resource> Resource;
			TextureDesc Desc;
			D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
			D3D12_CPU_DESCRIPTOR_HANDLE Srv = {};
			D3D12_CPU_DESCRIPTOR_HANDLE Rtv = {};
			D3D12_CPU_DESCRIPTOR_HANDLE Dsv = {};
			D3D12_CPU_DESCRIPTOR_HANDLE DsvReadOnly = {};
		};

		struct PipelineSlot
		{
			ComPtr<ID3D12PipelineState> State;
			UINT Strides[4] = {};
		};

		struct UploadSpan
		{
			ID3D12Resource* Resource = nullptr;
			UINT64 Offset = 0;
			D3D12_GPU_VIRTUAL_ADDRESS Address = 0;
			std::uint8_t* Data = nullptr;
		};

		struct FrameState
		{
			ComPtr<ID3D12CommandAllocator> Allocator;
			ComPtr<ID3D12GraphicsCommandList> List;
			ComPtr<ID3D12CommandAllocator> UploadAllocator;
			ComPtr<ID3D12GraphicsCommandList> UploadList;
			ComPtr<ID3D12Resource> Ring;
			std::uint8_t* RingData = nullptr;
			UINT64 RingOffset = 0;
			UINT64 ConstantOffset = 0;
			UINT DescriptorOffset = 0;
			UINT64 FenceValue = 0;
			std::vector<ComPtr<ID3D12Resource>> Garbage;

			ComPtr<ID3D12GraphicsCommandList4> RayList;
			ComPtr<ID3D12GraphicsCommandList4> RayUploadList;
			ComPtr<ID3D12Resource> Scene;
			ComPtr<ID3D12Resource> SceneScratch;
			UINT64 SceneSize = 0;
			UINT64 SceneScratchSize = 0;
		};

		D3D12_RESOURCE_DESC MakeBufferDesc(UINT64 Size)
		{
			D3D12_RESOURCE_DESC Desc = {};
			Desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			Desc.Width = Size;
			Desc.Height = 1;
			Desc.DepthOrArraySize = 1;
			Desc.MipLevels = 1;
			Desc.SampleDesc.Count = 1;
			Desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			return Desc;
		}

		class D3D12Device final : public Device
		{
		public:
			~D3D12Device() override
			{
				if (Queue != nullptr)
				{
					WaitIdle();
				}
				Vendor.reset(); // Streamline must shut down before the device goes
				if (FenceEvent != nullptr)
				{
					CloseHandle(FenceEvent);
				}
			}

			bool Init(const DeviceDesc& Desc)
			{
				Window = static_cast<HWND>(Desc.NativeWindow);

				UINT FactoryFlags = 0;
				if (Desc.bValidation)
				{
					ComPtr<ID3D12Debug> Debug;
					if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&Debug))))
					{
						Debug->EnableDebugLayer();
						FactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
					}
					else
					{
						Log("D3D12 debug layer is not installed");
					}

					// What the GPU was doing when a device is lost, which needs no debug layer.
					ComPtr<ID3D12DeviceRemovedExtendedDataSettings> Breadcrumbs;
					if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&Breadcrumbs))))
					{
						Breadcrumbs->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
						Breadcrumbs->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
					}
				}

				// With Streamline running, the factory and device come from its interposer.
				if (Desc.bVendorFeatures)
				{
					StartStreamline(EBackend::D3D12);
				}
				using CreateFactoryFunction = HRESULT(WINAPI*)(UINT, REFIID, void**);
				void* CreateFactory = GetStreamlineExport("CreateDXGIFactory2");
				const CreateFactoryFunction MakeFactory = CreateFactory != nullptr ? reinterpret_cast<CreateFactoryFunction>(CreateFactory) : &CreateDXGIFactory2;

				if (FAILED(MakeFactory(FactoryFlags, IID_PPV_ARGS(&Factory))) || !CreateLogicalDevice())
				{
					return false;
				}
				SetStreamlineDevice(D3D.Get());
				DetectRayTracing();

				D3D12_COMMAND_QUEUE_DESC QueueDesc = {};
				QueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
				if (FAILED(D3D->CreateCommandQueue(&QueueDesc, IID_PPV_ARGS(&Queue)))
					|| FAILED(D3D->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence))))
				{
					return false;
				}
				FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

				if (!CreateDescriptorHeaps() || !CreateRootSignature() || !CreateFrames() || !CreateSwapchain(Desc.Width, Desc.Height))
				{
					return false;
				}

				TextureDesc WhiteDesc;
				const std::uint32_t WhitePixel = 0xFFFFFFFFu;
				WhiteTexture = CreateTexture(WhiteDesc, &WhitePixel);

				if (GetStreamlineExport("slInit") != nullptr)
				{
					StreamlineAdapter Adapter;
					Adapter.Luid = &AdapterLuid;
					Adapter.LuidSize = sizeof(AdapterLuid);
					Vendor = CreateStreamlineFeatures(*this, Adapter);
				}
				return true;
			}

			EBackend GetBackend() const override { return EBackend::D3D12; }
			const std::string& GetAdapterName() const override { return AdapterName; }
			VendorFeatures* GetVendorFeatures() override { return Vendor.get(); }

			NativeTexture AccessForExternal(TextureHandle Handle, bool bWrite) override
			{
				TextureSlot& Slot = Textures[Handle.Id - 1];
				const bool bDepth = Slot.Desc.Format == EFormat::D32Float;
				Transition(Slot, bWrite ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : (bDepth ? DepthReadState : ColorReadState));
				FlushBarriers();

				NativeTexture Result;
				Result.Resource = Slot.Resource.Get();
				Result.State = static_cast<std::uint32_t>(Slot.State);
				Result.Width = Slot.Desc.Width;
				Result.Height = Slot.Desc.Height;
				return Result;
			}

			void* GetNativeCommandList() override { return List; }

			void RestoreAfterExternal() override
			{
				ID3D12DescriptorHeap* Heaps[] = { GpuHeap.Get() };
				List->SetDescriptorHeaps(1, Heaps);
				List->SetGraphicsRootSignature(RootSignature.Get());
				List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			}

			BufferHandle CreateBuffer(std::size_t Size, const void* Data) override
			{
				BufferSlot Slot;
				Slot.Size = Size;

				const D3D12_RESOURCE_DESC Desc = MakeBufferDesc(Size);
				const D3D12_HEAP_PROPERTIES Heap = { D3D12_HEAP_TYPE_DEFAULT };
				const HRESULT Created = D3D->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Slot.Resource));
				if (FAILED(Created))
				{
					Log("D3D12: failed to create a buffer of %zu bytes: 0x%08X, device reason 0x%08X", Size, static_cast<unsigned>(Created), static_cast<unsigned>(D3D->GetDeviceRemovedReason()));
					ReportLostDevice();
					return {};
				}

				const UploadSpan Upload = AllocateUpload(Size, 16);
				std::memcpy(Upload.Data, Data, Size);
				ID3D12GraphicsCommandList* Uploads = CurrentFrame().UploadList.Get();
				Uploads->CopyBufferRegion(Slot.Resource.Get(), 0, Upload.Resource, Upload.Offset, Size);

				D3D12_RESOURCE_BARRIER Barrier = {};
				Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				Barrier.Transition.pResource = Slot.Resource.Get();
				Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
				Barrier.Transition.StateAfter = BufferReadState;
				Uploads->ResourceBarrier(1, &Barrier);

				return { Store(Buffers, FreeBuffers, std::move(Slot)) };
			}

			void DestroyBuffer(BufferHandle Handle) override
			{
				BufferSlot& Slot = Buffers[Handle.Id - 1];
				CurrentFrame().Garbage.push_back(std::move(Slot.Resource));
				Slot = {};
				FreeBuffers.push_back(Handle.Id - 1);
			}

			TextureHandle CreateTexture(const TextureDesc& Desc, const void* Pixels) override
			{
				const bool bDepth = Desc.Format == EFormat::D32Float;

				TextureSlot Slot;
				Slot.Desc = Desc;

				const bool bVolume = Desc.Depth > 1;
				D3D12_RESOURCE_DESC ResourceDesc = {};
				ResourceDesc.Dimension = bVolume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				ResourceDesc.Width = Desc.Width;
				ResourceDesc.Height = Desc.Height;
				ResourceDesc.DepthOrArraySize = static_cast<UINT16>(bVolume ? Desc.Depth : Desc.Layers);
				ResourceDesc.MipLevels = static_cast<UINT16>(Desc.MipLevels);
				ResourceDesc.Format = bDepth ? DXGI_FORMAT_R32_TYPELESS : ToDxgi(Desc.Format);
				ResourceDesc.SampleDesc.Count = 1;
				if (Desc.bRenderTarget)
				{
					ResourceDesc.Flags = bDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
				}
				if (Desc.bUnorderedAccess)
				{
					ResourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				}

				D3D12_CLEAR_VALUE Clear = {};
				Clear.Format = ToDxgi(Desc.Format);
				if (!bDepth)
				{
					Clear.Color[3] = 1.0f;
				}

				Slot.State = Pixels != nullptr ? D3D12_RESOURCE_STATE_COPY_DEST : (bDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
				const D3D12_HEAP_PROPERTIES Heap = { D3D12_HEAP_TYPE_DEFAULT };
				if (!Desc.bRenderTarget && Pixels == nullptr)
				{
					Slot.State = D3D12_RESOURCE_STATE_COMMON;
				}
				if (FAILED(D3D->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &ResourceDesc, Slot.State,
					Desc.bRenderTarget ? &Clear : nullptr, IID_PPV_ARGS(&Slot.Resource))))
				{
					Log("D3D12: failed to create a %ux%u texture", Desc.Width, Desc.Height);
					return {};
				}

				D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
				SrvDesc.Format = bDepth ? DXGI_FORMAT_R32_FLOAT : ToDxgi(Desc.Format);
				SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				if (bVolume)
				{
					SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
					SrvDesc.Texture3D.MipLevels = Desc.MipLevels;
				}
				else if (Desc.Layers > 1)
				{
					SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
					SrvDesc.Texture2DArray.MipLevels = Desc.MipLevels;
					SrvDesc.Texture2DArray.ArraySize = Desc.Layers;
				}
				else
				{
					SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
					SrvDesc.Texture2D.MipLevels = Desc.MipLevels;
				}
				Slot.Srv = CpuHeap.Allocate();
				D3D->CreateShaderResourceView(Slot.Resource.Get(), &SrvDesc, Slot.Srv);

				if (Desc.bRenderTarget && bDepth)
				{
					D3D12_DEPTH_STENCIL_VIEW_DESC DsvDesc = {};
					DsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
					DsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
					Slot.Dsv = DsvHeap.Allocate();
					D3D->CreateDepthStencilView(Slot.Resource.Get(), &DsvDesc, Slot.Dsv);

					DsvDesc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
					Slot.DsvReadOnly = DsvHeap.Allocate();
					D3D->CreateDepthStencilView(Slot.Resource.Get(), &DsvDesc, Slot.DsvReadOnly);
				}
				else if (Desc.bRenderTarget)
				{
					Slot.Rtv = RtvHeap.Allocate();
					D3D->CreateRenderTargetView(Slot.Resource.Get(), nullptr, Slot.Rtv);
				}

				if (Pixels != nullptr)
				{
					RecordPixels(Slot, Pixels);
					Slot.State = ColorReadState;
				}

				return { Store(Textures, FreeTextures, std::move(Slot)) };
			}

			void UpdateTexture(TextureHandle Handle, const void* Pixels) override
			{
				const TextureSlot& Slot = Textures[Handle.Id - 1];

				D3D12_RESOURCE_BARRIER Barrier = {};
				Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				Barrier.Transition.pResource = Slot.Resource.Get();
				Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				Barrier.Transition.StateBefore = ColorReadState;
				Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
				CurrentFrame().UploadList->ResourceBarrier(1, &Barrier);

				RecordPixels(Slot, Pixels);
			}

			void DestroyTexture(TextureHandle Handle) override
			{
				TextureSlot& Slot = Textures[Handle.Id - 1];
				CurrentFrame().Garbage.push_back(std::move(Slot.Resource));
				ReleaseViews(Slot);
				Slot = {};
				FreeTextures.push_back(Handle.Id - 1);
			}

			PipelineHandle CreatePipeline(const PipelineDesc& Desc) override
			{
				PipelineSlot Slot;
				for (std::size_t Index = 0; Index < Desc.Streams.size(); ++Index)
				{
					Slot.Strides[Index] = Desc.Streams[Index].Stride;
				}

				std::vector<D3D12_INPUT_ELEMENT_DESC> Elements;
				for (std::size_t Index = 0; Index < Desc.Attributes.size(); ++Index)
				{
					const VertexAttribute& Attribute = Desc.Attributes[Index];
					const bool bInstanced = Desc.Streams[Attribute.Stream].bPerInstance;

					D3D12_INPUT_ELEMENT_DESC Element = {};
					Element.SemanticName = "TEXCOORD";
					Element.SemanticIndex = static_cast<UINT>(Index);
					Element.Format = ToDxgi(Attribute.Format);
					Element.InputSlot = Attribute.Stream;
					Element.AlignedByteOffset = Attribute.Offset;
					Element.InputSlotClass = bInstanced ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
					Element.InstanceDataStepRate = bInstanced ? 1 : 0;
					Elements.push_back(Element);
				}

				D3D12_GRAPHICS_PIPELINE_STATE_DESC State = {};
				State.pRootSignature = RootSignature.Get();
				State.VS = { Desc.Vertex.Data, Desc.Vertex.Size };
				State.PS = { Desc.Pixel.Data, Desc.Pixel.Size };
				State.InputLayout = { Elements.data(), static_cast<UINT>(Elements.size()) };
				State.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
				State.SampleMask = UINT_MAX;
				State.SampleDesc.Count = 1;

				D3D12_RASTERIZER_DESC& Raster = State.RasterizerState;
				Raster.FillMode = D3D12_FILL_MODE_SOLID;
				Raster.CullMode = Desc.Cull == ECull::None ? D3D12_CULL_MODE_NONE : (Desc.Cull == ECull::Back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_FRONT);
				Raster.FrontCounterClockwise = TRUE;
				Raster.DepthBias = Desc.DepthBias;
				Raster.SlopeScaledDepthBias = Desc.SlopeDepthBias;
				Raster.DepthClipEnable = TRUE;

				D3D12_RENDER_TARGET_BLEND_DESC& Blend = State.BlendState.RenderTarget[0];
				Blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
				if (Desc.Blend != EBlend::Opaque)
				{
					Blend.BlendEnable = TRUE;
					Blend.SrcBlend = Desc.Blend == EBlend::Alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE;
					Blend.DestBlend = Desc.Blend == EBlend::Alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ONE;
					Blend.BlendOp = D3D12_BLEND_OP_ADD;
					Blend.SrcBlendAlpha = D3D12_BLEND_ONE;
					Blend.DestBlendAlpha = D3D12_BLEND_ZERO;
					Blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
				}

				State.DepthStencilState.DepthEnable = Desc.bDepthTest ? TRUE : FALSE;
				State.DepthStencilState.DepthWriteMask = Desc.bDepthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
				State.DepthStencilState.DepthFunc = ToD3D(Desc.DepthCompare);

				if (Desc.ColorFormat != EFormat::Unknown)
				{
					State.NumRenderTargets = 1;
					State.RTVFormats[0] = ToDxgi(Desc.ColorFormat);
				}
				State.DSVFormat = ToDxgi(Desc.DepthFormat);

				const HRESULT Result = D3D->CreateGraphicsPipelineState(&State, IID_PPV_ARGS(&Slot.State));
				if (FAILED(Result))
				{
					Log("D3D12: CreateGraphicsPipelineState failed: 0x%08X", static_cast<unsigned>(Result));
					return {};
				}
				return { Store(Pipelines, FreePipelines, std::move(Slot)) };
			}

			bool SupportsRayTracing() const override { return bRayTracing; }

			AccelerationHandle CreateAcceleration(BufferHandle Vertices, std::uint32_t Stride, std::uint32_t VertexCount) override
			{
				D3D12_RAYTRACING_GEOMETRY_DESC Geometry = {};
				Geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
				Geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
				Geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
				Geometry.Triangles.VertexCount = VertexCount;
				Geometry.Triangles.VertexBuffer = { Buffers[Vertices.Id - 1].Resource->GetGPUVirtualAddress(), Stride };

				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs = {};
				Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
				Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
				Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
				Inputs.NumDescs = 1;
				Inputs.pGeometryDescs = &Geometry;

				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO Sizes = {};
				RayDevice->GetRaytracingAccelerationStructurePrebuildInfo(&Inputs, &Sizes);

				ComPtr<ID3D12Resource> Result = CreateRayBuffer(Sizes.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
				ComPtr<ID3D12Resource> Scratch = CreateRayBuffer(Sizes.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				if (Result == nullptr || Scratch == nullptr)
				{
					return {};
				}

				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Build = {};
				Build.Inputs = Inputs;
				Build.DestAccelerationStructureData = Result->GetGPUVirtualAddress();
				Build.ScratchAccelerationStructureData = Scratch->GetGPUVirtualAddress();

				FrameState& Frame = CurrentFrame();
				Frame.RayUploadList->BuildRaytracingAccelerationStructure(&Build, 0, nullptr);

				D3D12_RESOURCE_BARRIER Barrier = {};
				Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
				Barrier.UAV.pResource = Result.Get();
				Frame.UploadList->ResourceBarrier(1, &Barrier);

				Frame.Garbage.push_back(std::move(Scratch));
				return { Store(Accelerations, FreeAccelerations, std::move(Result)) };
			}

			void DestroyAcceleration(AccelerationHandle Handle) override
			{
				CurrentFrame().Garbage.push_back(std::move(Accelerations[Handle.Id - 1]));
				FreeAccelerations.push_back(Handle.Id - 1);
			}

			void SetRayScene(const RayInstance* Instances, std::uint32_t Count, const BufferHandle* VertexBuffers, std::uint32_t BufferCount) override
			{
				FrameState& Frame = CurrentFrame();

				const UploadSpan Upload = AllocateUpload(std::max<UINT64>(Count, 1) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
				for (std::uint32_t Index = 0; Index < Count; ++Index)
				{
					const RayInstance& Instance = Instances[Index];
					D3D12_RAYTRACING_INSTANCE_DESC Desc = {};
					std::memcpy(Desc.Transform, Instance.Transform, sizeof(Desc.Transform));
					Desc.InstanceID = Instance.Index;
					Desc.InstanceMask = 0xFF;
					Desc.Flags = Instance.bCutOut ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
					Desc.AccelerationStructure = Accelerations[Instance.Geometry.Id - 1]->GetGPUVirtualAddress();
					std::memcpy(Upload.Data + Index * sizeof(Desc), &Desc, sizeof(Desc));
				}

				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs = {};
				Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
				Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
				Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
				Inputs.NumDescs = Count;
				Inputs.InstanceDescs = Upload.Address;

				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO Sizes = {};
				RayDevice->GetRaytracingAccelerationStructurePrebuildInfo(&Inputs, &Sizes);

				// Each frame slot keeps its own scene, grown with some room to spare.
				if (Frame.SceneSize < Sizes.ResultDataMaxSizeInBytes)
				{
					Frame.Garbage.push_back(std::move(Frame.Scene));
					Frame.SceneSize = Sizes.ResultDataMaxSizeInBytes + Sizes.ResultDataMaxSizeInBytes / 2;
					Frame.Scene = CreateRayBuffer(Frame.SceneSize, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
				}
				if (Frame.SceneScratchSize < Sizes.ScratchDataSizeInBytes)
				{
					Frame.Garbage.push_back(std::move(Frame.SceneScratch));
					Frame.SceneScratchSize = Sizes.ScratchDataSizeInBytes + Sizes.ScratchDataSizeInBytes / 2;
					Frame.SceneScratch = CreateRayBuffer(Frame.SceneScratchSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				}

				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Build = {};
				Build.Inputs = Inputs;
				Build.DestAccelerationStructureData = Frame.Scene->GetGPUVirtualAddress();
				Build.ScratchAccelerationStructureData = Frame.SceneScratch->GetGPUVirtualAddress();
				Frame.RayList->BuildRaytracingAccelerationStructure(&Build, 0, nullptr);

				D3D12_RESOURCE_BARRIER Barrier = {};
				Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
				Barrier.UAV.pResource = Frame.Scene.Get();
				List->ResourceBarrier(1, &Barrier);
				List->SetGraphicsRootShaderResourceView(RootScene, Frame.Scene->GetGPUVirtualAddress());

				const UINT Base = FrameIndex * DescriptorsPerFrame + Frame.DescriptorOffset;
				Frame.DescriptorOffset += MaxRayBuffers;
				for (std::uint32_t Index = 0; Index < BufferCount; ++Index)
				{
					const BufferSlot& Slot = Buffers[VertexBuffers[Index].Id - 1];
					D3D12_SHADER_RESOURCE_VIEW_DESC View = {};
					View.Format = DXGI_FORMAT_R32_TYPELESS;
					View.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
					View.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					View.Buffer.NumElements = static_cast<UINT>(Slot.Size / 4);
					View.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
					D3D->CreateShaderResourceView(Slot.Resource.Get(), &View, { GpuCpuStart.ptr + static_cast<SIZE_T>(Base + Index) * GpuIncrement });
				}
				List->SetGraphicsRootDescriptorTable(RootSceneVertices, { GpuGpuStart.ptr + static_cast<UINT64>(Base) * GpuIncrement });
			}

			void Resize(std::uint32_t Width, std::uint32_t Height) override
			{
				WaitIdle();
				ReleaseBackbuffers();
				if (FAILED(Swapchain->ResizeBuffers(0, Width, Height, DXGI_FORMAT_UNKNOWN, 0)))
				{
					Log("D3D12: ResizeBuffers failed");
				}
				AcquireBackbuffers();
			}

			void WaitIdle() override
			{
				++LastFenceValue;
				Queue->Signal(Fence.Get(), LastFenceValue);
				WaitForFence(LastFenceValue);
			}

			void BeginFrame() override
			{
				FrameState& Frame = CurrentFrame();
				List = Frame.List.Get();
				Frame.Allocator->Reset();
				List->Reset(Frame.Allocator.Get(), nullptr);

				ID3D12DescriptorHeap* Heaps[] = { GpuHeap.Get() };
				List->SetDescriptorHeaps(1, Heaps);
				List->SetGraphicsRootSignature(RootSignature.Get());
				List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

				BackbufferIndex = Swapchain->GetCurrentBackBufferIndex();
				if (Vendor != nullptr)
				{
					Vendor->BeginRenderSubmit();
				}
			}

			void EndFrame() override
			{
				FrameState& Frame = CurrentFrame();
				TextureSlot& Back = Textures[Backbuffers[BackbufferIndex].Id - 1];

				if (bCaptureRequested)
				{
					RecordCapture(Back);
				}
				Transition(Back, D3D12_RESOURCE_STATE_PRESENT);
				FlushBarriers();

				List->Close();
				Frame.UploadList->Close();
				ID3D12CommandList* Lists[] = { Frame.UploadList.Get(), List };
				Queue->ExecuteCommandLists(2, Lists);

				if (Vendor != nullptr)
				{
					Vendor->EndRenderSubmit();
					Vendor->BeginPresent();
				}
				const HRESULT Presented = Swapchain->Present(0, 0);
				if (Vendor != nullptr)
				{
					Vendor->EndPresent();
				}
				if (FAILED(Presented))
				{
					Log("D3D12: Present failed: 0x%08X, device reason 0x%08X", static_cast<unsigned>(Presented), static_cast<unsigned>(D3D->GetDeviceRemovedReason()));
					ReportLostDevice();
				}

				++LastFenceValue;
				Queue->Signal(Fence.Get(), LastFenceValue);
				Frame.FenceValue = LastFenceValue;

				if (bCaptureRequested)
				{
					WaitForFence(LastFenceValue);
					ResolveCapture();
				}

				FrameIndex = (FrameIndex + 1) % FrameCount;
				FrameState& Next = CurrentFrame();
				WaitForFence(Next.FenceValue);
				Next.Garbage.clear();
				Next.RingOffset = 0;
				Next.ConstantOffset = RingSize - ConstantRegion;
				Next.DescriptorOffset = 0;
				Next.UploadAllocator->Reset();
				Next.UploadList->Reset(Next.UploadAllocator.Get(), nullptr);
				List = nullptr;
			}

			TextureHandle GetBackbuffer() const override { return Backbuffers[BackbufferIndex]; }
			EFormat GetBackbufferFormat() const override { return EFormat::BGRA8Unorm; }

			void BeginPass(const RenderPass& Pass) override
			{
				for (const TextureHandle Read : Pass.Reads)
				{
					if (Read)
					{
						TextureSlot& Slot = Textures[Read.Id - 1];
						Transition(Slot, Slot.Desc.Format == EFormat::D32Float ? DepthReadState : ColorReadState);
					}
				}

				D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle = {};
				D3D12_CPU_DESCRIPTOR_HANDLE DsvHandle = {};
				UINT Width = 1;
				UINT Height = 1;

				if (Pass.Color)
				{
					TextureSlot& Slot = Textures[Pass.Color.Id - 1];
					Transition(Slot, D3D12_RESOURCE_STATE_RENDER_TARGET);
					RtvHandle = Slot.Rtv;
					Width = Slot.Desc.Width;
					Height = Slot.Desc.Height;
				}
				if (Pass.Depth)
				{
					TextureSlot& Slot = Textures[Pass.Depth.Id - 1];
					Transition(Slot, Pass.bDepthReadOnly ? DepthReadState : D3D12_RESOURCE_STATE_DEPTH_WRITE);
					DsvHandle = Pass.bDepthReadOnly ? Slot.DsvReadOnly : Slot.Dsv;
					Width = Slot.Desc.Width;
					Height = Slot.Desc.Height;
				}
				FlushBarriers();

				List->OMSetRenderTargets(Pass.Color ? 1 : 0, Pass.Color ? &RtvHandle : nullptr, FALSE, Pass.Depth ? &DsvHandle : nullptr);
				if (Pass.bClearColor)
				{
					List->ClearRenderTargetView(RtvHandle, Pass.ClearColor, 0, nullptr);
				}
				if (Pass.bClearDepth)
				{
					List->ClearDepthStencilView(DsvHandle, D3D12_CLEAR_FLAG_DEPTH, Pass.ClearDepth, 0, 0, nullptr);
				}
				SetViewport(0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height));
			}

			void EndPass() override
			{
			}

			void SetPipeline(PipelineHandle Handle) override
			{
				CurrentPipeline = &Pipelines[Handle.Id - 1];
				List->SetPipelineState(CurrentPipeline->State.Get());
			}

			void SetViewport(float X, float Y, float Width, float Height) override
			{
				const D3D12_VIEWPORT Viewport = { X, Y, Width, Height, 0.0f, 1.0f };
				const D3D12_RECT Scissor = { static_cast<LONG>(X), static_cast<LONG>(Y), static_cast<LONG>(X + Width), static_cast<LONG>(Y + Height) };
				List->RSSetViewports(1, &Viewport);
				List->RSSetScissorRects(1, &Scissor);
			}

			void SetVertexBuffer(std::uint32_t Stream, BufferHandle Handle) override
			{
				const BufferSlot& Slot = Buffers[Handle.Id - 1];
				D3D12_VERTEX_BUFFER_VIEW View = {};
				View.BufferLocation = Slot.Resource->GetGPUVirtualAddress();
				View.SizeInBytes = static_cast<UINT>(Slot.Size);
				View.StrideInBytes = CurrentPipeline->Strides[Stream];
				List->IASetVertexBuffers(Stream, 1, &View);
			}

			void* SetTransientVertexBuffer(std::uint32_t Stream, std::size_t Size) override
			{
				const UploadSpan Span = AllocateUpload(Size, 16);
				D3D12_VERTEX_BUFFER_VIEW View = {};
				View.BufferLocation = Span.Address;
				View.SizeInBytes = static_cast<UINT>(Size);
				View.StrideInBytes = CurrentPipeline->Strides[Stream];
				List->IASetVertexBuffers(Stream, 1, &View);
				return Span.Data;
			}

			void SetIndexBuffer(BufferHandle Handle) override
			{
				const BufferSlot& Slot = Buffers[Handle.Id - 1];
				D3D12_INDEX_BUFFER_VIEW View = {};
				View.BufferLocation = Slot.Resource->GetGPUVirtualAddress();
				View.SizeInBytes = static_cast<UINT>(Slot.Size);
				View.Format = DXGI_FORMAT_R32_UINT;
				List->IASetIndexBuffer(&View);
			}

			void SetConstants(std::uint32_t Slot, const void* Data, std::size_t Size) override
			{
				FrameState& Frame = CurrentFrame();
				const UINT64 Offset = Frame.ConstantOffset;
				Frame.ConstantOffset = AlignUp(Offset + Size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
				std::memcpy(Frame.RingData + Offset, Data, Size);
				List->SetGraphicsRootConstantBufferView(RootConstants + Slot, Frame.Ring->GetGPUVirtualAddress() + Offset);
			}

			void SetPushConstants(const void* Data, std::size_t Size) override
			{
				List->SetGraphicsRoot32BitConstants(RootPush, static_cast<UINT>(Size / 4), Data, 0);
			}

			void SetTextures(const TextureHandle* Handles, int Count) override
			{
				FrameState& Frame = CurrentFrame();
				const UINT Base = FrameIndex * DescriptorsPerFrame + Frame.DescriptorOffset;
				Frame.DescriptorOffset += MaxTextureBindings;

				for (int Index = 0; Index < MaxTextureBindings; ++Index)
				{
					const TextureHandle Handle = Index < Count && Handles[Index] ? Handles[Index] : WhiteTexture;
					const D3D12_CPU_DESCRIPTOR_HANDLE Target = { GpuCpuStart.ptr + static_cast<SIZE_T>(Base + Index) * GpuIncrement };
					D3D->CopyDescriptorsSimple(1, Target, Textures[Handle.Id - 1].Srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				}
				List->SetGraphicsRootDescriptorTable(RootTextures, { GpuGpuStart.ptr + static_cast<UINT64>(Base) * GpuIncrement });
			}

			void Draw(std::uint32_t Vertices, std::uint32_t Instances, std::uint32_t FirstVertex) override
			{
				List->DrawInstanced(Vertices, Instances, FirstVertex, 0);
			}

			void DrawIndexed(std::uint32_t Indices, std::uint32_t Instances) override
			{
				List->DrawIndexedInstanced(Indices, Instances, 0, 0, 0);
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
				OutWidth = CaptureWidth;
				OutHeight = CaptureHeight;
				return true;
			}

		private:
			template <typename SlotType>
			static std::uint32_t Store(std::vector<SlotType>& Slots, std::vector<std::uint32_t>& FreeSlots, SlotType&& Slot)
			{
				if (!FreeSlots.empty())
				{
					const std::uint32_t Index = FreeSlots.back();
					FreeSlots.pop_back();
					Slots[Index] = std::move(Slot);
					return Index + 1;
				}
				Slots.push_back(std::move(Slot));
				return static_cast<std::uint32_t>(Slots.size());
			}

			FrameState& CurrentFrame() { return Frames[FrameIndex]; }

			bool CreateLogicalDevice()
			{
				ComPtr<IDXGIFactory6> Factory6;
				if (FAILED(Factory.As(&Factory6)))
				{
					return false;
				}

				using CreateDeviceFunction = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
				void* CreateDevice = GetStreamlineExport("D3D12CreateDevice");
				const CreateDeviceFunction MakeDevice = CreateDevice != nullptr ? reinterpret_cast<CreateDeviceFunction>(CreateDevice) : &D3D12CreateDevice;

				ComPtr<IDXGIAdapter1> Adapter;
				for (UINT Index = 0; Factory6->EnumAdapterByGpuPreference(Index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&Adapter)) != DXGI_ERROR_NOT_FOUND; ++Index)
				{
					DXGI_ADAPTER_DESC1 Info = {};
					Adapter->GetDesc1(&Info);
					if ((Info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0
						&& SUCCEEDED(MakeDevice(Adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&D3D))))
					{
						AdapterName = ToUtf8(Info.Description);
						AdapterLuid = Info.AdapterLuid;
						return true;
					}
				}
				return false;
			}

			/** Copies pixels into a texture in the copy state, and leaves it ready to sample. */
			void RecordPixels(const TextureSlot& Slot, const void* Pixels)
			{
				const TextureDesc& Desc = Slot.Desc;
				const UINT Subresources = Desc.MipLevels * Desc.Layers;
				std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Footprints(Subresources);
				UINT64 RequiredBytes = 0;
				const D3D12_RESOURCE_DESC CreatedDesc = Slot.Resource->GetDesc();
				D3D->GetCopyableFootprints(&CreatedDesc, 0, Subresources, 0, Footprints.data(), nullptr, nullptr, &RequiredBytes);

				const UploadSpan Upload = AllocateUpload(RequiredBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
				ID3D12GraphicsCommandList* Uploads = CurrentFrame().UploadList.Get();
				const std::uint8_t* Source = static_cast<const std::uint8_t*>(Pixels);

				// Subresources are numbered by mip within layer, the order the pixels come in.
				for (UINT Index = 0; Index < Subresources; ++Index)
				{
					D3D12_PLACED_SUBRESOURCE_FOOTPRINT& Footprint = Footprints[Index];
					const UINT Mip = Index % Desc.MipLevels;
					const UINT Height = std::max(Desc.Height >> Mip, 1u);
					const UINT RowBytes = std::max(Desc.Width >> Mip, 1u) * BytesPerPixel(Desc.Format);
					for (UINT Slice = 0; Slice < Desc.Depth; ++Slice)
					{
						const UINT64 SliceOffset = Footprint.Offset + static_cast<UINT64>(Slice) * Height * Footprint.Footprint.RowPitch;
						for (UINT Row = 0; Row < Height; ++Row)
						{
							std::memcpy(Upload.Data + SliceOffset + Row * Footprint.Footprint.RowPitch, Source, RowBytes);
							Source += RowBytes;
						}
					}
					Footprint.Offset += Upload.Offset;

					D3D12_TEXTURE_COPY_LOCATION Target = {};
					Target.pResource = Slot.Resource.Get();
					Target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					Target.SubresourceIndex = Index;
					D3D12_TEXTURE_COPY_LOCATION From = {};
					From.pResource = Upload.Resource;
					From.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					From.PlacedFootprint = Footprint;
					Uploads->CopyTextureRegion(&Target, 0, 0, 0, &From, nullptr);
				}

				D3D12_RESOURCE_BARRIER Barrier = {};
				Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				Barrier.Transition.pResource = Slot.Resource.Get();
				Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
				Barrier.Transition.StateAfter = ColorReadState;
				Uploads->ResourceBarrier(1, &Barrier);
			}

			/** Says what each command list had reached, where the validation option recorded that. */
			void ReportLostDevice()
			{
				ComPtr<ID3D12DeviceRemovedExtendedData1> Details;
				if (bDeviceLossReported || FAILED(D3D.As(&Details)))
				{
					return;
				}
				bDeviceLossReported = true;

				D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 Crumbs = {};
				if (SUCCEEDED(Details->GetAutoBreadcrumbsOutput1(&Crumbs)))
				{
					for (const D3D12_AUTO_BREADCRUMB_NODE1* Node = Crumbs.pHeadAutoBreadcrumbNode; Node != nullptr; Node = Node->pNext)
					{
						const UINT Reached = Node->pLastBreadcrumbValue != nullptr ? *Node->pLastBreadcrumbValue : 0;
						if (Reached < Node->BreadcrumbCount && Node->pCommandHistory != nullptr)
						{
							std::string Recent;
							for (UINT Index = Reached > 3 ? Reached - 3 : 0; Index < std::min(Reached + 3, Node->BreadcrumbCount); ++Index)
							{
								Recent += (Index == Reached ? " >" : " ") + std::to_string(static_cast<int>(Node->pCommandHistory[Index]));
							}
							Log("D3D12: list stopped at %u of %u, operations%s", Reached, Node->BreadcrumbCount, Recent.c_str());
						}
					}
				}

				D3D12_DRED_PAGE_FAULT_OUTPUT1 Fault = {};
				if (SUCCEEDED(Details->GetPageFaultAllocationOutput1(&Fault)) && Fault.PageFaultVA != 0)
				{
					Log("D3D12: page fault at 0x%llX", static_cast<unsigned long long>(Fault.PageFaultVA));
				}
			}

			void DetectRayTracing()
			{
				D3D12_FEATURE_DATA_D3D12_OPTIONS5 Options = {};
				D3D12_FEATURE_DATA_SHADER_MODEL Model = { D3D_SHADER_MODEL_6_5 };
				bRayTracing = SUCCEEDED(D3D->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &Options, sizeof(Options)))
					&& Options.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1
					&& SUCCEEDED(D3D->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &Model, sizeof(Model)))
					&& Model.HighestShaderModel >= D3D_SHADER_MODEL_6_5
					&& SUCCEEDED(D3D.As(&RayDevice));
			}

			ComPtr<ID3D12Resource> CreateRayBuffer(UINT64 Size, D3D12_RESOURCE_STATES State)
			{
				D3D12_RESOURCE_DESC Desc = MakeBufferDesc(Size);
				Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				const D3D12_HEAP_PROPERTIES Heap = { D3D12_HEAP_TYPE_DEFAULT };
				ComPtr<ID3D12Resource> Buffer;
				if (FAILED(D3D->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, State, nullptr, IID_PPV_ARGS(&Buffer))))
				{
					Log("D3D12: failed to create a ray tracing buffer of %llu bytes", static_cast<unsigned long long>(Size));
				}
				return Buffer;
			}

			bool CreateDescriptorHeaps()
			{
				if (!RtvHeap.Init(D3D.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, TargetDescriptors)
					|| !DsvHeap.Init(D3D.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, TargetDescriptors)
					|| !CpuHeap.Init(D3D.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, CpuDescriptors))
				{
					return false;
				}

				D3D12_DESCRIPTOR_HEAP_DESC Desc = {};
				Desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				Desc.NumDescriptors = FrameCount * DescriptorsPerFrame;
				Desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				if (FAILED(D3D->CreateDescriptorHeap(&Desc, IID_PPV_ARGS(&GpuHeap))))
				{
					return false;
				}
				GpuCpuStart = GpuHeap->GetCPUDescriptorHandleForHeapStart();
				GpuGpuStart = GpuHeap->GetGPUDescriptorHandleForHeapStart();
				GpuIncrement = D3D->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				return true;
			}

			bool CreateRootSignature()
			{
				D3D12_DESCRIPTOR_RANGE Range = {};
				Range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
				Range.NumDescriptors = MaxTextureBindings;

				D3D12_DESCRIPTOR_RANGE SceneRange = {};
				SceneRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
				SceneRange.NumDescriptors = MaxRayBuffers;
				SceneRange.BaseShaderRegister = 7;

				D3D12_ROOT_PARAMETER Parameters[6] = {};
				Parameters[RootScene].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
				Parameters[RootScene].Descriptor.ShaderRegister = 6;
				Parameters[RootSceneVertices].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				Parameters[RootSceneVertices].DescriptorTable.NumDescriptorRanges = 1;
				Parameters[RootSceneVertices].DescriptorTable.pDescriptorRanges = &SceneRange;
				Parameters[RootPush].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
				Parameters[RootPush].Constants.ShaderRegister = 2;
				Parameters[RootPush].Constants.Num32BitValues = MaxPushConstantBytes / 4;
				Parameters[RootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
				Parameters[RootConstants].Descriptor.ShaderRegister = 0;
				Parameters[RootConstants + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
				Parameters[RootConstants + 1].Descriptor.ShaderRegister = 1;
				Parameters[RootTextures].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				Parameters[RootTextures].DescriptorTable.NumDescriptorRanges = 1;
				Parameters[RootTextures].DescriptorTable.pDescriptorRanges = &Range;

				const auto MakeSampler = [](UINT Register, D3D12_FILTER Filter, D3D12_TEXTURE_ADDRESS_MODE Address)
				{
					D3D12_STATIC_SAMPLER_DESC Sampler = {};
					Sampler.Filter = Filter;
					Sampler.AddressU = Address;
					Sampler.AddressV = Address;
					Sampler.AddressW = Address;
					Sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
					Sampler.MaxAnisotropy = MaxAnisotropy;
					Sampler.MaxLOD = D3D12_FLOAT32_MAX;
					Sampler.ShaderRegister = Register;
					Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
					return Sampler;
				};
				const D3D12_STATIC_SAMPLER_DESC Samplers[] = {
					MakeSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
					MakeSampler(1, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP),
					MakeSampler(2, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
					MakeSampler(3, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP) };

				D3D12_ROOT_SIGNATURE_DESC Desc = {};
				Desc.NumParameters = bRayTracing ? 6 : 4;
				Desc.pParameters = Parameters;
				Desc.NumStaticSamplers = 4;
				Desc.pStaticSamplers = Samplers;
				Desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

				ComPtr<ID3DBlob> Blob;
				ComPtr<ID3DBlob> Error;
				if (FAILED(D3D12SerializeRootSignature(&Desc, D3D_ROOT_SIGNATURE_VERSION_1, &Blob, &Error)))
				{
					Log("D3D12: root signature: %s", Error != nullptr ? static_cast<const char*>(Error->GetBufferPointer()) : "unknown error");
					return false;
				}
				return SUCCEEDED(D3D->CreateRootSignature(0, Blob->GetBufferPointer(), Blob->GetBufferSize(), IID_PPV_ARGS(&RootSignature)));
			}

			ComPtr<ID3D12Resource> CreateUploadBuffer(UINT64 Size)
			{
				const D3D12_RESOURCE_DESC Desc = MakeBufferDesc(Size);
				const D3D12_HEAP_PROPERTIES Heap = { D3D12_HEAP_TYPE_UPLOAD };
				ComPtr<ID3D12Resource> Buffer;
				D3D->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Buffer));
				return Buffer;
			}

			bool CreateFrames()
			{
				for (FrameState& Frame : Frames)
				{
					if (FAILED(D3D->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Frame.Allocator)))
						|| FAILED(D3D->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Frame.UploadAllocator)))
						|| FAILED(D3D->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Frame.Allocator.Get(), nullptr, IID_PPV_ARGS(&Frame.List)))
						|| FAILED(D3D->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Frame.UploadAllocator.Get(), nullptr, IID_PPV_ARGS(&Frame.UploadList))))
					{
						return false;
					}
					Frame.List->Close();
					Frame.UploadList->Close();

					if (bRayTracing && (FAILED(Frame.List.As(&Frame.RayList)) || FAILED(Frame.UploadList.As(&Frame.RayUploadList))))
					{
						Log("D3D12: command lists have no ray tracing interface");
						bRayTracing = false;
					}

					Frame.Ring = CreateUploadBuffer(RingSize);
					if (Frame.Ring == nullptr)
					{
						return false;
					}
					Frame.Ring->Map(0, nullptr, reinterpret_cast<void**>(&Frame.RingData));
					Frame.ConstantOffset = RingSize - ConstantRegion;
				}

				// The upload list is always open, ready for resources made between frames.
				CurrentFrame().UploadAllocator->Reset();
				CurrentFrame().UploadList->Reset(CurrentFrame().UploadAllocator.Get(), nullptr);
				return true;
			}

			bool CreateSwapchain(std::uint32_t Width, std::uint32_t Height)
			{
				DXGI_SWAP_CHAIN_DESC1 Desc = {};
				Desc.Width = Width;
				Desc.Height = Height;
				Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				Desc.SampleDesc.Count = 1;
				Desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
				Desc.BufferCount = BackbufferCount;
				Desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

				ComPtr<IDXGISwapChain1> Base;
				if (FAILED(Factory->CreateSwapChainForHwnd(Queue.Get(), Window, &Desc, nullptr, nullptr, &Base)) || FAILED(Base.As(&Swapchain)))
				{
					return false;
				}
				Factory->MakeWindowAssociation(Window, DXGI_MWA_NO_ALT_ENTER);
				AcquireBackbuffers();
				return true;
			}

			void AcquireBackbuffers()
			{
				DXGI_SWAP_CHAIN_DESC1 Desc = {};
				Swapchain->GetDesc1(&Desc);

				for (UINT Index = 0; Index < BackbufferCount; ++Index)
				{
					TextureSlot Slot;
					Slot.Desc.Width = Desc.Width;
					Slot.Desc.Height = Desc.Height;
					Slot.Desc.Format = EFormat::BGRA8Unorm;
					Slot.Desc.bRenderTarget = true;
					Slot.State = D3D12_RESOURCE_STATE_PRESENT;
					Swapchain->GetBuffer(Index, IID_PPV_ARGS(&Slot.Resource));
					Slot.Rtv = RtvHeap.Allocate();
					D3D->CreateRenderTargetView(Slot.Resource.Get(), nullptr, Slot.Rtv);
					Backbuffers[Index] = { Store(Textures, FreeTextures, std::move(Slot)) };
				}
			}

			void ReleaseViews(const TextureSlot& Slot)
			{
				if (Slot.Srv.ptr != 0)
				{
					CpuHeap.Free(Slot.Srv);
				}
				if (Slot.Rtv.ptr != 0)
				{
					RtvHeap.Free(Slot.Rtv);
				}
				if (Slot.Dsv.ptr != 0)
				{
					DsvHeap.Free(Slot.Dsv);
					DsvHeap.Free(Slot.DsvReadOnly);
				}
			}

			void ReleaseBackbuffers()
			{
				for (const TextureHandle Handle : Backbuffers)
				{
					TextureSlot& Slot = Textures[Handle.Id - 1];
					ReleaseViews(Slot);
					Slot = {};
					FreeTextures.push_back(Handle.Id - 1);
				}
			}

			UploadSpan AllocateUpload(UINT64 Size, UINT64 Alignment)
			{
				FrameState& Frame = CurrentFrame();
				const UINT64 Offset = AlignUp(Frame.RingOffset, Alignment);
				if (Offset + Size <= RingSize - ConstantRegion)
				{
					Frame.RingOffset = Offset + Size;
					return { Frame.Ring.Get(), Offset, Frame.Ring->GetGPUVirtualAddress() + Offset, Frame.RingData + Offset };
				}

				// Bursts larger than the ring get a buffer of their own for this frame.
				ComPtr<ID3D12Resource> Extra = CreateUploadBuffer(Size);
				std::uint8_t* Data = nullptr;
				Extra->Map(0, nullptr, reinterpret_cast<void**>(&Data));
				UploadSpan Span = { Extra.Get(), 0, Extra->GetGPUVirtualAddress(), Data };
				Frame.Garbage.push_back(std::move(Extra));
				return Span;
			}

			void Transition(TextureSlot& Slot, D3D12_RESOURCE_STATES State)
			{
				if (Slot.State == State)
				{
					return;
				}
				D3D12_RESOURCE_BARRIER Barrier = {};
				Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				Barrier.Transition.pResource = Slot.Resource.Get();
				Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				Barrier.Transition.StateBefore = Slot.State;
				Barrier.Transition.StateAfter = State;
				PendingBarriers.push_back(Barrier);
				Slot.State = State;
			}

			void FlushBarriers()
			{
				if (!PendingBarriers.empty())
				{
					List->ResourceBarrier(static_cast<UINT>(PendingBarriers.size()), PendingBarriers.data());
					PendingBarriers.clear();
				}
			}

			void WaitForFence(UINT64 Value)
			{
				if (Fence->GetCompletedValue() < Value)
				{
					Fence->SetEventOnCompletion(Value, FenceEvent);
					WaitForSingleObject(FenceEvent, INFINITE);
				}
			}

			void RecordCapture(TextureSlot& Back)
			{
				const D3D12_RESOURCE_DESC Desc = Back.Resource->GetDesc();
				UINT64 RequiredBytes = 0;
				D3D->GetCopyableFootprints(&Desc, 0, 1, 0, &CaptureFootprint, nullptr, nullptr, &RequiredBytes);

				const D3D12_RESOURCE_DESC BufferDesc = MakeBufferDesc(RequiredBytes);
				const D3D12_HEAP_PROPERTIES Heap = { D3D12_HEAP_TYPE_READBACK };
				CaptureBuffer.Reset();
				D3D->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &BufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&CaptureBuffer));

				Transition(Back, D3D12_RESOURCE_STATE_COPY_SOURCE);
				FlushBarriers();

				D3D12_TEXTURE_COPY_LOCATION Source = {};
				Source.pResource = Back.Resource.Get();
				Source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				D3D12_TEXTURE_COPY_LOCATION Target = {};
				Target.pResource = CaptureBuffer.Get();
				Target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				Target.PlacedFootprint = CaptureFootprint;
				List->CopyTextureRegion(&Target, 0, 0, 0, &Source, nullptr);

				CaptureWidth = static_cast<std::uint32_t>(Desc.Width);
				CaptureHeight = Desc.Height;
			}

			void ResolveCapture()
			{
				const std::uint8_t* Mapped = nullptr;
				CaptureBuffer->Map(0, nullptr, reinterpret_cast<void**>(const_cast<std::uint8_t**>(&Mapped)));

				CapturePixels.resize(static_cast<std::size_t>(CaptureWidth) * CaptureHeight * 4);
				for (std::uint32_t Row = 0; Row < CaptureHeight; ++Row)
				{
					const std::uint8_t* Source = Mapped + Row * CaptureFootprint.Footprint.RowPitch;
					std::uint8_t* Target = CapturePixels.data() + static_cast<std::size_t>(Row) * CaptureWidth * 4;
					for (std::uint32_t Column = 0; Column < CaptureWidth; ++Column)
					{
						Target[Column * 4 + 0] = Source[Column * 4 + 2];
						Target[Column * 4 + 1] = Source[Column * 4 + 1];
						Target[Column * 4 + 2] = Source[Column * 4 + 0];
						Target[Column * 4 + 3] = 255;
					}
				}
				CaptureBuffer->Unmap(0, nullptr);
				CaptureBuffer.Reset();
				bCaptureRequested = false;
			}

			HWND Window = nullptr;
			std::string AdapterName;
			LUID AdapterLuid = {};
			std::unique_ptr<VendorFeatures> Vendor;

			ComPtr<IDXGIFactory4> Factory;
			ComPtr<ID3D12Device> D3D;
			ComPtr<ID3D12Device5> RayDevice;
			bool bRayTracing = false;
			bool bDeviceLossReported = false;
			ComPtr<ID3D12CommandQueue> Queue;
			ComPtr<IDXGISwapChain3> Swapchain;
			ComPtr<ID3D12Fence> Fence;
			ComPtr<ID3D12RootSignature> RootSignature;
			HANDLE FenceEvent = nullptr;
			UINT64 LastFenceValue = 0;

			DescriptorPool RtvHeap;
			DescriptorPool DsvHeap;
			DescriptorPool CpuHeap;
			ComPtr<ID3D12DescriptorHeap> GpuHeap;
			D3D12_CPU_DESCRIPTOR_HANDLE GpuCpuStart = {};
			D3D12_GPU_DESCRIPTOR_HANDLE GpuGpuStart = {};
			UINT GpuIncrement = 0;

			FrameState Frames[FrameCount];
			UINT FrameIndex = 0;
			UINT BackbufferIndex = 0;
			TextureHandle Backbuffers[BackbufferCount];
			ID3D12GraphicsCommandList* List = nullptr;
			const PipelineSlot* CurrentPipeline = nullptr;
			std::vector<D3D12_RESOURCE_BARRIER> PendingBarriers;

			std::vector<BufferSlot> Buffers;
			std::vector<TextureSlot> Textures;
			std::vector<PipelineSlot> Pipelines;
			std::vector<ComPtr<ID3D12Resource>> Accelerations;
			std::vector<std::uint32_t> FreeAccelerations;
			std::vector<std::uint32_t> FreeBuffers;
			std::vector<std::uint32_t> FreeTextures;
			std::vector<std::uint32_t> FreePipelines;
			TextureHandle WhiteTexture;

			bool bCaptureRequested = false;
			ComPtr<ID3D12Resource> CaptureBuffer;
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT CaptureFootprint = {};
			std::vector<std::uint8_t> CapturePixels;
			std::uint32_t CaptureWidth = 0;
			std::uint32_t CaptureHeight = 0;
		};
	}

	std::unique_ptr<Device> CreateD3D12Device(const DeviceDesc& Desc)
	{
		auto Result = std::make_unique<D3D12Device>();
		if (!Result->Init(Desc))
		{
			return nullptr;
		}
		return Result;
	}
}
