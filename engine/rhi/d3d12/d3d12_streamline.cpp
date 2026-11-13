// D3D12 side of NVIDIA Streamline (DLSS Super Resolution, Reflex) with manual hooking: handing the device over,
// upgrading the swapchain to the SL proxy, describing the DLSS images and restoring our bindings afterwards.
#include <cstdio>

#include "d3d12_internal.h"
#include "rhi/streamline.h"

#pragma warning(push, 0)
#include <sl.h>
#pragma warning(pop)

namespace taelia::rhi::d3d12 {

void D3D12Device::initStreamline() {
    auto* setDevice = reinterpret_cast<PFun_slSetD3DDevice*>(streamline::proc("slSetD3DDevice"));
    const sl::Result r = setDevice ? setDevice(device.Get()) : sl::Result::eErrorMissingOrInvalidAPI;
    if (r != sl::Result::eOk) {
        std::fprintf(stderr, "d3d12: slSetD3DDevice failed (%d), Streamline disabled\n", int(r));
        streamline::unload();
        slActive = false;
        return;
    }
    const LUID luid = device->GetAdapterLuid();
    streamline::checkSupport(&luid, uint32_t(sizeof luid), nullptr);
}

void D3D12Device::upgradeSwapchain(ComPtr<IDXGISwapChain1>& sc) {
    auto* upgrade = reinterpret_cast<PFun_slUpgradeInterface*>(streamline::proc("slUpgradeInterface"));
    if (!upgrade) return;
    // The proxy takes its own reference on the native swapchain; we keep only the proxy.
    void* p = sc.Get();
    const sl::Result r = upgrade(&p);
    if (r != sl::Result::eOk) {
        std::fprintf(stderr, "d3d12: slUpgradeInterface(swapchain) failed (%d)\n", int(r));
        return;
    }
    if (p != sc.Get()) {
        ComPtr<IDXGISwapChain1> proxy;
        proxy.Attach(static_cast<IDXGISwapChain1*>(p));
        sc = proxy;
    }
}

bool D3D12Device::upscalerSupported() const { return slActive && streamline::dlssAvailable(); }

bool D3D12Device::upscaleRenderSize(UpscaleMode mode, uint32_t outWidth, uint32_t outHeight, uint32_t& renderWidth,
                                    uint32_t& renderHeight) {
    renderWidth = outWidth;
    renderHeight = outHeight;
    return slActive && streamline::renderSize(mode, outWidth, outHeight, renderWidth, renderHeight);
}

bool D3D12Device::latencySupported() const { return slActive && streamline::reflexAvailable(); }

void D3D12Device::setLatencyMode(LatencyMode mode) {
    if (slActive) streamline::setLatencyMode(mode);
}

void D3D12Device::latencySleep() {
    if (slActive) streamline::latencySleep(frameNumber_);
}

void D3D12Device::latencyMarker(LatencyMarker marker) {
    if (slActive) streamline::latencyMarker(marker, frameNumber_);
}

void D3D12CommandList::restoreBindings() {
    ID3D12DescriptorHeap* heaps[] = {dev->bindless.heap.Get()};
    cl->SetDescriptorHeaps(1, heaps);
    ID3D12RootSignature* rs = dev->rootSig.Get();
    cl->SetGraphicsRootSignature(rs);
    cl->SetComputeRootSignature(rs);
    cl->SetGraphicsRoot32BitConstants(0, kPushConstantBytes / 4, push, 0);
    cl->SetComputeRoot32BitConstants(0, kPushConstantBytes / 4, push, 0);
    cl->SetGraphicsRootConstantBufferView(1, frameConstants);
    cl->SetComputeRootConstantBufferView(1, frameConstants);
    cl->SetGraphicsRootDescriptorTable(2, dev->bindless.gpu(0));
    cl->SetComputeRootDescriptorTable(2, dev->bindless.gpu(0));
    if (pipeline) {
        cl->SetPipelineState(pipeline->pso.Get());
        if (!pipeline->isCompute()) cl->IASetPrimitiveTopology(pipeline->topology);
    }
}

bool D3D12CommandList::upscale(const UpscaleInputs& in) {
    if (!dev->upscalerSupported() || in.mode == UpscaleMode::Off) return false;
    D3D12Texture* tex[4] = {static_cast<D3D12Texture*>(in.color), static_cast<D3D12Texture*>(in.depth),
                            static_cast<D3D12Texture*>(in.motion), static_cast<D3D12Texture*>(in.output)};
    streamline::NativeImage img[4];
    for (int i = 0; i < 4; ++i) {
        D3D12Texture* t = tex[i];
        if (!t || !t->res) return false;
        // The state each image is in right now (it stays there while DLSS runs), as our barriers track it.
        D3D12_RESOURCE_STATES s = toD3D(t->state, false);
        if (t->state == State::DepthRead && t->sampled) s |= D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        img[i].resource = t->res;
        img[i].state = uint32_t(s);
        img[i].width = t->width();
        img[i].height = t->height();
        img[i].nativeFormat = uint32_t(t->res->GetDesc().Format);
    }
    if (!(tex[3]->res->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return false;  // needs Storage
    const bool ok = streamline::evaluate(cl.Get(), img, in, dev->frameNumber());
    restoreBindings();
    return ok;
}

}  // namespace taelia::rhi::d3d12
