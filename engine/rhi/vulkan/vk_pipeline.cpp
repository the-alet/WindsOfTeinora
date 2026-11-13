// Vulkan graphics and compute pipelines (one shared layout, dynamic rendering).
#include "vk_internal.h"

namespace taelia::rhi::vk {

VkPipe::~VkPipe() {
    VulkanDevice* dev = dev_;
    VkPipeline p = pipeline;
    dev->retire([dev, p] {
        if (p) vkDestroyPipeline(dev->device, p, nullptr);
    });
}

VkShaderModule VulkanDevice::loadShader(const std::string& name, std::string* error) {
    std::vector<uint8_t> code;
    if (!loadShaderBinary(shaderDir_, name, ".spv", code, error)) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &m) != VK_SUCCESS) {
        if (error) *error = "vkCreateShaderModule failed for " + name;
        return VK_NULL_HANDLE;
    }
    return m;
}

namespace {

std::string entryOf(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? std::string("main") : name.substr(dot + 1);
}

VkCompareOp toVk(Compare c) {
    switch (c) {
        case Compare::Never: return VK_COMPARE_OP_NEVER;
        case Compare::Less: return VK_COMPARE_OP_LESS;
        case Compare::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case Compare::Equal: return VK_COMPARE_OP_EQUAL;
        case Compare::Greater: return VK_COMPARE_OP_GREATER;
        case Compare::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case Compare::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkPipelineColorBlendAttachmentState blendOf(Blend b) {
    VkPipelineColorBlendAttachmentState s{};
    s.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                       VK_COLOR_COMPONENT_A_BIT;
    if (b == Blend::Opaque) return s;
    s.blendEnable = VK_TRUE;
    s.colorBlendOp = s.alphaBlendOp = VK_BLEND_OP_ADD;
    switch (b) {
        case Blend::Alpha:
            s.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case Blend::Premultiplied:
            s.srcColorBlendFactor = s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            s.dstColorBlendFactor = s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        default:  // Additive
            s.srcColorBlendFactor = s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            s.dstColorBlendFactor = s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
    }
    return s;
}

}  // namespace

std::unique_ptr<Pipeline> VulkanDevice::createGraphicsPipeline(const GraphicsPipelineDesc& d, std::string* error) {
    VkShaderModule vs = loadShader(d.vs, error);
    VkShaderModule ps = d.ps.empty() ? VK_NULL_HANDLE : loadShader(d.ps, error);
    if (!vs || (!d.ps.empty() && !ps)) {
        if (vs) vkDestroyShaderModule(device, vs, nullptr);
        if (ps) vkDestroyShaderModule(device, ps, nullptr);
        return nullptr;
    }
    const std::string vsEntry = entryOf(d.vs), psEntry = entryOf(d.ps);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = vsEntry.c_str();
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = psEntry.c_str();

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    switch (d.topology) {
        case Topology::TriangleList: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        case Topology::TriangleStrip: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
        case Topology::LineList: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case Topology::PointList: ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    }
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = d.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = d.cull == Cull::Back ? VK_CULL_MODE_BACK_BIT : d.cull == Cull::Front ? VK_CULL_MODE_FRONT_BIT
                                                                                       : VK_CULL_MODE_NONE;
    // The negative-height viewport keeps D3D's screen-space winding, so the flag maps directly.
    rs.frontFace = d.frontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = (d.depthBias != 0 || d.slopeScaledDepthBias != 0) ? VK_TRUE : VK_FALSE;
    rs.depthBiasConstantFactor = d.depthBias;
    rs.depthBiasSlopeFactor = d.slopeScaledDepthBias;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depthTest ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = d.depthWrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = d.depthTest ? toVk(d.depthCompare) : VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendAttachmentState blends[kMaxColorTargets]{};
    VkFormat colorFormats[kMaxColorTargets]{};
    for (uint32_t i = 0; i < d.colorCount; ++i) {
        blends[i] = blendOf(d.blend[i]);
        colorFormats[i] = rhi::vk::toVk(d.colorFormats[i]);
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = d.colorCount;
    cb.pAttachments = blends;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = d.colorCount;
    ri.pColorAttachmentFormats = colorFormats;
    ri.depthAttachmentFormat = rhi::vk::toVk(d.depthFormat);

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
    ci.stageCount = ps ? 2 : 1;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = pipelineLayout;
    auto p = std::make_unique<VkPipe>(this, false);
    const VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p->pipeline);
    vkDestroyShaderModule(device, vs, nullptr);
    if (ps) vkDestroyShaderModule(device, ps, nullptr);
    if (r != VK_SUCCESS) {
        if (error) *error = "vkCreateGraphicsPipelines failed for " + d.name + " (" + std::to_string(int(r)) + ")";
        p->pipeline = VK_NULL_HANDLE;
        return nullptr;
    }
    setName(VK_OBJECT_TYPE_PIPELINE, uint64_t(p->pipeline), d.name);
    return p;
}

std::unique_ptr<Pipeline> VulkanDevice::createComputePipeline(const ComputePipelineDesc& d, std::string* error) {
    VkShaderModule cs = loadShader(d.cs, error);
    if (!cs) return nullptr;
    const std::string entry = entryOf(d.cs);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = cs;
    ci.stage.pName = entry.c_str();
    ci.layout = pipelineLayout;
    auto p = std::make_unique<VkPipe>(this, true);
    const VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p->pipeline);
    vkDestroyShaderModule(device, cs, nullptr);
    if (r != VK_SUCCESS) {
        if (error) *error = "vkCreateComputePipelines failed for " + d.name + " (" + std::to_string(int(r)) + ")";
        p->pipeline = VK_NULL_HANDLE;
        return nullptr;
    }
    setName(VK_OBJECT_TYPE_PIPELINE, uint64_t(p->pipeline), d.name);
    return p;
}

}  // namespace taelia::rhi::vk
