// Vulkan device: instance, validation, device, bindless descriptor set, frame loop, deferred deletion.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <windows.h>

#include "vk_internal.h"

namespace taelia::rhi {

std::unique_ptr<Device> createVulkanDevice(const DeviceDesc& desc, std::string* error) {
    auto dev = std::make_unique<vk::VulkanDevice>();
    if (!dev->init(desc, error)) return nullptr;
    return dev;
}

}  // namespace taelia::rhi

namespace taelia::rhi::vk {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT type,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    // Performance warnings are not counted (only validation and general).
    if (!(type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT)))
        return VK_FALSE;
    if (!(severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)))
        return VK_FALSE;
    // Benign loader noise: the machine simply has no layer manifests registered in the registry.
    if (data->pMessage && std::strstr(data->pMessage, "Registry lookup failed to get layer manifest files"))
        return VK_FALSE;
    auto* dev = static_cast<VulkanDevice*>(user);
    dev->debugMessages_.fetch_add(1);
    const bool err = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    std::string msg = std::string("[vulkan ") + (err ? "error" : "warning") + "] " +
                      (data->pMessageIdName ? data->pMessageIdName : "") + ": " +
                      (data->pMessage ? data->pMessage : "") + "\n";
    std::fputs(msg.c_str(), stderr);
    OutputDebugStringA(msg.c_str());
    return VK_FALSE;
}

std::string exeDir() {
    char buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameA(nullptr, buf, DWORD(sizeof buf));
    std::string s(buf, n);
    const size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
}

bool hasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

}  // namespace

VkFormat toVk(Format f) {
    switch (f) {
        case Format::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::RGB10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case Format::R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case Format::RGBA16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::RGBA32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::RG16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
        case Format::R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
        case Format::R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case Format::R8_UNORM: return VK_FORMAT_R8_UNORM;
        case Format::RG8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case Format::R32_UINT: return VK_FORMAT_R32_UINT;
        case Format::D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
        case Format::Unknown: break;
    }
    return VK_FORMAT_UNDEFINED;
}

#define VK_CHECK_INIT(expr, what)                                       \
    do {                                                                \
        const VkResult r_ = (expr);                                     \
        if (r_ != VK_SUCCESS) {                                         \
            if (error) *error = std::string(what) + " failed (VkResult " + std::to_string(int(r_)) + ")"; \
            return false;                                               \
        }                                                               \
    } while (0)

