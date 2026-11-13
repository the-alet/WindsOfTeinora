// D3D12 textures, buffers, views, synchronous uploads and readback.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "d3d12_internal.h"

namespace taelia::rhi::d3d12 {

uint32_t D3D12Texture::arraySize() const {
    if (desc_.dim == TextureDim::Tex3D) return 1;
    if (desc_.dim == TextureDim::Cube) return std::max(desc_.depthOrLayers, 6u);
    return std::max(desc_.depthOrLayers, 1u);
}

D3D12Texture::~D3D12Texture() {
    D3D12Device* d = dev;
    D3D12MA::Allocation* a = alloc;
    ComPtr<ID3D12Resource> ext = external;
    const uint32_t s = srv_, r = rtv, ds = dsv, dsr = dsvReadOnly;
    std::vector<uint32_t> u = uavs_;
    d->defer([d, a, ext, s, r, ds, dsr, u]() {
        if (a) a->Release();
        d->bindless.free(s);
        for (uint32_t i : u) d->bindless.free(i);
        d->rtvs.free(r);
        d->dsvs.free(ds);
        d->dsvs.free(dsr);
    });
}

D3D12Buffer::~D3D12Buffer() {
    D3D12Device* d = dev;
    D3D12MA::Allocation* a = alloc;
    const uint32_t s = srv_, u = uav_;
    d->defer([d, a, s, u]() {
        if (a) a->Release();
        d->bindless.free(s);
        d->bindless.free(u);
    });
}

D3D12Pipeline::~D3D12Pipeline() {
    if (!dev) return;
    ComPtr<ID3D12PipelineState> p = pso;
    dev->defer([p]() {});
}

bool D3D12Device::createTextureViews(D3D12Texture* t) {
    const TextureDesc& d = t->desc();
    const bool depth = isDepthFormat(d.format);
    const uint32_t layers = t->arraySize();
    const bool array = d.dim == TextureDim::Tex2DArray || d.dim == TextureDim::Cube;
    uint32_t srv = 0;
    std::vector<uint32_t> uavs;
    if (d.usage & TextureUsage::Sampled) {
        srv = bindless.alloc();
        if (srv == kInvalid) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = depth ? DXGI_FORMAT_R32_FLOAT : toDxgi(d.format);
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        switch (d.dim) {
            case TextureDim::Tex2D:
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = d.mips;
                break;
            case TextureDim::Tex2DArray:
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                sd.Texture2DArray.MipLevels = d.mips;
                sd.Texture2DArray.ArraySize = layers;
                break;
            case TextureDim::Cube:
                if (layers > 6) {
                    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                    sd.TextureCubeArray.MipLevels = d.mips;
                    sd.TextureCubeArray.NumCubes = layers / 6;
                } else {
                    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                    sd.TextureCube.MipLevels = d.mips;
                }
                break;
            case TextureDim::Tex3D:
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                sd.Texture3D.MipLevels = d.mips;
                break;
        }
        device->CreateShaderResourceView(t->res, &sd, bindless.cpu(srv));
    }
    if (d.usage & TextureUsage::Storage) {
        for (uint32_t mip = 0; mip < d.mips; ++mip) {
            const uint32_t u = bindless.alloc();
            if (u == kInvalid) return false;
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = toDxgi(d.format);
            if (d.dim == TextureDim::Tex3D) {
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                ud.Texture3D.MipSlice = mip;
                ud.Texture3D.WSize = UINT(-1);
            } else if (array) {
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                ud.Texture2DArray.MipSlice = mip;
                ud.Texture2DArray.ArraySize = layers;
            } else {
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                ud.Texture2D.MipSlice = mip;
            }
            device->CreateUnorderedAccessView(t->res, nullptr, &ud, bindless.cpu(u));
            uavs.push_back(u);
        }
    }
    t->setViews(srv, std::move(uavs));
    if (d.usage & TextureUsage::RenderTarget) {
        t->rtv = rtvs.alloc();
        if (t->rtv == kInvalid) return false;
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = toDxgi(d.format);
        if (d.dim == TextureDim::Tex3D) {
            rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
            rd.Texture3D.WSize = UINT(-1);
        } else if (array) {
            rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rd.Texture2DArray.ArraySize = layers;
        } else {
            rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        }
        device->CreateRenderTargetView(t->res, &rd, rtvs.cpu(t->rtv));
    }
    if (d.usage & TextureUsage::DepthStencil) {
        t->dsv = dsvs.alloc();
        t->dsvReadOnly = dsvs.alloc();
        if (t->dsv == kInvalid || t->dsvReadOnly == kInvalid) return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        if (array) {
            dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dd.Texture2DArray.ArraySize = layers;
        } else {
            dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        }
        device->CreateDepthStencilView(t->res, &dd, dsvs.cpu(t->dsv));
        dd.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        device->CreateDepthStencilView(t->res, &dd, dsvs.cpu(t->dsvReadOnly));
    }
    return true;
}

std::unique_ptr<Texture> D3D12Device::createTexture(const TextureDesc& d) {
    auto t = std::make_unique<D3D12Texture>(this, d);
    const bool depth = isDepthFormat(d.format);
    t->sampled = (d.usage & TextureUsage::Sampled) != 0;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = d.dim == TextureDim::Tex3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = d.width;
    rd.Height = d.height;
    rd.DepthOrArraySize = UINT16(d.dim == TextureDim::Tex3D ? d.depthOrLayers : t->arraySize());
    rd.MipLevels = UINT16(d.mips);
    rd.Format = depth && t->sampled ? DXGI_FORMAT_R32_TYPELESS : toDxgi(d.format);
    rd.SampleDesc = {1, 0};
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_CLEAR_VALUE cv{};
    const D3D12_CLEAR_VALUE* cvp = nullptr;
    if (d.usage & TextureUsage::RenderTarget) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        cv.Format = toDxgi(d.format);
        std::memcpy(cv.Color, d.clearColor, sizeof cv.Color);
        cvp = &cv;
    }
    if (d.usage & TextureUsage::DepthStencil) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (!t->sampled) rd.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = d.clearDepth;
        cvp = &cv;
    }
    if (d.usage & TextureUsage::Storage) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12MA::ALLOCATION_DESC ad{};
    ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    const HRESULT hr =
        allocator->CreateResource(&ad, &rd, D3D12_RESOURCE_STATE_COMMON, cvp, &t->alloc, IID_NULL, nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "d3d12: createTexture '%s' failed (hr 0x%08X)\n", d.name.c_str(), unsigned(hr));
        return nullptr;
    }
    t->res = t->alloc->GetResource();
    setName(t->res, d.name);
    if (!createTextureViews(t.get())) return nullptr;
    return t;
}

