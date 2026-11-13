// D3D12 device: adapter, debug layer, bindless heap, root signature, frame loop, upload ring, deferred deletion.
#include <cstdio>
#include <cstring>

#include "d3d12_internal.h"
#include "rhi/streamline.h"

namespace taelia::rhi::d3d12 {

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

void setName(ID3D12Object* o, const std::string& name) {
    if (o && !name.empty()) o->SetName(toWide(name).c_str());
}

DXGI_FORMAT toDxgi(Format f) {
    switch (f) {
        case Format::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::BGRA8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::RGB10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case Format::R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
        case Format::RGBA16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::RGBA32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::RG16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
        case Format::R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
        case Format::R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case Format::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
        case Format::RG8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
        case Format::R32_UINT: return DXGI_FORMAT_R32_UINT;
        case Format::D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
        case Format::Unknown: break;
    }
    return DXGI_FORMAT_UNKNOWN;
}

D3D12_RESOURCE_STATES toD3D(State s, bool isBuffer) {
    switch (s) {
        case State::Undefined: return D3D12_RESOURCE_STATE_COMMON;
        case State::ShaderRead:
            return isBuffer ? D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
                            : D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        case State::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case State::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case State::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case State::DepthRead: return D3D12_RESOURCE_STATE_DEPTH_READ;
        case State::CopySrc: return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case State::CopyDst: return D3D12_RESOURCE_STATE_COPY_DEST;
        case State::Present: return D3D12_RESOURCE_STATE_PRESENT;
        case State::IndexBuffer: return D3D12_RESOURCE_STATE_INDEX_BUFFER;
        case State::IndirectArgument: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

// ------------------------------------------------------------------------------------------- descriptor pools

bool DescriptorPool::init(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shaderVisible,
                          bool reserveFirst) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = type;
    hd.NumDescriptors = count;
    hd.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return false;
    inc = dev->GetDescriptorHandleIncrementSize(type);
    capacity = count;
    reserveZero = reserveFirst;
    next = reserveFirst ? 1 : 0;
    cpuBase = heap->GetCPUDescriptorHandleForHeapStart();
    if (shaderVisible) gpuBase = heap->GetGPUDescriptorHandleForHeapStart();
    return true;
}

uint32_t DescriptorPool::alloc() {
    if (!freeList.empty()) {
        const uint32_t i = freeList.back();
        freeList.pop_back();
        return i;
    }
    if (next >= capacity) {
        std::fprintf(stderr, "d3d12: descriptor heap full (%u)\n", capacity);
        return kInvalid;
    }
    return next++;
}

void DescriptorPool::free(uint32_t i) {
    if (i != kInvalid && !(reserveZero && i == 0)) freeList.push_back(i);
}

// ------------------------------------------------------------------------------------------- debug messages

namespace {
bool isBenign(D3D12_MESSAGE_ID id) {
    // Clearing with a value other than the optimized clear value is legal and only slower.
    return id == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE ||
           id == D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE;
}

void __stdcall onDebugMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                              LPCSTR text, void* context) {
    if (severity > D3D12_MESSAGE_SEVERITY_WARNING || isBenign(id)) return;
    auto* dev = static_cast<D3D12Device*>(context);
    dev->debugMessages.fetch_add(1);
    const char* sev = severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION"
                      : severity == D3D12_MESSAGE_SEVERITY_ERROR    ? "ERROR"
                                                                    : "WARNING";
    char buf[2048];
    std::snprintf(buf, sizeof buf, "D3D12 %s #%d: %s\n", sev, int(id), text ? text : "");
    std::fputs(buf, stderr);
    OutputDebugStringA(buf);
}
}  // namespace

// ------------------------------------------------------------------------------------------- device

bool D3D12Device::init(const DeviceDesc& desc, std::string* error) {
    auto fail = [&](const char* what, HRESULT hr) {
        if (error) {
            char buf[256];
            std::snprintf(buf, sizeof buf, "%s failed (hr 0x%08X)", what, unsigned(hr));
            *error = buf;
        }
        return false;
    };
    shaderDir_ = desc.shaderDir;
    if (desc.debug) {
        ComPtr<ID3D12Debug> dbg;
        const HRESULT dhr = D3D12GetDebugInterface(IID_PPV_ARGS(&dbg));
        if (SUCCEEDED(dhr))
            dbg->EnableDebugLayer();
        else
            std::fprintf(stderr,
                         "d3d12: debug layer unavailable (hr 0x%08X); the Agility SDK layers also need the Windows "
                         "'Graphics Tools' optional feature\n",
                         unsigned(dhr));
    }
    if (desc.streamline) {
        std::string why;
        slActive = streamline::load(false, &why);
        if (!slActive) std::fprintf(stderr, "d3d12: Streamline unavailable: %s\n", why.c_str());
    }
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return fail("CreateDXGIFactory2", hr);
    BOOL allowTearing = FALSE;
    if (SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof allowTearing)))
        tearing = allowTearing != FALSE;

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) ==
            DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 ad{};
        a->GetDesc1(&ad);
        if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
            adapter = a;
            char name[256];
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, nullptr, nullptr);
            adapterName_ = name;
            break;
        }
    }
    if (!device) return fail("D3D12CreateDevice (no suitable adapter)", E_FAIL);
    if (slActive) initStreamline();

    if (desc.debug && SUCCEEDED(device.As(&infoQueue_))) {
        infoQueue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        infoQueue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        infoQueue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
        D3D12_MESSAGE_ID deny[] = {D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                                   D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE};
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumIDs = UINT(std::size(deny));
        filter.DenyList.pIDList = deny;
        infoQueue_->PushStorageFilter(&filter);
        const HRESULT chr = infoQueue_->RegisterMessageCallback(onDebugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, this,
                                                                &callbackCookie_);
        if (SUCCEEDED(chr))
            debugActive_ = true;
        else
            std::fprintf(stderr, "d3d12: RegisterMessageCallback failed (hr 0x%08X)\n", unsigned(chr));
    } else if (desc.debug) {
        std::fprintf(stderr, "d3d12: ID3D12InfoQueue1 unavailable, debug messages are not counted\n");
    }

    D3D12MA::ALLOCATOR_DESC ad{};
    ad.pDevice = device.Get();
    ad.pAdapter = adapter.Get();
    hr = D3D12MA::CreateAllocator(&ad, &allocator);
    if (FAILED(hr)) return fail("D3D12MA::CreateAllocator", hr);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) return fail("CreateCommandQueue", hr);
    setName(queue.Get(), "rhi.queue");
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) return fail("CreateFence", hr);
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    for (auto& a : frameAlloc_) {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        if (FAILED(hr)) return fail("CreateCommandAllocator", hr);
    }
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&immAlloc_));
    if (FAILED(hr)) return fail("CreateCommandAllocator", hr);
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frameAlloc_[0].Get(), nullptr,
                                   IID_PPV_ARGS(&cmd_.cl));
    if (FAILED(hr)) return fail("CreateCommandList", hr);
    cmd_.cl->Close();
    cmd_.dev = this;
    setName(cmd_.cl.Get(), "rhi.frameList");
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, immAlloc_.Get(), nullptr, IID_PPV_ARGS(&immList_));
    if (FAILED(hr)) return fail("CreateCommandList", hr);
    immList_->Close();
    setName(immList_.Get(), "rhi.immediateList");

    if (!bindless.init(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kMaxBindless, true, true) ||
        !rtvs.init(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvCount, false, false) ||
        !dsvs.init(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false, false))
        return fail("CreateDescriptorHeap", E_FAIL);
    setName(bindless.heap.Get(), "rhi.bindless");
    {  // index 0: null descriptor
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(nullptr, &sd, bindless.cpu(0));
    }
    if (!createRootSignature(error)) return false;

    ring_ = createBuffer({kRingBytesPerFrame * kFramesInFlight,
                          BufferUsage::Storage | BufferUsage::Index | BufferUsage::Constant, Memory::Upload,
                          "rhi.uploadRing"},
                         nullptr);
    zeroConstants_ = createBuffer({kFrameConstantBytes, BufferUsage::Constant, Memory::Upload, "rhi.zeroConstants"},
                                  nullptr);
    if (!ring_ || !zeroConstants_ || !ring_->mapped() || !zeroConstants_->mapped())
        return fail("upload ring allocation", E_OUTOFMEMORY);
    std::memset(zeroConstants_->mapped(), 0, kFrameConstantBytes);
    return true;
}