bool VulkanDevice::init(const DeviceDesc& desc, std::string* error) {
    shaderDir_ = desc.shaderDir;
    if (desc.debug) {
        // Only this process: the validation layer copied next to the exe.
        const std::string layerDir = exeDir() + "\\vulkan-layers";
        SetEnvironmentVariableA("VK_ADD_LAYER_PATH", layerDir.c_str());
        _putenv_s("VK_ADD_LAYER_PATH", layerDir.c_str());
    }
    SlRequirements slReq;  // Streamline first: volk then goes through its vkGetInstanceProcAddr
    if (!(desc.streamline && slLoad(this, slReq))) VK_CHECK_INIT(volkInitialize(), "volkInitialize (no Vulkan loader)");
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        if (error) *error = "Vulkan 1.3 loader required";
        return false;
    }

    // ---------------------------------------------------------------- instance
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> instExts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, instExts.data());
    std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    debugUtils_ = hasExtension(instExts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    std::vector<const char*> layers;
    if (desc.debug) {
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layerProps(count);
        vkEnumerateInstanceLayerProperties(&count, layerProps.data());
        for (auto& l : layerProps)
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) validation_ = true;
        if (validation_) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            // The layer provides debug utils itself.
            vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &count, nullptr);
            std::vector<VkExtensionProperties> layerExts(count);
            vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &count, layerExts.data());
            debugUtils_ = debugUtils_ || hasExtension(layerExts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        if (validation_ && !debugUtils_) validation_ = false;  // can't see its messages
        if (!validation_) layers.clear();
    }
    if (debugUtils_) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    for (const std::string& e : slReq.instanceExtensions) {
        bool have = false;
        for (const char* x : extensions) have |= e == x;
        if (!have && hasExtension(instExts, e.c_str())) extensions.push_back(e.c_str());
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "WondersOfTaelia";
    app.pEngineName = "Taelia";
    app.apiVersion = VK_API_VERSION_1_3;

    VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    dbg.pfnUserCallback = debugCallback;
    dbg.pUserData = this;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = uint32_t(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    if (validation_) ici.pNext = &dbg;
    VK_CHECK_INIT(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
    volkLoadInstanceOnly(instance);
    if (validation_) VK_CHECK_INIT(vkCreateDebugUtilsMessengerEXT(instance, &dbg, nullptr, &messenger), "debug messenger");

    // ---------------------------------------------------------------- physical device: discrete first, Vulkan 1.3
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance, &count, gpus.data());
    int bestScore = -1;
    for (VkPhysicalDevice pd : gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t qc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, qf.data());
        int family = -1;
        for (uint32_t i = 0; i < qc; ++i)
            if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                    (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) &&
                vkGetPhysicalDeviceWin32PresentationSupportKHR(pd, i)) {
                family = int(i);
                break;
            }
        if (family < 0) continue;
        const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                          : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                                   : 1;
        if (score > bestScore) {
            bestScore = score;
            physical = pd;
            props = p;
            queueFamily = uint32_t(family);
        }
    }
    if (!physical) {
        if (error) *error = "no Vulkan 1.3 GPU with a graphics queue";
        return false;
    }
    adapterName_ = props.deviceName;

    // ---------------------------------------------------------------- features
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> devExtProps(count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, devExtProps.data());
    const bool rtExts = hasExtension(devExtProps, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                        hasExtension(devExtProps, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                        hasExtension(devExtProps, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    VkPhysicalDeviceRayQueryFeaturesKHR sRq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR sAs{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &sRq};
    VkPhysicalDeviceVulkan13Features s13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    if (rtExts) s13.pNext = &sAs;
    VkPhysicalDeviceVulkan12Features s12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &s13};
    VkPhysicalDeviceVulkan11Features s11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &s12};
    VkPhysicalDeviceFeatures2 sup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &s11};
    vkGetPhysicalDeviceFeatures2(physical, &sup);

    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13};
    VkPhysicalDeviceVulkan11Features e11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &e12};
    VkPhysicalDeviceFeatures2 en{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e11};
    std::string missing;
#define REQ(e, s, f) \
    if (!(s).f) missing += " " #f; \
    (e).f = VK_TRUE
