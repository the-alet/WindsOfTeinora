// D3D12 inline ray tracing (DXR 1.1 acceleration structures for RayQuery) and indirect draws / dispatches.
#include <cstdio>
#include <cstring>

#include "d3d12_internal.h"

namespace taelia::rhi::d3d12 {
namespace {

constexpr uint64_t alignAs(uint64_t v) {
    constexpr uint64_t a = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
    return (v + a - 1) & ~(a - 1);
}

std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs(const BlasDesc& desc) {
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> out(desc.geometries.size());
    for (size_t i = 0; i < desc.geometries.size(); ++i) {
        const BlasGeometry& g = desc.geometries[i];
        D3D12_RAYTRACING_GEOMETRY_DESC& d = out[i];
        d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        d.Flags = g.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        auto& t = d.Triangles;
        t.Transform3x4 = 0;
        t.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        t.VertexCount = g.vertexCount;
        if (g.vertices) t.VertexBuffer.StartAddress = static_cast<D3D12Buffer*>(g.vertices)->va + g.vertexOffset;
        t.VertexBuffer.StrideInBytes = g.vertexStride;
        if (g.indices && g.indexCount) {
            t.IndexFormat = g.indexType == IndexType::U16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
            t.IndexCount = g.indexCount;
            t.IndexBuffer = static_cast<D3D12Buffer*>(g.indices)->va + g.indexOffset;
        } else {
            t.IndexFormat = DXGI_FORMAT_UNKNOWN;
        }
    }
    return out;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs(
    const std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>& geoms, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = flags;
    in.NumDescs = UINT(geoms.size());
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.pGeometryDescs = geoms.data();
    return in;
}

D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs(uint32_t count,
                                                                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags = flags;
    in.NumDescs = count;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    return in;
}

// Allocates the result and scratch buffers of `as` from the prebuild info of `in`.
bool allocate(D3D12Device* dev, ID3D12Device5* d5, D3D12AccelerationStructure* as,
              const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in, const std::string& name) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    d5->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    if (info.ResultDataMaxSizeInBytes == 0) {
        std::fprintf(stderr, "d3d12: acceleration structure '%s': empty prebuild info\n", name.c_str());
        return false;
    }
    auto make = [&](uint64_t size, D3D12_RESOURCE_STATES state, D3D12MA::Allocation** out) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = alignAs(size ? size : 1);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12MA::ALLOCATION_DESC ad{};
        ad.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        return SUCCEEDED(dev->allocator->CreateResource(&ad, &rd, state, nullptr, out, IID_NULL, nullptr));
    };
    if (!make(info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &as->alloc) ||
        !make(info.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &as->scratch)) {
        std::fprintf(stderr, "d3d12: acceleration structure '%s': allocation failed\n", name.c_str());
        return false;
    }
    as->va = as->alloc->GetResource()->GetGPUVirtualAddress();
    as->scratchVa = as->scratch->GetResource()->GetGPUVirtualAddress();
    setName(as->alloc->GetResource(), name);
    setName(as->scratch->GetResource(), name + ".scratch");
    return true;
}

void uavBarrier(ID3D12GraphicsCommandList* cl, D3D12MA::Allocation* a, D3D12MA::Allocation* b = nullptr) {
    D3D12_RESOURCE_BARRIER bs[2]{};
    UINT n = 0;
    for (D3D12MA::Allocation* x : {a, b}) {
        if (!x) continue;
        bs[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        bs[n].UAV.pResource = x->GetResource();
        ++n;
    }
    if (n) cl->ResourceBarrier(n, bs);
}

}  // namespace

D3D12AccelerationStructure::~D3D12AccelerationStructure() {
    D3D12Device* d = dev;
    D3D12MA::Allocation* a = alloc;
    D3D12MA::Allocation* s = scratch;
    const uint32_t view = srv_;
    d->defer([d, a, s, view]() {
        if (a) a->Release();
        if (s) s->Release();
        d->bindless.free(view);
    });
}

bool D3D12Device::rayTracingSupported() const {
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5))) return false;
    ComPtr<ID3D12Device5> d5;
    return o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 && SUCCEEDED(device.As(&d5));
}

std::unique_ptr<AccelerationStructure> D3D12Device::createBlas(const BlasDesc& desc) {
    ComPtr<ID3D12Device5> d5;
    if (!rayTracingSupported() || FAILED(device.As(&d5))) return nullptr;
    auto as = std::make_unique<D3D12AccelerationStructure>(this, false);
    as->flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    const auto geoms = geometryDescs(desc);
    if (!allocate(this, d5.Get(), as.get(), blasInputs(geoms, as->flags), desc.name)) return nullptr;
    return as;
}

std::unique_ptr<AccelerationStructure> D3D12Device::createTlas(uint32_t maxInstances, const std::string& name) {
    ComPtr<ID3D12Device5> d5;
    if (!rayTracingSupported() || FAILED(device.As(&d5))) return nullptr;
    auto as = std::make_unique<D3D12AccelerationStructure>(this, true);
    as->flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    as->maxInstances = maxInstances ? maxInstances : 1;
    if (!allocate(this, d5.Get(), as.get(), tlasInputs(as->maxInstances, as->flags), name)) return nullptr;
    const uint32_t srv = bindless.alloc();
    if (srv == kInvalid) return nullptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.RaytracingAccelerationStructure.Location = as->va;
    device->CreateShaderResourceView(nullptr, &sd, bindless.cpu(srv));
    as->setSrv(srv);
    return as;
}

