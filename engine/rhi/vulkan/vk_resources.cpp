// Vulkan textures, buffers, uploads and readback.
#include <cstdio>
#include <cstring>

#include "vk_internal.h"

namespace taelia::rhi::vk {

// ---------------------------------------------------------------- lifetime (deferred by frame fence)
VkTex::~VkTex() {
    VulkanDevice* dev = dev_;
    std::vector<VkImageView> views = uavViews;
    views.push_back(srvView);
    views.push_back(rtvView);
    std::vector<uint32_t> indices = uavList();
    indices.push_back(srv());
    if (!ownsImage) {  // swapchain image: the swapchain owner waited for the GPU already
        for (VkImageView v : views)
            if (v) vkDestroyImageView(dev->device, v, nullptr);
        return;
    }
    VkImage img = image;
    VmaAllocation a = alloc;
    dev->retire([dev, views, indices, img, a] {
        for (VkImageView v : views)
            if (v) vkDestroyImageView(dev->device, v, nullptr);
        for (uint32_t i : indices) dev->freeIndex(i);
        if (img) vmaDestroyImage(dev->allocator, img, a);
    });
}

VkBuf::~VkBuf() {
    VulkanDevice* dev = dev_;
    VkBuffer b = buffer;
    VmaAllocation a = alloc;
    const uint32_t s = srv(), u = uav();
    dev->retire([dev, b, a, s, u] {
        dev->freeIndex(s);
        dev->freeIndex(u);
        if (b) vmaDestroyBuffer(dev->allocator, b, a);
    });
}

// ---------------------------------------------------------------- textures
std::unique_ptr<Texture> VulkanDevice::createTexture(const TextureDesc& d) {
    auto t = std::make_unique<VkTex>(this, d);
    t->vkFormat = toVk(d.format);
    t->aspect = isDepthFormat(d.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    const uint32_t mips = d.mips ? d.mips : 1;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = d.dim == TextureDim::Tex3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = t->vkFormat;
    ci.extent = {d.width, d.height, d.dim == TextureDim::Tex3D ? d.depthOrLayers : 1};
    ci.mipLevels = mips;
    uint32_t layers = d.dim == TextureDim::Tex3D ? 1 : (d.depthOrLayers ? d.depthOrLayers : 1);
    if (d.dim == TextureDim::Cube) {
        ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        if (layers < 6) layers = 6;
    }
    t->layers = layers;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (d.usage & TextureUsage::Sampled) ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (d.usage & TextureUsage::Storage) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (d.usage & TextureUsage::RenderTarget) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (d.usage & TextureUsage::DepthStencil) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (d.usage & (TextureUsage::RenderTarget | TextureUsage::DepthStencil))
        ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    t->vkUsage = ci.usage;
    const VkResult r = vmaCreateImage(allocator, &ci, &ai, &t->image, &t->alloc, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] cannot create texture %s (%d)\n", d.name.c_str(), int(r));
        t->image = VK_NULL_HANDLE;
        return nullptr;
    }
    setName(VK_OBJECT_TYPE_IMAGE, uint64_t(t->image), d.name);

    auto makeView = [&](VkImageViewType type, uint32_t baseMip, uint32_t mipCount, uint32_t layerCount) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t->image;
        vi.viewType = type;
        vi.format = t->vkFormat;
        vi.subresourceRange = {t->aspect, baseMip, mipCount, 0, layerCount};
        VkImageView v = VK_NULL_HANDLE;
        vkCreateImageView(device, &vi, nullptr, &v);
        return v;
    };
    VkImageViewType srvType = VK_IMAGE_VIEW_TYPE_2D;
    if (d.dim == TextureDim::Tex3D) srvType = VK_IMAGE_VIEW_TYPE_3D;
    else if (d.dim == TextureDim::Cube) srvType = layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    else if (d.dim == TextureDim::Tex2DArray) srvType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (d.usage & TextureUsage::Sampled) {
        t->srvView = makeView(srvType, 0, mips, layers);
        t->setSrv(allocIndex());
        writeImageDescriptor(t->srv(), t->srvView, false);
    }
    if (d.usage & TextureUsage::Storage) {
        const VkImageViewType uavType = d.dim == TextureDim::Tex3D ? VK_IMAGE_VIEW_TYPE_3D
                                        : layers > 1               ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                                   : VK_IMAGE_VIEW_TYPE_2D;
        for (uint32_t m = 0; m < mips; ++m) {
            t->uavViews.push_back(makeView(uavType, m, 1, layers));
            t->addUav(allocIndex());
            writeImageDescriptor(t->uavList().back(), t->uavViews.back(), true);
        }
    }
    if (d.usage & (TextureUsage::RenderTarget | TextureUsage::DepthStencil))
        t->rtvView = makeView(d.dim == TextureDim::Tex3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, 0, 1, 1);
    return t;
}