#define OPT(e, s, f) (e).f = (s).f
    REQ(e13, s13, dynamicRendering);
    REQ(e13, s13, synchronization2);
    OPT(e13, s13, maintenance4);
    REQ(e13, s13, shaderDemoteToHelperInvocation);  // DXC turns `discard` (alpha-tested leaves) into demote
    REQ(e12, s12, timelineSemaphore);
    REQ(e12, s12, descriptorIndexing);
    REQ(e12, s12, runtimeDescriptorArray);
    REQ(e12, s12, descriptorBindingPartiallyBound);
    REQ(e12, s12, descriptorBindingSampledImageUpdateAfterBind);
    REQ(e12, s12, descriptorBindingStorageImageUpdateAfterBind);
    REQ(e12, s12, descriptorBindingStorageBufferUpdateAfterBind);
    REQ(e12, s12, descriptorBindingUpdateUnusedWhilePending);
    REQ(e12, s12, shaderSampledImageArrayNonUniformIndexing);
    OPT(e12, s12, shaderStorageImageArrayNonUniformIndexing);
    OPT(e12, s12, shaderStorageBufferArrayNonUniformIndexing);
    OPT(e12, s12, shaderFloat16);
    OPT(e12, s12, scalarBlockLayout);
    OPT(e12, s12, uniformBufferStandardLayout);
    OPT(e12, s12, shaderSubgroupExtendedTypes);
    OPT(e12, s12, drawIndirectCount);
    OPT(e12, s12, samplerFilterMinmax);
    OPT(e11, s11, storageBuffer16BitAccess);
    OPT(e11, s11, uniformAndStorageBuffer16BitAccess);
    OPT(e11, s11, storagePushConstant16);
    OPT(e11, s11, shaderDrawParameters);
    REQ(en.features, sup.features, shaderStorageImageWriteWithoutFormat);
    OPT(en.features, sup.features, shaderStorageImageReadWithoutFormat);
    REQ(en.features, sup.features, samplerAnisotropy);
    OPT(en.features, sup.features, fillModeNonSolid);
    OPT(en.features, sup.features, shaderInt16);
    OPT(en.features, sup.features, shaderInt64);
    OPT(en.features, sup.features, independentBlend);
    OPT(en.features, sup.features, imageCubeArray);
    OPT(en.features, sup.features, multiDrawIndirect);
    OPT(en.features, sup.features, drawIndirectFirstInstance);
    OPT(en.features, sup.features, depthClamp);
    OPT(en.features, sup.features, depthBiasClamp);
    OPT(en.features, sup.features, shaderImageGatherExtended);
    OPT(en.features, sup.features, fragmentStoresAndAtomics);
    OPT(en.features, sup.features, vertexPipelineStoresAndAtomics);
    OPT(e12, s12, bufferDeviceAddress);
    // Inline ray tracing: optional, all or nothing.
    rayTracing_ = rtExts && sAs.accelerationStructure && sAs.descriptorBindingAccelerationStructureUpdateAfterBind &&
                  sRq.rayQuery && s12.bufferDeviceAddress;
    VkPhysicalDeviceRayQueryFeaturesKHR eRq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR eAs{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &eRq};
    if (rayTracing_) {
        eRq.rayQuery = VK_TRUE;
        eAs.accelerationStructure = VK_TRUE;
        eAs.descriptorBindingAccelerationStructureUpdateAfterBind = VK_TRUE;
        e13.pNext = &eAs;
        g_extraReadStages = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    } else {
        g_extraReadStages = 0;
    }
