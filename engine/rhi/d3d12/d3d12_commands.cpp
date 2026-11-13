// D3D12 command recording with backend-tracked resource states.
#include <cstdio>
#include <cstring>

#include "d3d12_internal.h"

namespace taelia::rhi::d3d12 {
namespace {

D3D12_RESOURCE_STATES textureState(const D3D12Texture* t, State s) {
    D3D12_RESOURCE_STATES r = toD3D(s, false);
    if (s == State::DepthRead && t->sampled) r |= D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    return r;
}

void issue(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES before,
           D3D12_RESOURCE_STATES after, bool uav) {
    D3D12_RESOURCE_BARRIER b{};
    if (uav) {
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = res;
    } else {
        if (before == after) return;
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
    }
    cl->ResourceBarrier(1, &b);
}

}  // namespace

void transition(ID3D12GraphicsCommandList* cl, D3D12Texture* t, State s) {
    if (!t) return;
    if (t->state == s) {
        if (s == State::UnorderedAccess) issue(cl, t->res, {}, {}, true);
        return;
    }
    issue(cl, t->res, textureState(t, t->state), textureState(t, s), false);
    t->state = s;
}

void transition(ID3D12GraphicsCommandList* cl, D3D12Buffer* b, State s) {
    if (!b || !b->tracked()) return;
    if (b->state == s) {
        if (s == State::UnorderedAccess) issue(cl, b->res, {}, {}, true);
        return;
    }
    issue(cl, b->res, toD3D(b->state, true), toD3D(s, true), false);
    b->state = s;
}

void D3D12CommandList::barrier(Texture* t, State state) { transition(cl.Get(), static_cast<D3D12Texture*>(t), state); }
void D3D12CommandList::barrier(Buffer* b, State state) { transition(cl.Get(), static_cast<D3D12Buffer*>(b), state); }

void D3D12CommandList::beginRenderPass(const RenderPass& pass) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[kMaxColorTargets]{};
    const uint32_t count = pass.colorCount < kMaxColorTargets ? pass.colorCount : kMaxColorTargets;
    uint32_t w = 0, h = 0;
    for (uint32_t i = 0; i < count; ++i) {
        auto* t = static_cast<D3D12Texture*>(pass.color[i].texture);
        rtv[i] = dev->rtvs.cpu(t->rtv);
        if (i == 0) w = t->width(), h = t->height();
        if (pass.color[i].load == LoadOp::Clear) cl->ClearRenderTargetView(rtv[i], pass.color[i].clear, 0, nullptr);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    auto* depth = static_cast<D3D12Texture*>(pass.depth.texture);
    if (depth) {
        dsv = dev->dsvs.cpu(pass.depth.readOnly ? depth->dsvReadOnly : depth->dsv);
        if (count == 0) w = depth->width(), h = depth->height();
        if (pass.depth.load == LoadOp::Clear && !pass.depth.readOnly)
            cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, pass.depth.clear, 0, 0, nullptr);
    }
    cl->OMSetRenderTargets(count, count ? rtv : nullptr, FALSE, depth ? &dsv : nullptr);
    setViewport(0, 0, float(w), float(h));
    setScissor(0, 0, w, h);
}

void D3D12CommandList::endRenderPass() {}

void D3D12CommandList::setViewport(float x, float y, float w, float h) {
    const D3D12_VIEWPORT vp{x, y, w, h, 0.0f, 1.0f};
    cl->RSSetViewports(1, &vp);
}

void D3D12CommandList::setScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    const D3D12_RECT r{LONG(x), LONG(y), LONG(x + int32_t(w)), LONG(y + int32_t(h))};
    cl->RSSetScissorRects(1, &r);
}

void D3D12CommandList::setPipeline(Pipeline* p) {
    pipeline = static_cast<D3D12Pipeline*>(p);
    cl->SetPipelineState(pipeline->pso.Get());
    if (!pipeline->isCompute()) cl->IASetPrimitiveTopology(pipeline->topology);
}

void D3D12CommandList::pushConstants(const void* data, uint32_t size) {
    // Same root signature for both pipelines: setting both keeps the values across setPipeline.
    const UINT n = (size < kPushConstantBytes ? size : kPushConstantBytes) / 4;
    std::memcpy(push, data, size_t(n) * 4);
    cl->SetGraphicsRoot32BitConstants(0, n, data, 0);
    cl->SetComputeRoot32BitConstants(0, n, data, 0);
}

