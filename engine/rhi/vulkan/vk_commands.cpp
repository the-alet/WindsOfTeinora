// Vulkan command recording and state-tracked barriers (synchronization2).
#include <cstring>

#include "vk_internal.h"

namespace taelia::rhi::vk {

VkPipelineStageFlags2 g_extraReadStages = 0;

namespace {

struct Sync {
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    VkImageLayout layout;
};

constexpr VkPipelineStageFlags2 kShaderStages =
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

Sync syncOf(State s, bool depth) {
    switch (s) {
        case State::Undefined: return {VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED};
        case State::ShaderRead:
            return {kShaderStages | g_extraReadStages, VK_ACCESS_2_SHADER_READ_BIT,
                    depth ?VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        case State::UnorderedAccess:
            return {kShaderStages, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL};
        case State::RenderTarget:
            return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        case State::DepthWrite:
            return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL};
        case State::DepthRead:
            return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                        kShaderStages,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        case State::CopySrc:
            return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        case State::CopyDst:
            return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
        case State::Present:
            // Pairs with the acquire semaphore wait (ALL_COMMANDS) and the present (no access).
            return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
        case State::IndexBuffer:
            return {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
        case State::IndirectArgument:
            return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED};
    }
    return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_GENERAL};
}

}  // namespace