ID3D12CommandSignature* D3D12Device::commandSignature(IndirectKind kind) {
    ComPtr<ID3D12CommandSignature>& sig = cmdSigs[int(kind)];
    if (sig) return sig.Get();
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    D3D12_COMMAND_SIGNATURE_DESC sd{};
    switch (kind) {
        case IndirectKind::Draw:
            arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
            sd.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
            break;
        case IndirectKind::DrawIndexed:
            arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
            sd.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
            break;
        case IndirectKind::Dispatch:
            arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
            sd.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
            break;
    }
    sd.NumArgumentDescs = 1;
    sd.pArgumentDescs = &arg;
    if (FAILED(device->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&sig)))) {
        std::fprintf(stderr, "d3d12: CreateCommandSignature failed\n");
        return nullptr;
    }
    return sig.Get();
}

static_assert(sizeof(D3D12_DRAW_ARGUMENTS) == 16 && sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) == 20 &&
              sizeof(D3D12_DISPATCH_ARGUMENTS) == 12);

void D3D12CommandList::drawIndirect(Buffer* args, uint64_t offset, uint32_t drawCount) {
    if (ID3D12CommandSignature* s = dev->commandSignature(D3D12Device::IndirectKind::Draw))
        cl->ExecuteIndirect(s, drawCount, static_cast<D3D12Buffer*>(args)->res, offset, nullptr, 0);
}

void D3D12CommandList::drawIndexedIndirect(Buffer* args, uint64_t offset, uint32_t drawCount) {
    if (ID3D12CommandSignature* s = dev->commandSignature(D3D12Device::IndirectKind::DrawIndexed))
        cl->ExecuteIndirect(s, drawCount, static_cast<D3D12Buffer*>(args)->res, offset, nullptr, 0);
}

void D3D12CommandList::dispatchIndirect(Buffer* args, uint64_t offset) {
    if (ID3D12CommandSignature* s = dev->commandSignature(D3D12Device::IndirectKind::Dispatch))
        cl->ExecuteIndirect(s, 1, static_cast<D3D12Buffer*>(args)->res, offset, nullptr, 0);
}

void D3D12CommandList::buildBlas(AccelerationStructure* blas, const BlasDesc& desc) {
    auto* as = static_cast<D3D12AccelerationStructure*>(blas);
    if (!as || (!cl4 && FAILED(cl.As(&cl4)))) return;
    const auto geoms = geometryDescs(desc);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.DestAccelerationStructureData = as->va;
    bd.Inputs = blasInputs(geoms, as->flags);
    bd.ScratchAccelerationStructureData = as->scratchVa;
    uavBarrier(cl.Get(), as->alloc, as->scratch);  // earlier build / reads of this structure in this list
    cl4->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    uavBarrier(cl.Get(), as->alloc);  // visible to later TLAS builds
}

void D3D12CommandList::buildTlas(AccelerationStructure* tlas, const TlasInstance* instances, uint32_t count) {
    auto* as = static_cast<D3D12AccelerationStructure*>(tlas);
    if (!as || (!cl4 && FAILED(cl.As(&cl4)))) return;
    if (count > as->maxInstances) {
        std::fprintf(stderr, "d3d12: buildTlas: %u instances, room for %u\n", count, as->maxInstances);
        count = as->maxInstances;
    }
    D3D12_GPU_VIRTUAL_ADDRESS descs = 0;
    if (count) {
        const Transient t = dev->allocTransient(uint64_t(count) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
        if (!t.buffer) return;
        auto* out = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(t.cpu);
        for (uint32_t i = 0; i < count; ++i) {
            const TlasInstance& in = instances[i];
            D3D12_RAYTRACING_INSTANCE_DESC d{};
            std::memcpy(d.Transform, in.transform, sizeof d.Transform);
            d.InstanceID = in.customIndex & 0xFFFFFFu;
            d.InstanceMask = in.mask;
            d.InstanceContributionToHitGroupIndex = 0;
            UINT flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            if (in.doubleSided) flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            if (in.forceOpaque) flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
            d.Flags = flags;
            d.AccelerationStructure = in.blas ? static_cast<D3D12AccelerationStructure*>(in.blas)->va : 0;
            out[i] = d;
        }
        descs = static_cast<D3D12Buffer*>(t.buffer)->va + t.offset;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.DestAccelerationStructureData = as->va;
    bd.Inputs = tlasInputs(count, as->flags);
    bd.Inputs.InstanceDescs = descs;
    bd.ScratchAccelerationStructureData = as->scratchVa;
    uavBarrier(cl.Get(), as->alloc, as->scratch);  // earlier build / ray queries on this structure in this list
    cl4->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    uavBarrier(cl.Get(), as->alloc);  // visible to later draws / dispatches
}

}  // namespace taelia::rhi::d3d12
