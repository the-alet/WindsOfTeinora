// D3D12 pipeline state objects (one global root signature).
#include "d3d12_internal.h"

namespace taelia::rhi::d3d12 {
namespace {

D3D12_COMPARISON_FUNC toD3D(Compare c) {
    switch (c) {
        case Compare::Never: return D3D12_COMPARISON_FUNC_NEVER;
        case Compare::Less: return D3D12_COMPARISON_FUNC_LESS;
        case Compare::LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case Compare::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
        case Compare::Greater: return D3D12_COMPARISON_FUNC_GREATER;
        case Compare::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case Compare::Always: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

D3D12_RENDER_TARGET_BLEND_DESC toD3D(Blend b) {
    D3D12_RENDER_TARGET_BLEND_DESC d{};
    d.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.LogicOp = D3D12_LOGIC_OP_NOOP;
    d.BlendOp = d.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    d.SrcBlend = d.SrcBlendAlpha = D3D12_BLEND_ONE;
    d.DestBlend = d.DestBlendAlpha = D3D12_BLEND_ZERO;
    switch (b) {
        case Blend::Opaque: break;
        case Blend::Alpha:
            d.BlendEnable = TRUE;
            d.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            d.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            d.SrcBlendAlpha = D3D12_BLEND_ONE;
            d.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case Blend::Premultiplied:
            d.BlendEnable = TRUE;
            d.DestBlend = d.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case Blend::Additive:
            d.BlendEnable = TRUE;
            d.DestBlend = d.DestBlendAlpha = D3D12_BLEND_ONE;
            break;
    }
    return d;
}

}  // namespace

std::unique_ptr<Pipeline> D3D12Device::createGraphicsPipeline(const GraphicsPipelineDesc& d, std::string* error) {
    std::vector<uint8_t> vs, ps;
    if (!loadShaderBinary(shaderDir_, d.vs, ".dxil", vs, error)) return nullptr;
    if (!d.ps.empty() && !loadShaderBinary(shaderDir_, d.ps, ".dxil", ps, error)) return nullptr;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rootSig.Get();
    pd.VS = {vs.data(), vs.size()};
    if (!ps.empty()) pd.PS = {ps.data(), ps.size()};
    for (uint32_t i = 0; i < kMaxColorTargets; ++i) pd.BlendState.RenderTarget[i] = toD3D(d.blend[i]);
    pd.BlendState.IndependentBlendEnable = TRUE;
    pd.SampleMask = UINT_MAX;
    D3D12_RASTERIZER_DESC& r = pd.RasterizerState;
    r.FillMode = d.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    r.CullMode = d.cull == Cull::Back ? D3D12_CULL_MODE_BACK : d.cull == Cull::Front ? D3D12_CULL_MODE_FRONT
                                                                                      : D3D12_CULL_MODE_NONE;
    r.FrontCounterClockwise = d.frontCounterClockwise;
    r.DepthBias = INT(d.depthBias);
    r.SlopeScaledDepthBias = d.slopeScaledDepthBias;
    r.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC& ds = pd.DepthStencilState;
    ds.DepthEnable = d.depthTest || d.depthWrite;
    ds.DepthWriteMask = d.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = d.depthTest ? toD3D(d.depthCompare) : D3D12_COMPARISON_FUNC_ALWAYS;
    pd.InputLayout = {nullptr, 0};
    pd.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    auto pipeline = std::make_unique<D3D12Pipeline>(false);
    switch (d.topology) {
        case Topology::TriangleList:
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pipeline->topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
        case Topology::TriangleStrip:
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pipeline->topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            break;
        case Topology::LineList:
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            pipeline->topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            break;
        case Topology::PointList:
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            pipeline->topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            break;
    }
    pd.NumRenderTargets = d.colorCount;
    for (uint32_t i = 0; i < d.colorCount && i < kMaxColorTargets; ++i) pd.RTVFormats[i] = toDxgi(d.colorFormats[i]);
    pd.DSVFormat = d.depthFormat == Format::Unknown ? DXGI_FORMAT_UNKNOWN : toDxgi(d.depthFormat);
    pd.SampleDesc = {1, 0};
    const HRESULT hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline->pso));
    if (FAILED(hr)) {
        if (error) *error = "CreateGraphicsPipelineState failed for " + (d.name.empty() ? d.vs : d.name);
        return nullptr;
    }
    setName(pipeline->pso.Get(), d.name);
    pipeline->dev = this;
    return pipeline;
}

std::unique_ptr<Pipeline> D3D12Device::createComputePipeline(const ComputePipelineDesc& d, std::string* error) {
    std::vector<uint8_t> cs;
    if (!loadShaderBinary(shaderDir_, d.cs, ".dxil", cs, error)) return nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rootSig.Get();
    pd.CS = {cs.data(), cs.size()};
    auto pipeline = std::make_unique<D3D12Pipeline>(true);
    const HRESULT hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline->pso));
    if (FAILED(hr)) {
        if (error) *error = "CreateComputePipelineState failed for " + (d.name.empty() ? d.cs : d.name);
        return nullptr;
    }
    setName(pipeline->pso.Get(), d.name);
    pipeline->dev = this;
    return pipeline;
}

}  // namespace taelia::rhi::d3d12