#undef REQ
#undef OPT
    // What the Streamline features ask for, where the GPU has it.
    mergeFeatures(e12, slReq.features12, s12);
    mergeFeatures(e13, slReq.features13, s13);
    if (streamline_) e13.privateData = s13.privateData;  // NGX creates private data slots
    if (!missing.empty()) {
        if (error) *error = "GPU lacks required Vulkan features:" + missing;
        return false;
    }

    // Our queue first, then the ones Streamline's features ask for (same graphics + compute family).
    uint32_t slGraphics = slReq.graphicsQueues, slCompute = slReq.computeQueues;
    {
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> qf(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, qf.data());
        const uint32_t room = qf[queueFamily].queueCount - 1;
        if (slGraphics > room) slGraphics = room;
        if (slCompute > room - slGraphics) slCompute = room - slGraphics;
    }
    const std::vector<float> prios(1 + slGraphics + slCompute, 1.0f);
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = uint32_t(prios.size());
    qci.pQueuePriorities = prios.data();
    std::vector<const char*> devExts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (rayTracing_)
        devExts.insert(devExts.end(), {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                       VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME});
    // Streamline's extensions where the GPU has them. VK_EXT_buffer_device_address is core (1.2 feature) here;
    // VK_NV_low_latency2 (Reflex) needs VK_KHR_present_id.
    VkPhysicalDevicePresentIdFeaturesKHR presentId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    std::vector<std::string> slExts;
    for (const std::string& e : slReq.deviceExtensions) {
        if (e == VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) continue;
        if (e == VK_NV_LOW_LATENCY_2_EXTENSION_NAME) {
            if (!hasExtension(devExtProps, VK_KHR_PRESENT_ID_EXTENSION_NAME)) continue;
            slExts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
        }
        slExts.push_back(e);
    }
    for (const std::string& e : slExts) {
        bool have = false;
        for (const char* x : devExts) have |= e == x;
        if (have || !hasExtension(devExtProps, e.c_str())) continue;
        devExts.push_back(e.c_str());
        if (e == VK_KHR_PRESENT_ID_EXTENSION_NAME) {
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &presentId};
            vkGetPhysicalDeviceFeatures2(physical, &q);
            presentId.pNext = e11.pNext;  // keep the chain: en -> e11 -> presentId -> e12 ...
            e11.pNext = &presentId;
        }
    }
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &en};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();
    VK_CHECK_INIT(vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
    volkLoadDevice(device);
    slHandOver(this, 1, 1 + slGraphics);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VmaVulkanFunctions vf{};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.instance = instance;
    aci.physicalDevice = physical;
    aci.device = device;
    aci.pVulkanFunctions = &vf;
    if (e12.bufferDeviceAddress) aci.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    VK_CHECK_INIT(vmaCreateAllocator(&aci, &allocator), "vmaCreateAllocator");

    // ---------------------------------------------------------------- samplers (s0..s5)
    for (int i = 0; i < 6; ++i) {
        VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        const bool point = i == 2 || i == 4;
        const bool wrap = i == 0 || i == 3 || i == 4;
        s.magFilter = s.minFilter = point ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        s.mipmapMode = point ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        s.addressModeU = s.addressModeV = s.addressModeW =
            wrap ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.maxLod = VK_LOD_CLAMP_NONE;
        if (i == 3) {
            s.anisotropyEnable = VK_TRUE;
            s.maxAnisotropy = props.limits.maxSamplerAnisotropy < 16.0f ? props.limits.maxSamplerAnisotropy : 16.0f;
        }
        if (i == 5) {
            s.compareEnable = VK_TRUE;
            s.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        }
        VK_CHECK_INIT(vkCreateSampler(device, &s, nullptr, &samplers[i]), "vkCreateSampler");
    }

    // ---------------------------------------------------------------- bindless set 0, frame constants set 1, layout
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR pAs{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
    if (rayTracing_) p12.pNext = &pAs;
    vkGetPhysicalDeviceProperties2(physical, &p2);
    // Indices are shared by every binding, so the acceleration-structure binding clamps the common count too.
    const uint32_t bindlessArrays = rayTracing_ ? 4 : 3;
    for (uint32_t lim : {p12.maxDescriptorSetUpdateAfterBindSampledImages, p12.maxDescriptorSetUpdateAfterBindStorageImages,
                         p12.maxDescriptorSetUpdateAfterBindStorageBuffers,
                         p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                         p12.maxPerStageDescriptorUpdateAfterBindStorageImages,
                         p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers,
                         (p12.maxPerStageUpdateAfterBindResources - 64) / bindlessArrays})
        if (lim < bindlessCount) bindlessCount = lim;
    if (rayTracing_) {
        for (uint32_t lim : {pAs.maxDescriptorSetUpdateAfterBindAccelerationStructures,
                             pAs.maxPerStageDescriptorUpdateAfterBindAccelerationStructures})
            if (lim < bindlessCount) bindlessCount = lim;
        if (pAs.minAccelerationStructureScratchOffsetAlignment > scratchAlign_)
            scratchAlign_ = pAs.minAccelerationStructureScratchOffsetAlignment;
    }

    // Bindings 0..3: bindless arrays (3 = acceleration structures, only when ray tracing is supported).
    const VkDescriptorType bindlessTypes[4] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                               VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    VkDescriptorSetLayoutBinding b[10]{};
    VkDescriptorBindingFlags bf[10]{};
    uint32_t nb = 0;
    for (uint32_t i = 0; i < bindlessArrays; ++i, ++nb) {
        b[nb].binding = i;
        b[nb].descriptorType = bindlessTypes[i];
        b[nb].descriptorCount = bindlessCount;
        b[nb].stageFlags = VK_SHADER_STAGE_ALL;
        bf[nb] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                 VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }
    for (uint32_t i = 0; i < 6; ++i, ++nb) {
        b[nb].binding = 4 + i;
        b[nb].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        b[nb].descriptorCount = 1;
        b[nb].stageFlags = VK_SHADER_STAGE_ALL;
        b[nb].pImmutableSamplers = &samplers[i];
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = nb;
    bfi.pBindingFlags = bf;
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    dl.bindingCount = nb;
    dl.pBindings = b;
    VK_CHECK_INIT(vkCreateDescriptorSetLayout(device, &dl, nullptr, &setLayout0), "set 0 layout");

    VkDescriptorSetLayoutBinding fb{};
    fb.binding = 0;
    fb.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    fb.descriptorCount = 1;
    fb.stageFlags = VK_SHADER_STAGE_ALL;
    VkDescriptorSetLayoutCreateInfo dl1{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl1.bindingCount = 1;
    dl1.pBindings = &fb;
    VK_CHECK_INIT(vkCreateDescriptorSetLayout(device, &dl1, nullptr, &setLayout1), "set 1 layout");

    VkPushConstantRange pcr{VK_SHADER_STAGE_ALL, 0, kPushConstantBytes};
    const VkDescriptorSetLayout setLayouts[2] = {setLayout0, setLayout1};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = setLayouts;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VK_CHECK_INIT(vkCreatePipelineLayout(device, &pli, nullptr, &pipelineLayout), "pipeline layout");

    const VkDescriptorPoolSize sizes[6] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, bindlessCount},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, bindlessCount},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bindlessCount},
                                           {VK_DESCRIPTOR_TYPE_SAMPLER, 6},
                                           {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1},
                                           {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, bindlessCount}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    dpi.maxSets = 2;
    dpi.poolSizeCount = rayTracing_ ? 6 : 5;
    dpi.pPoolSizes = sizes;
    VK_CHECK_INIT(vkCreateDescriptorPool(device, &dpi, nullptr, &pool), "descriptor pool");
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool;
    dai.descriptorSetCount = 2;
    dai.pSetLayouts = setLayouts;
    VkDescriptorSet sets[2];
    VK_CHECK_INIT(vkAllocateDescriptorSets(device, &dai, sets), "descriptor sets");
    set0 = sets[0];
    set1 = sets[1];

    // ---------------------------------------------------------------- frame loop objects
    VkSemaphoreTypeCreateInfo sti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo tsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sti};
    VK_CHECK_INIT(vkCreateSemaphore(device, &tsci, nullptr, &timeline), "timeline semaphore");
    setName(VK_OBJECT_TYPE_SEMAPHORE, uint64_t(timeline), "frame timeline");
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpi.queueFamilyIndex = queueFamily;
    for (auto& f : frames_) {
        VK_CHECK_INIT(vkCreateCommandPool(device, &cpi, nullptr, &f.pool), "command pool");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = f.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK_INIT(vkAllocateCommandBuffers(device, &cai, &f.cb), "command buffer");
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK_INIT(vkCreateSemaphore(device, &sci, nullptr, &f.acquire), "semaphore");
    }
    VK_CHECK_INIT(vkCreateCommandPool(device, &cpi, nullptr, &immPool), "command pool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = immPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_CHECK_INIT(vkAllocateCommandBuffers(device, &cai, &immCb), "command buffer");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK_INIT(vkCreateFence(device, &fci, nullptr, &immFence), "fence");

    // ---------------------------------------------------------------- upload ring (+ slack so b0's fixed range fits)
    BufferDesc rd;
    rd.size = kRingBytesPerFrame * kFramesInFlight + kFrameConstantBytes;
    rd.usage = BufferUsage::Storage | BufferUsage::Index | BufferUsage::Constant;
    rd.memory = Memory::Upload;
    rd.name = "upload ring";
    if (rayTracing_) rd.usage |= BufferUsage::AccelerationInput;  // TLAS instance data
    auto ring = createBuffer(rd, nullptr);
    if (!ring) {
        if (error) *error = "cannot create the upload ring";
        return false;
    }
    ring_.reset(static_cast<VkBuf*>(ring.release()));
    if (rayTracing_) ringAddress_ = bufferAddress(ring_->buffer);
    VkDescriptorBufferInfo dbi{ring_->buffer, 0, kFrameConstantBytes};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set1;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    return true;
}

}  // namespace taelia::rhi::vk