// ---------------------------------------------------------------- buffers
std::unique_ptr<Buffer> VulkanDevice::createBuffer(const BufferDesc& d, const void* initialData) {
    if (d.size == 0) return nullptr;
    auto b = std::make_unique<VkBuf>(this, d);
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = d.size;
    ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (d.usage & BufferUsage::Storage) ci.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (d.usage & BufferUsage::Index) ci.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (d.usage & BufferUsage::Constant) ci.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (d.usage & BufferUsage::Indirect) ci.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if ((d.usage & BufferUsage::AccelerationInput) && rayTracing_)
        ci.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (d.memory == Memory::GpuOnly) {
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    } else {
        ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   (d.memory == Memory::Upload ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                               : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
        ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    VmaAllocationInfo info{};
    const VkResult r = vmaCreateBuffer(allocator, &ci, &ai, &b->buffer, &b->alloc, &info);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] cannot create buffer %s (%d)\n", d.name.c_str(), int(r));
        b->buffer = VK_NULL_HANDLE;
        return nullptr;
    }
    setName(VK_OBJECT_TYPE_BUFFER, uint64_t(b->buffer), d.name);
    if (d.memory != Memory::GpuOnly) b->setMapped(info.pMappedData);
    if (d.usage & BufferUsage::Storage) {
        b->setSrv(allocIndex());
        writeBufferDescriptor(b->srv(), b->buffer);
        if (d.memory == Memory::GpuOnly) {
            b->setUav(allocIndex());
            writeBufferDescriptor(b->uav(), b->buffer);
        }
    }
    if (initialData) {
        if (b->mapped()) {
            std::memcpy(b->mapped(), initialData, d.size);
        } else {
            auto staging = createBuffer({d.size, BufferUsage::CopySrc, Memory::Upload, d.name + ".staging"}, initialData);
            if (!staging) return nullptr;
            VkCommandBuffer cb = beginImmediate();
            bufferBarrier(cb, b.get(), State::CopyDst);
            const VkBufferCopy region{0, 0, d.size};
            vkCmdCopyBuffer(cb, static_cast<VkBuf*>(staging.get())->buffer, b->buffer, 1, &region);
            const State after = (d.usage & (BufferUsage::Storage | BufferUsage::AccelerationInput)) ? State::ShaderRead
                                : (d.usage & BufferUsage::Index)    ? State::IndexBuffer
                                : (d.usage & BufferUsage::Indirect) ? State::IndirectArgument
                                                                    : State::CopyDst;
            bufferBarrier(cb, b.get(), after);
            endImmediate();
        }
    }
    return b;
}

// ---------------------------------------------------------------- texture upload (mip 0, all layers / slices)
void VulkanDevice::uploadTexture(Texture* tex, const void* data, size_t size) {
    auto* t = static_cast<VkTex*>(tex);
    if (!t || !data || !size) return;
    auto staging = createBuffer({size, BufferUsage::CopySrc, Memory::Upload, t->desc().name + ".staging"}, data);
    if (!staging) return;
    const auto& d = t->desc();
    VkCommandBuffer cb = beginImmediate();
    textureBarrier(this, cb, t, State::CopyDst);
    VkBufferImageCopy region{};
    region.imageSubresource = {t->aspect, 0, 0, t->layers};
    region.imageExtent = {d.width, d.height, d.dim == TextureDim::Tex3D ? d.depthOrLayers : 1};
    vkCmdCopyBufferToImage(cb, static_cast<VkBuf*>(staging.get())->buffer, t->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    textureBarrier(this, cb, t, State::ShaderRead);
    endImmediate();
}

// ---------------------------------------------------------------- readback (RGBA8 / BGRA8, mip 0)
bool VulkanDevice::readbackRGBA8(Texture* tex, std::vector<uint8_t>& out) {
    auto* t = static_cast<VkTex*>(tex);
    if (!t) return false;
    const auto& d = t->desc();
    const bool bgra = d.format == Format::BGRA8_UNORM;
    if (!bgra && d.format != Format::RGBA8_UNORM && d.format != Format::RGBA8_SRGB) return false;
    const uint64_t bytes = uint64_t(d.width) * d.height * 4;
    auto rb = createBuffer({bytes, BufferUsage::CopyDst, Memory::Readback, "readback"}, nullptr);
    if (!rb) return false;
    const State prev = t->state;
    VkCommandBuffer cb = beginImmediate();
    textureBarrier(this, cb, t, State::CopySrc);
    VkBufferImageCopy region{};
    region.imageSubresource = {t->aspect, 0, 0, 1};
    region.imageExtent = {d.width, d.height, 1};
    auto* rbb = static_cast<VkBuf*>(rb.get());
    vkCmdCopyImageToBuffer(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbb->buffer, 1, &region);
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(cb, &dep);
    if (prev != State::Undefined) textureBarrier(this, cb, t, prev);
    endImmediate();
    out.resize(size_t(bytes));
    std::memcpy(out.data(), rb->mapped(), size_t(bytes));
    if (bgra)
        for (size_t i = 0; i < out.size(); i += 4) std::swap(out[i], out[i + 2]);
    return true;
}

}  // namespace taelia::rhi::vk