bool D3D12Device::createRootSignature(std::string* error) {
    // One table: every bindless.hlsli declaration in its own register space, all unbounded at heap offset 0.
    struct RangeDecl {
        D3D12_DESCRIPTOR_RANGE_TYPE type;
        UINT space;
    };
    const RangeDecl decls[] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1}, {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2}, {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4}, {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 7}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 8}, {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 9},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 10}, {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 11},  // 11: TLAS
    };
    D3D12_DESCRIPTOR_RANGE1 ranges[std::size(decls)]{};
    for (size_t i = 0; i < std::size(decls); ++i) {
        ranges[i].RangeType = decls[i].type;
        ranges[i].NumDescriptors = UINT_MAX;
        ranges[i].BaseShaderRegister = 0;
        ranges[i].RegisterSpace = decls[i].space;
        // Descriptors change while bound (bindless); data too (written and read within one command list).
        ranges[i].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        ranges[i].OffsetInDescriptorsFromTableStart = 0;
    }
    D3D12_ROOT_PARAMETER1 params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {1, 0, kPushConstantBytes / 4};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {UINT(std::size(ranges)), ranges};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[6]{};
    auto sampler = [&](UINT reg, D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE mode) {
        D3D12_STATIC_SAMPLER_DESC& s = samplers[reg];
        s.Filter = filter;
        s.AddressU = s.AddressV = s.AddressW = mode;
        s.MaxAnisotropy = filter == D3D12_FILTER_ANISOTROPIC ? 16 : 1;
        s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        s.MinLOD = 0;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        s.ShaderRegister = reg;
        s.RegisterSpace = 0;
        s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    sampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    sampler(2, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    sampler(3, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    sampler(4, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    sampler(5, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[5].ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reverse-Z

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rd{};
    rd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rd.Desc_1_1.NumParameters = UINT(std::size(params));
    rd.Desc_1_1.pParameters = params;
    rd.Desc_1_1.NumStaticSamplers = UINT(std::size(samplers));
    rd.Desc_1_1.pStaticSamplers = samplers;
    rd.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&rd, &blob, &err);
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig));
    if (FAILED(hr)) {
        if (error) {
            *error = "root signature creation failed";
            if (err) *error += ": " + std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
        }
        return false;
    }
    setName(rootSig.Get(), "rhi.rootSignature");
    return true;
}

D3D12Device::~D3D12Device() {
    if (queue && fence) waitIdle();
    if (slActive) streamline::unload();  // before the device goes away
    slActive = false;
    ring_.reset();
    zeroConstants_.reset();
    collectGarbage(true);
    if (infoQueue_ && callbackCookie_) infoQueue_->UnregisterMessageCallback(callbackCookie_);
    cmd_.cl4.Reset();
    cmd_.cl.Reset();
    immList_.Reset();
    for (auto& s : cmdSigs) s.Reset();
    if (allocator) allocator->Release();
    if (fenceEvent) CloseHandle(fenceEvent);
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12Device::zeroConstantsVA() const {
    return static_cast<D3D12Buffer*>(zeroConstants_.get())->va;
}

void D3D12Device::waitFence(uint64_t value) {
    if (fence->GetCompletedValue() >= value) return;
    fence->SetEventOnCompletion(value, fenceEvent);
    WaitForSingleObject(fenceEvent, INFINITE);
}

void D3D12Device::defer(std::function<void()> fn) {
    // Inside a frame the frame's command list may still reference the resource: retire it with that frame's fence.
    garbage_.push_back({inFrame_ ? UINT64_MAX : fenceValue, std::move(fn)});
}

void D3D12Device::collectGarbage(bool all) {
    const uint64_t done = fence ? fence->GetCompletedValue() : UINT64_MAX;
    std::vector<Garbage> keep, run;
    for (auto& g : garbage_) (all || g.fence <= done ? run : keep).push_back(std::move(g));
    garbage_ = std::move(keep);
    for (auto& g : run) g.fn();
}

ID3D12GraphicsCommandList* D3D12Device::beginImmediate() {
    immAlloc_->Reset();
    immList_->Reset(immAlloc_.Get(), nullptr);
    return immList_.Get();
}

void D3D12Device::endImmediate() {
    immList_->Close();
    ID3D12CommandList* lists[] = {immList_.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), ++fenceValue);
    waitFence(fenceValue);
}

CommandList* D3D12Device::beginFrame(Swapchain* swapchain) {
    const uint32_t slot = uint32_t(frameNumber_ % kFramesInFlight);
    waitFence(frameFence_[slot]);
    collectGarbage(false);
    frameAlloc_[slot]->Reset();
    ID3D12GraphicsCommandList* cl = cmd_.cl.Get();
    cl->Reset(frameAlloc_[slot].Get(), nullptr);
    cmd_.pipeline = nullptr;
    ringBase_ = kRingBytesPerFrame * slot;
    ringOffset_ = 0;
    inFrame_ = true;

    std::memset(cmd_.push, 0, sizeof cmd_.push);
    cmd_.frameConstants = zeroConstantsVA();
    cmd_.restoreBindings();
    if (swapchain) swapchain->backbuffer();
    return &cmd_;
}

void D3D12Device::endFrame(Swapchain* swapchain) {
    const uint32_t slot = uint32_t(frameNumber_ % kFramesInFlight);
    auto* sc = static_cast<D3D12Swapchain*>(swapchain);
    if (sc) transition(cmd_.cl.Get(), static_cast<D3D12Texture*>(sc->backbuffer()), State::Present);
    cmd_.cl->Close();
    ID3D12CommandList* lists[] = {cmd_.cl.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (sc) {
        const bool reflex = latencySupported();
        if (reflex) latencyMarker(LatencyMarker::PresentStart);
        sc->present();
        if (reflex) latencyMarker(LatencyMarker::PresentEnd);
    }
    queue->Signal(fence.Get(), ++fenceValue);
    frameFence_[slot] = fenceValue;
    for (auto& g : garbage_)
        if (g.fence == UINT64_MAX) g.fence = fenceValue;
    inFrame_ = false;
    ++frameNumber_;
}

void D3D12Device::waitIdle() {
    queue->Signal(fence.Get(), ++fenceValue);
    waitFence(fenceValue);
    if (!inFrame_) collectGarbage(false);
}

Transient D3D12Device::allocTransient(uint64_t size) {
    const uint64_t offset = (ringOffset_ + 255) & ~uint64_t(255);
    if (!inFrame_ || offset + size > kRingBytesPerFrame) {
        std::fprintf(stderr, "d3d12: allocTransient(%llu) %s\n", (unsigned long long)size,
                     inFrame_ ? "overflows the per-frame upload ring" : "called outside beginFrame/endFrame");
        return {};
    }
    ringOffset_ = offset + size;
    Transient t;
    t.buffer = ring_.get();
    t.offset = ringBase_ + offset;
    t.cpu = static_cast<uint8_t*>(ring_->mapped()) + t.offset;
    t.size = size;
    return t;
}

}  // namespace taelia::rhi::d3d12

namespace taelia::rhi {
std::unique_ptr<Device> createD3D12Device(const DeviceDesc& desc, std::string* error) {
    auto dev = std::make_unique<d3d12::D3D12Device>();
    if (!dev->init(desc, error)) return nullptr;
    return dev;
}
}  // namespace taelia::rhi
