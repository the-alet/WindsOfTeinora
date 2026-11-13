// Vulkan inline ray tracing (acceleration structures for VK_KHR_ray_query) and indirect draws / dispatches.
#include <cstdio>
#include <cstring>

#include "vk_internal.h"

namespace taelia::rhi::vk {

namespace {

constexpr VkBuildAccelerationStructureFlagsKHR kBuildFlags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;

// Triangle geometries of a BLAS desc; device addresses only when `addresses` (size queries ignore them).
void blasGeometry(VulkanDevice* dev, const BlasDesc& d, bool addresses, std::vector<VkAccelerationStructureGeometryKHR>& geoms,
                  std::vector<uint32_t>& prims) {
    geoms.clear();
    prims.clear();
    for (const BlasGeometry& g : d.geometries) {
        VkAccelerationStructureGeometryKHR ge{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        ge.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        ge.flags = g.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
        auto& t = ge.geometry.triangles;
        t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        t.vertexStride = g.vertexStride;
        t.maxVertex = g.vertexCount ? g.vertexCount - 1 : 0;
        t.indexType = g.indexType == IndexType::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        if (addresses) {
            t.vertexData.deviceAddress = dev->bufferAddress(static_cast<VkBuf*>(g.vertices)->buffer) + g.vertexOffset;
            t.indexData.deviceAddress = dev->bufferAddress(static_cast<VkBuf*>(g.indices)->buffer) + g.indexOffset;
        }
        geoms.push_back(ge);
        prims.push_back(g.indexCount / 3);
    }
}

void memoryBarrier(VkCommandBuffer cb, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = srcStage;
    m.srcAccessMask = srcAccess;
    m.dstStageMask = dstStage;
    m.dstAccessMask = dstAccess;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cb, &dep);
}

// Before a build: earlier builds (BLAS results read by a TLAS build, or the same structure / scratch rewritten) and
// earlier readers of the structure (previous frames' ray queries) must be done.
void preBuildBarrier(VkCommandBuffer cb) {
    memoryBarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                  VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                  VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
}

}  // namespace

// ---------------------------------------------------------------- lifetime
VkAccel::~VkAccel() {
    VulkanDevice* dev = dev_;
    VkAccelerationStructureKHR a = as;
    VkBuffer b = buffer, s = scratch;
    VmaAllocation ba = alloc, sa = scratchAlloc;
    const uint32_t index = srv();
    dev->retire([dev, a, b, s, ba, sa, index] {
        if (a) vkDestroyAccelerationStructureKHR(dev->device, a, nullptr);
        if (b) vmaDestroyBuffer(dev->allocator, b, ba);
        if (s) vmaDestroyBuffer(dev->allocator, s, sa);
        dev->freeIndex(index);
    });
}

VkDeviceAddress VulkanDevice::bufferAddress(VkBuffer b) const {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = b;
    return vkGetBufferDeviceAddress(device, &info);
}

void VulkanDevice::writeAccelDescriptor(uint32_t index, VkAccelerationStructureKHR as) {
    if (!index) return;
    VkWriteDescriptorSetAccelerationStructureKHR wa{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    wa.accelerationStructureCount = 1;
    wa.pAccelerationStructures = &as;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &wa};
    w.dstSet = set0;
    w.dstBinding = 3;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

bool VulkanDevice::createAccel(VkAccel* a, VkAccelerationStructureTypeKHR type, VkDeviceSize asSize,
                               VkDeviceSize scratchSize, const std::string& name) {
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = asSize;
    ci.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkResult r = vmaCreateBuffer(allocator, &ci, &ai, &a->buffer, &a->alloc, nullptr);
    if (r != VK_SUCCESS) {
        a->buffer = VK_NULL_HANDLE;
        std::fprintf(stderr, "[vulkan] cannot create acceleration structure buffer %s (%d)\n", name.c_str(), int(r));
        return false;
    }
    setName(VK_OBJECT_TYPE_BUFFER, uint64_t(a->buffer), name);
    // Persistent scratch per structure: only this structure's builds use it (ordered by preBuildBarrier).
    ci.size = (scratchSize ? scratchSize : 1);
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    r = vmaCreateBufferWithAlignment(allocator, &ci, &ai, scratchAlign_, &a->scratch, &a->scratchAlloc, nullptr);
    if (r != VK_SUCCESS) {
        a->scratch = VK_NULL_HANDLE;
        std::fprintf(stderr, "[vulkan] cannot create acceleration structure scratch %s (%d)\n", name.c_str(), int(r));
        return false;
    }
    setName(VK_OBJECT_TYPE_BUFFER, uint64_t(a->scratch), name + ".scratch");
    a->scratchAddress = bufferAddress(a->scratch);

    VkAccelerationStructureCreateInfoKHR asi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    asi.buffer = a->buffer;
    asi.size = asSize;
    asi.type = type;
    r = vkCreateAccelerationStructureKHR(device, &asi, nullptr, &a->as);
    if (r != VK_SUCCESS) {
        a->as = VK_NULL_HANDLE;
        std::fprintf(stderr, "[vulkan] cannot create acceleration structure %s (%d)\n", name.c_str(), int(r));
        return false;
    }
    setName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, uint64_t(a->as), name);
    VkAccelerationStructureDeviceAddressInfoKHR dai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    dai.accelerationStructure = a->as;
    a->address = vkGetAccelerationStructureDeviceAddressKHR(device, &dai);
    return true;
}

// ---------------------------------------------------------------- creation
std::unique_ptr<AccelerationStructure> VulkanDevice::createBlas(const BlasDesc& desc) {
    if (!rayTracing_ || desc.geometries.empty()) return nullptr;
    std::vector<VkAccelerationStructureGeometryKHR> geoms;
    std::vector<uint32_t> prims;
    blasGeometry(this, desc, false, geoms, prims);
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = kBuildFlags;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = uint32_t(geoms.size());
    bi.pGeometries = geoms.data();
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, prims.data(),
                                            &sizes);
    auto a = std::make_unique<VkAccel>(this, false);
    if (!createAccel(a.get(), VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize,
                     sizes.buildScratchSize, desc.name))
        return nullptr;
    return a;
}

