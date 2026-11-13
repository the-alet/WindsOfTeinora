// D3D12 swapchain: flip-discard, 3 buffers, tearing (never V-Sync).
#include <cstdio>

#include "d3d12_internal.h"

namespace taelia::rhi::d3d12 {

std::unique_ptr<Swapchain> D3D12Device::createSwapchain(void* hwnd, uint32_t width, uint32_t height) {
    auto sc = std::make_unique<D3D12Swapchain>(this);
    if (!sc->init(static_cast<HWND>(hwnd), width, height)) return nullptr;
    return sc;
}

bool D3D12Swapchain::init(HWND hwnd, uint32_t w, uint32_t h) {
    width_ = w ? w : 1;
    height_ = h ? h : 1;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width_;
    sd.Height = height_;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc = {1, 0};
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBuffers;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = dev->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = dev->factory->CreateSwapChainForHwnd(dev->queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
    // With Streamline the SL proxy replaces it: Present / ResizeBuffers / GetBuffer / GetCurrentBackBufferIndex
    // all go through the proxy.
    if (SUCCEEDED(hr) && dev->slActive) dev->upgradeSwapchain(sc1);
    if (FAILED(hr) || FAILED(sc1.As(&swapchain))) {
        std::fprintf(stderr, "d3d12: CreateSwapChainForHwnd failed (hr 0x%08X)\n", unsigned(hr));
        return false;
    }
    dev->factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    createBuffers();
    return true;
}

D3D12Swapchain::~D3D12Swapchain() {
    dev->waitIdle();
    releaseBuffers();
}

void D3D12Swapchain::createBuffers() {
    for (uint32_t i = 0; i < kBuffers; ++i) {
        TextureDesc td;
        td.width = width_;
        td.height = height_;
        td.format = Format::RGBA8_UNORM;
        td.usage = TextureUsage::RenderTarget | TextureUsage::CopySrc | TextureUsage::CopyDst;
        td.name = "rhi.backbuffer" + std::to_string(i);
        auto t = std::make_unique<D3D12Texture>(dev, td);
        swapchain->GetBuffer(i, IID_PPV_ARGS(&t->external));
        t->res = t->external.Get();
        t->state = State::Present;
        setName(t->res, td.name);
        dev->createTextureViews(t.get());
        buffers[i] = std::move(t);
    }
}

void D3D12Swapchain::releaseBuffers() {
    for (auto& b : buffers) b.reset();
    dev->collectGarbage(false);  // the GPU is idle: really drop the references before ResizeBuffers
}

Texture* D3D12Swapchain::backbuffer() { return buffers[swapchain->GetCurrentBackBufferIndex()].get(); }

void D3D12Swapchain::resize(uint32_t w, uint32_t h) {
    if (!w || !h || (w == width_ && h == height_)) return;
    dev->waitIdle();
    releaseBuffers();
    width_ = w;
    height_ = h;
    const HRESULT hr = swapchain->ResizeBuffers(kBuffers, w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                dev->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) std::fprintf(stderr, "d3d12: ResizeBuffers failed (hr 0x%08X)\n", unsigned(hr));
    createBuffers();
}

void D3D12Swapchain::present() {
    const HRESULT hr = swapchain->Present(0, dev->tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(hr)) std::fprintf(stderr, "d3d12: Present failed (hr 0x%08X)\n", unsigned(hr));
}

}  // namespace taelia::rhi::d3d12