std::unique_ptr<Buffer> D3D12Device::createBuffer(const BufferDesc& d, const void* initialData) {
    auto b = std::make_unique<D3D12Buffer>(this, d);
    const uint64_t size4 = (std::max<uint64_t>(d.size, 4) + 3) & ~uint64_t(3);
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (size4 + 255) & ~uint64_t(255);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc = {1, 0};
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const bool storage = (d.usage & BufferUsage::Storage) != 0;
    if (storage && d.memory == Memory::GpuOnly) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12MA::ALLOCATION_DESC ad{};
    D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON;
    switch (d.memory) {
        case Memory::GpuOnly: ad.HeapType = D3D12_HEAP_TYPE_DEFAULT; break;
        case Memory::Upload:
            ad.HeapType = D3D12_HEAP_TYPE_UPLOAD;
            initial = D3D12_RESOURCE_STATE_GENERIC_READ;
            break;
        case Memory::Readback:
            ad.HeapType = D3D12_HEAP_TYPE_READBACK;
            initial = D3D12_RESOURCE_STATE_COPY_DEST;
            break;
    }
    HRESULT hr = allocator->CreateResource(&ad, &rd, initial, nullptr, &b->alloc, IID_NULL, nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "d3d12: createBuffer '%s' failed (hr 0x%08X)\n", d.name.c_str(), unsigned(hr));
        return nullptr;
    }
    b->res = b->alloc->GetResource();
    b->va = b->res->GetGPUVirtualAddress();
    setName(b->res, d.name);
    void* mapped = nullptr;
    if (d.memory != Memory::GpuOnly) {
        const D3D12_RANGE none{0, 0};
        hr = b->res->Map(0, d.memory == Memory::Upload ? &none : nullptr, &mapped);
        if (FAILED(hr)) return nullptr;
    }
    uint32_t srv = 0, uav = 0;
    if (storage) {
        srv = bindless.alloc();
        if (srv == kInvalid) return nullptr;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = UINT(size4 / 4);
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(b->res, &sd, bindless.cpu(srv));
        if (d.memory == Memory::GpuOnly) {
            uav = bindless.alloc();
            if (uav == kInvalid) return nullptr;
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = UINT(size4 / 4);
            ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            device->CreateUnorderedAccessView(b->res, nullptr, &ud, bindless.cpu(uav));
        }
    }
    b->setViews(srv, uav, mapped);

    if (initialData && d.size) {
        if (mapped) {
            std::memcpy(mapped, initialData, size_t(d.size));
        } else {
            auto staging = createBuffer({d.size, BufferUsage::CopySrc, Memory::Upload, "rhi.staging"}, initialData);
            if (!staging) return nullptr;
            ID3D12GraphicsCommandList* cl = beginImmediate();
            transition(cl, b.get(), State::CopyDst);
            cl->CopyBufferRegion(b->res, 0, static_cast<D3D12Buffer*>(staging.get())->res, 0, d.size);
            State after = State::ShaderRead;
            if (!storage && (d.usage & BufferUsage::Index)) after = State::IndexBuffer;
            if (!storage && (d.usage & BufferUsage::Indirect)) after = State::IndirectArgument;
            transition(cl, b.get(), after);
            endImmediate();
        }
    }
    return b;
}