std::unique_ptr<AccelerationStructure> VulkanDevice::createTlas(uint32_t maxInstances, const std::string& name) {
    if (!rayTracing_) return nullptr;
    if (!maxInstances) maxInstances = 1;
    VkAccelerationStructureGeometryKHR ge{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    ge.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    ge.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bi.flags = kBuildFlags;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &ge;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &maxInstances,
                                            &sizes);
    auto a = std::make_unique<VkAccel>(this, true);
    a->maxInstances = maxInstances;
    if (!createAccel(a.get(), VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizes.accelerationStructureSize,
                     sizes.buildScratchSize, name))
        return nullptr;
    a->setSrv(allocIndex());
    writeAccelDescriptor(a->srv(), a->as);
    return a;
}

// ---------------------------------------------------------------- builds
void VkCmd::buildBlas(AccelerationStructure* blas, const BlasDesc& desc) {
    auto* a = static_cast<VkAccel*>(blas);
    if (!a || !a->as) return;
    std::vector<VkAccelerationStructureGeometryKHR> geoms;
    std::vector<uint32_t> prims;
    blasGeometry(dev_, desc, true, geoms, prims);
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(geoms.size());
    for (size_t i = 0; i < ranges.size(); ++i) ranges[i] = {prims[i], 0, 0, 0};
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = kBuildFlags;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.dstAccelerationStructure = a->as;
    bi.geometryCount = uint32_t(geoms.size());
    bi.pGeometries = geoms.data();
    bi.scratchData.deviceAddress = a->scratchAddress;
    preBuildBarrier(cb);
    const VkAccelerationStructureBuildRangeInfoKHR* pr = ranges.data();
    vkCmdBuildAccelerationStructuresKHR(cb, 1, &bi, &pr);
}

void VkCmd::buildTlas(AccelerationStructure* tlas, const TlasInstance* instances, uint32_t count) {
    auto* a = static_cast<VkAccel*>(tlas);
    if (!a || !a->as) return;
    if (count > a->maxInstances) count = a->maxInstances;
    // Instance data in the per-frame upload ring (256-aligned, host-coherent: visible at submit).
    const Transient t = dev_->allocTransient(uint64_t(count ? count : 1) * sizeof(VkAccelerationStructureInstanceKHR));
    if (!t.cpu) return;
    auto* dst = static_cast<VkAccelerationStructureInstanceKHR*>(t.cpu);
    for (uint32_t i = 0; i < count; ++i) {
        const TlasInstance& s = instances[i];
        VkAccelerationStructureInstanceKHR v{};
        std::memcpy(v.transform.matrix, s.transform, sizeof v.transform.matrix);
        v.instanceCustomIndex = s.customIndex & 0xFFFFFFu;
        v.mask = s.mask;
        v.instanceShaderBindingTableRecordOffset = 0;
        VkGeometryInstanceFlagsKHR f = 0;
        if (s.doubleSided) f |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        if (s.forceOpaque) f |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        v.flags = f & 0xFFu;
        v.accelerationStructureReference = s.blas ? static_cast<VkAccel*>(s.blas)->address : 0;
        std::memcpy(dst + i, &v, sizeof v);
    }
    VkAccelerationStructureGeometryKHR ge{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    ge.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    ge.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    ge.geometry.instances.data.deviceAddress = dev_->ringAddress_ + t.offset;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bi.flags = kBuildFlags;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.dstAccelerationStructure = a->as;
    bi.geometryCount = 1;
    bi.pGeometries = &ge;
    bi.scratchData.deviceAddress = a->scratchAddress;
    const VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* pr = &range;
    preBuildBarrier(cb);  // also makes the BLAS builds before it visible
    vkCmdBuildAccelerationStructuresKHR(cb, 1, &bi, &pr);
    // Later draws / dispatches see the result.
    memoryBarrier(cb, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                  VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
}

// ---------------------------------------------------------------- indirect
void VkCmd::drawIndirect(Buffer* args, uint64_t offset, uint32_t drawCount) {
    vkCmdDrawIndirect(cb, static_cast<VkBuf*>(args)->buffer, offset, drawCount, 16);
}

void VkCmd::drawIndexedIndirect(Buffer* args, uint64_t offset, uint32_t drawCount) {
    vkCmdDrawIndexedIndirect(cb, static_cast<VkBuf*>(args)->buffer, offset, drawCount, 20);
}

void VkCmd::dispatchIndirect(Buffer* args, uint64_t offset) {
    vkCmdDispatchIndirect(cb, static_cast<VkBuf*>(args)->buffer, offset);
}

}  // namespace taelia::rhi::vk