void textureBarrier(VulkanDevice*, VkCommandBuffer cb, VkTex* t, State to) {
    if (!t) return;
    if (t->state == to && to != State::UnorderedAccess) return;
    const bool depth = t->aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
    const Sync src = syncOf(t->state, depth), dst = syncOf(to, depth);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src.stage;
    b.srcAccessMask = src.access & ~VkAccessFlags2(VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                                                    VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    b.dstStageMask = to == State::Present ? VK_PIPELINE_STAGE_2_NONE : dst.stage;
    b.dstAccessMask = to == State::Present ? 0 : dst.access;
    b.oldLayout = src.layout;
    b.newLayout = dst.layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange = {t->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &dep);
    t->state = to;
}

void bufferBarrier(VkCommandBuffer cb, VkBuf* buf, State to) {
    if (!buf) return;
    if (buf->state == to && to != State::UnorderedAccess) return;
    const Sync src = syncOf(buf->state, false), dst = syncOf(to, false);
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = src.stage == VK_PIPELINE_STAGE_2_NONE ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : src.stage;
    b.srcAccessMask = src.access & ~VkAccessFlags2(VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                                                    VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    b.dstStageMask = dst.stage;
    b.dstAccessMask = dst.access;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buf->buffer;
    b.offset = 0;
    b.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &dep);
    buf->state = to;
}

void VkCmd::begin(VkCommandBuffer c) {
    cb = c;
    pipeline_ = nullptr;
    frameConstOffset_ = 0;
    pushBytes_ = 0;
    const VkDescriptorSet sets[2] = {dev_->set0, dev_->set1};
    const uint32_t zero = 0;
    for (VkPipelineBindPoint bp : {VK_PIPELINE_BIND_POINT_GRAPHICS, VK_PIPELINE_BIND_POINT_COMPUTE})
        vkCmdBindDescriptorSets(cb, bp, dev_->pipelineLayout, 0, 2, sets, 1, &zero);
}

void VkCmd::barrier(Texture* t, State state) { textureBarrier(dev_, cb, static_cast<VkTex*>(t), state); }
void VkCmd::barrier(Buffer* b, State state) {
    auto* vb = static_cast<VkBuf*>(b);
    if (vb && vb->desc().memory != Memory::GpuOnly) return;  // host-visible ring / staging: nothing to do
    bufferBarrier(cb, vb, state);
}

void VkCmd::beginRenderPass(const RenderPass& pass) {
    VkRenderingAttachmentInfo colors[kMaxColorTargets]{};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    auto loadOp = [](LoadOp l) {
        return l == LoadOp::Clear  ? VK_ATTACHMENT_LOAD_OP_CLEAR
               : l == LoadOp::Load ? VK_ATTACHMENT_LOAD_OP_LOAD
                                   : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    };
    uint32_t w = 0, h = 0;
    for (uint32_t i = 0; i < pass.colorCount; ++i) {
        auto* t = static_cast<VkTex*>(pass.color[i].texture);
        VkRenderingAttachmentInfo& a = colors[i];
        a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        a.imageView = t->rtvView;
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = loadOp(pass.color[i].load);
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        for (int c = 0; c < 4; ++c) a.clearValue.color.float32[c] = pass.color[i].clear[c];
        if (!w) w = t->width(), h = t->height();
    }
    auto* dt = static_cast<VkTex*>(pass.depth.texture);
    if (dt) {
        depth.imageView = dt->rtvView;
        depth.imageLayout =
            pass.depth.readOnly ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = loadOp(pass.depth.load);
        depth.storeOp = pass.depth.readOnly ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {pass.depth.clear, 0};
        if (!w) w = dt->width(), h = dt->height();
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = pass.colorCount;
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = dt ? &depth : nullptr;
    vkCmdBeginRendering(cb, &ri);
    setViewport(0, 0, float(w), float(h));
    setScissor(0, 0, w, h);
}

void VkCmd::endRenderPass() { vkCmdEndRendering(cb); }

void VkCmd::setViewport(float x, float y, float w, float h) {
    const VkViewport vp{x, y + h, w, -h, 0.0f, 1.0f};  // negative height: y down like D3D
    vkCmdSetViewport(cb, 0, 1, &vp);
}

void VkCmd::setScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    const VkRect2D r{{x, y}, {w, h}};
    vkCmdSetScissor(cb, 0, 1, &r);
}

void VkCmd::setPipeline(Pipeline* p) {
    auto* vp = static_cast<VkPipe*>(p);
    pipeline_ = vp;
    vkCmdBindPipeline(cb, vp->isCompute() ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                      vp->pipeline);
}

void VkCmd::pushConstants(const void* data, uint32_t size) {
    vkCmdPushConstants(cb, dev_->pipelineLayout, VK_SHADER_STAGE_ALL, 0, size, data);
    if (size <= kPushConstantBytes) {
        std::memcpy(push_, data, size);
        if (size > pushBytes_) pushBytes_ = size;
    }
}

void VkCmd::setFrameConstants(const Transient& t) {
    const uint32_t offset = uint32_t(t.offset);
    frameConstOffset_ = offset;
    for (VkPipelineBindPoint bp : {VK_PIPELINE_BIND_POINT_GRAPHICS, VK_PIPELINE_BIND_POINT_COMPUTE})
        vkCmdBindDescriptorSets(cb, bp, dev_->pipelineLayout, 1, 1, &dev_->set1, 1, &offset);
}

void VkCmd::setIndexBuffer(Buffer* b, uint64_t offset, IndexType type) {
    vkCmdBindIndexBuffer(cb, static_cast<VkBuf*>(b)->buffer, offset,
                         type == IndexType::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void VkCmd::draw(uint32_t vertexCount, uint32_t instanceCount) { vkCmdDraw(cb, vertexCount, instanceCount, 0, 0); }

void VkCmd::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex) {
    vkCmdDrawIndexed(cb, indexCount, instanceCount, firstIndex, 0, 0);
}

void VkCmd::dispatch(uint32_t x, uint32_t y, uint32_t z) { vkCmdDispatch(cb, x, y, z); }

void VkCmd::copyBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) {
    const VkBufferCopy r{srcOffset, dstOffset, size};
    vkCmdCopyBuffer(cb, static_cast<VkBuf*>(src)->buffer, static_cast<VkBuf*>(dst)->buffer, 1, &r);
}

void VkCmd::copyBufferToTexture(Texture* dst, uint32_t mip, uint32_t layer, Buffer* src, uint64_t srcOffset) {
    auto* t = static_cast<VkTex*>(dst);
    const auto& d = t->desc();
    VkBufferImageCopy r{};
    r.bufferOffset = srcOffset;
    r.imageSubresource = {t->aspect, mip, d.dim == TextureDim::Tex3D ? 0 : layer, 1};
    const uint32_t w = d.width >> mip, h = d.height >> mip, z = d.depthOrLayers >> mip;
    r.imageExtent = {w ? w : 1, h ? h : 1, d.dim == TextureDim::Tex3D ? (z ? z : 1) : 1};
    vkCmdCopyBufferToImage(cb, static_cast<VkBuf*>(src)->buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}

void VkCmd::copyTexture(Texture* dst, Texture* src) {
    auto* d = static_cast<VkTex*>(dst);
    auto* s = static_cast<VkTex*>(src);
    VkImageCopy r{};
    r.srcSubresource = {s->aspect, 0, 0, 1};
    r.dstSubresource = {d->aspect, 0, 0, 1};
    r.extent = {s->width(), s->height(), 1};
    vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &r);
}

void VkCmd::beginMarker(const char* name) {
    if (!dev_->debugUtils_) return;
    VkDebugUtilsLabelEXT l{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    l.pLabelName = name;
    vkCmdBeginDebugUtilsLabelEXT(cb, &l);
}

void VkCmd::endMarker() {
    if (dev_->debugUtils_) vkCmdEndDebugUtilsLabelEXT(cb);
}

}  // namespace taelia::rhi::vk