void D3D12CommandList::setFrameConstants(const Transient& t) {
    if (!t.buffer) return;
    const D3D12_GPU_VIRTUAL_ADDRESS va = static_cast<D3D12Buffer*>(t.buffer)->va + t.offset;
    frameConstants = va;
    cl->SetGraphicsRootConstantBufferView(1, va);
    cl->SetComputeRootConstantBufferView(1, va);
}

void D3D12CommandList::setIndexBuffer(Buffer* b, uint64_t offset, IndexType type) {
    auto* buf = static_cast<D3D12Buffer*>(b);
    D3D12_INDEX_BUFFER_VIEW v{};
    v.BufferLocation = buf->va + offset;
    v.SizeInBytes = UINT(buf->size() - offset);
    v.Format = type == IndexType::U16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    cl->IASetIndexBuffer(&v);
}

void D3D12CommandList::draw(uint32_t vertexCount, uint32_t instanceCount) {
    cl->DrawInstanced(vertexCount, instanceCount, 0, 0);
}

void D3D12CommandList::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex) {
    cl->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, 0, 0);
}

void D3D12CommandList::dispatch(uint32_t x, uint32_t y, uint32_t z) { cl->Dispatch(x, y, z); }

void D3D12CommandList::copyBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) {
    cl->CopyBufferRegion(static_cast<D3D12Buffer*>(dst)->res, dstOffset, static_cast<D3D12Buffer*>(src)->res,
                         srcOffset, size);
}

void D3D12CommandList::copyBufferToTexture(Texture* dst, uint32_t mip, uint32_t layer, Buffer* src,
                                           uint64_t srcOffset) {
    auto* t = static_cast<D3D12Texture*>(dst);
    auto* b = static_cast<D3D12Buffer*>(src);
    const TextureDesc& d = t->desc();
    const D3D12_RESOURCE_DESC rd = t->res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    const UINT sub = mip + layer * d.mips;
    dev->device->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, nullptr, nullptr);
    const uint64_t w = (d.width >> mip) ? (d.width >> mip) : 1;
    const uint64_t tight = w * formatBytesPerTexel(d.format);
    ID3D12Resource* from = b->res;
    if (tight == fp.Footprint.RowPitch && srcOffset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0) {
        fp.Offset = srcOffset;
    } else if (b->mapped()) {
        // Repack the tightly packed rows into the upload ring with the pitch D3D12 wants.
        const uint64_t slices = fp.Footprint.Depth;
        const uint64_t bytes = uint64_t(fp.Footprint.RowPitch) * rows * slices;
        const Transient tr = dev->allocTransient(bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!tr.buffer) return;
        const uint64_t pad = (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - tr.offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) %
                             D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        auto* out = static_cast<uint8_t*>(tr.cpu) + pad;
        const auto* in = static_cast<const uint8_t*>(b->mapped()) + srcOffset;
        for (uint64_t r = 0; r < rows * slices; ++r)
            std::memcpy(out + r * fp.Footprint.RowPitch, in + r * tight, size_t(tight));
        fp.Offset = tr.offset + pad;
        from = static_cast<D3D12Buffer*>(tr.buffer)->res;
    } else {
        std::fprintf(stderr,
                     "d3d12: copyBufferToTexture '%s': rows of %llu bytes need a 256-aligned pitch or a mapped source\n",
                     d.name.c_str(), (unsigned long long)tight);
        return;
    }
    D3D12_TEXTURE_COPY_LOCATION to{}, src2{};
    to.pResource = t->res;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = sub;
    src2.pResource = from;
    src2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src2.PlacedFootprint = fp;
    cl->CopyTextureRegion(&to, 0, 0, 0, &src2, nullptr);
}

void D3D12CommandList::copyTexture(Texture* dst, Texture* src) {
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = static_cast<D3D12Texture*>(dst)->res;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = static_cast<D3D12Texture*>(src)->res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
}

void D3D12CommandList::beginMarker(const char* name) {
    // PIX-compatible: metadata 1 = ANSI string including the terminator.
    cl->BeginEvent(1, name, UINT(std::strlen(name) + 1));
}

void D3D12CommandList::endMarker() { cl->EndEvent(); }

}  // namespace taelia::rhi::d3d12