void D3D12Device::uploadTexture(Texture* tex, const void* data, size_t size) {
    auto* t = static_cast<D3D12Texture*>(tex);
    const TextureDesc& d = t->desc();
    const uint32_t layers = t->arraySize();
    const D3D12_RESOURCE_DESC rd = t->res->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps(layers);
    std::vector<UINT> rows(layers);
    uint64_t total = 0;
    for (uint32_t l = 0; l < layers; ++l) {
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&rd, l * d.mips, 1, total, &fps[l], &rows[l], nullptr, &bytes);
        total = (fps[l].Offset + bytes + 511) & ~uint64_t(511);
    }
    const uint64_t rowBytes = uint64_t(d.width) * formatBytesPerTexel(d.format);
    const uint32_t slices = d.dim == TextureDim::Tex3D ? d.depthOrLayers : 1;
    const uint64_t need = rowBytes * d.height * slices * layers;
    if (size < need) {
        std::fprintf(stderr, "d3d12: uploadTexture '%s': %zu bytes given, %llu needed\n", d.name.c_str(), size,
                     (unsigned long long)need);
        return;
    }
    auto staging = createBuffer({total, BufferUsage::CopySrc, Memory::Upload, "rhi.staging"}, nullptr);
    if (!staging) return;
    auto* dst = static_cast<uint8_t*>(staging->mapped());
    const auto* src = static_cast<const uint8_t*>(data);
    for (uint32_t l = 0; l < layers; ++l)
        for (uint32_t z = 0; z < slices; ++z)
            for (uint32_t y = 0; y < rows[l]; ++y)
                std::memcpy(dst + fps[l].Offset + (uint64_t(z) * rows[l] + y) * fps[l].Footprint.RowPitch,
                            src + ((uint64_t(l) * slices + z) * d.height + y) * rowBytes, size_t(rowBytes));

    ID3D12GraphicsCommandList* cl = beginImmediate();
    transition(cl, t, State::CopyDst);
    for (uint32_t l = 0; l < layers; ++l) {
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = t->res;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = l * d.mips;
        from.pResource = static_cast<D3D12Buffer*>(staging.get())->res;
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = fps[l];
        cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    transition(cl, t, State::ShaderRead);
    endImmediate();
}

bool D3D12Device::readbackRGBA8(Texture* tex, std::vector<uint8_t>& out) {
    auto* t = static_cast<D3D12Texture*>(tex);
    const Format f = t->desc().format;
    if (inFrame_ || (f != Format::RGBA8_UNORM && f != Format::RGBA8_SRGB && f != Format::BGRA8_UNORM)) return false;
    const D3D12_RESOURCE_DESC rd = t->res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 total = 0;
    device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, nullptr, &total);
    auto rb = createBuffer({total, BufferUsage::CopyDst, Memory::Readback, "rhi.readback"}, nullptr);
    if (!rb) return false;

    const State before = t->state;
    ID3D12GraphicsCommandList* cl = beginImmediate();
    transition(cl, t, State::CopySrc);
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    from.pResource = t->res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0;
    to.pResource = static_cast<D3D12Buffer*>(rb.get())->res;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = fp;
    cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(cl, t, before);
    endImmediate();

    const uint32_t w = t->width(), h = t->height();
    out.resize(size_t(w) * h * 4);
    const auto* src = static_cast<const uint8_t*>(rb->mapped());
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = &out[size_t(y) * w * 4];
        std::memcpy(row, src + fp.Offset + uint64_t(y) * fp.Footprint.RowPitch, size_t(w) * 4);
        if (f == Format::BGRA8_UNORM)
            for (uint32_t x = 0; x < w; ++x) std::swap(row[x * 4 + 0], row[x * 4 + 2]);
    }
    return true;
}

}  // namespace taelia::rhi::d3d12
