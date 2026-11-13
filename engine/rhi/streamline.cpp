#include "rhi/streamline.h"

#include <windows.h>

#include <filesystem>
#include <mutex>

#pragma warning(push, 0)
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>  // defines functions: this translation unit only
#pragma warning(pop)

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace taelia::rhi::streamline {

namespace {

struct State {
    HMODULE dll = nullptr;
    bool dlss = false, reflex = false;
    PFun_slInit* init = nullptr;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slIsFeatureSupported* isSupported = nullptr;
    PFun_slGetFeatureFunction* featureFunction = nullptr;
    PFun_slGetNewFrameToken* newFrameToken = nullptr;
    PFun_slSetConstants* setConstants = nullptr;
    PFun_slSetTagForFrame* setTagForFrame = nullptr;
    PFun_slEvaluateFeature* evaluate = nullptr;
    PFun_slDLSSGetOptimalSettings* dlssOptimal = nullptr;
    PFun_slDLSSSetOptions* dlssSetOptions = nullptr;
    PFun_slReflexSetOptions* reflexSetOptions = nullptr;
    PFun_slReflexSleep* reflexSleep = nullptr;
    PFun_slPCLSetMarker* pclMarker = nullptr;
    sl::FrameToken* token = nullptr;
    uint64_t tokenFrame = ~0ull;
};
State g;
std::mutex g_tokenMutex;

template <typename F>
F* get(const char* name) {
    return reinterpret_cast<F*>(GetProcAddress(g.dll, name));
}

template <typename F>
F* feature(sl::Feature f, const char* name) {
    void* fn = nullptr;
    if (!g.featureFunction || g.featureFunction(f, name, fn) != sl::Result::eOk) return nullptr;
    return reinterpret_cast<F*>(fn);
}

sl::DLSSMode toDlss(UpscaleMode m) {
    switch (m) {
        case UpscaleMode::DLAA: return sl::DLSSMode::eDLAA;
        case UpscaleMode::Quality: return sl::DLSSMode::eMaxQuality;
        case UpscaleMode::Balanced: return sl::DLSSMode::eBalanced;
        case UpscaleMode::Performance: return sl::DLSSMode::eMaxPerformance;
        case UpscaleMode::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
        default: return sl::DLSSMode::eOff;
    }
}

// Ours: clip = M * v, rows stored first. Streamline: row vectors (v * M), so it gets the transpose.
void toSl(const float m[16], sl::float4x4& out) {
    for (int r = 0; r < 4; ++r) out[r] = sl::float4(m[0 * 4 + r], m[1 * 4 + r], m[2 * 4 + r], m[3 * 4 + r]);
}

const sl::FrameToken* tokenFor(uint64_t frame) {
    std::lock_guard<std::mutex> lock(g_tokenMutex);
    if (g.tokenFrame != frame || !g.token) {
        const uint32_t index = uint32_t(frame);
        if (g.newFrameToken(g.token, &index) != sl::Result::eOk) return nullptr;
        g.tokenFrame = frame;
    }
    return g.token;
}

}  // namespace

bool load(bool vulkan, std::string* why) {
    if (g.dll) return true;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::filesystem::path dir = std::filesystem::path(exe).parent_path();
    const std::wstring interposer = (dir / L"sl.interposer.dll").wstring();
    if (!std::filesystem::exists(interposer)) {
        if (why) *why = "sl.interposer.dll not next to the exe";
        return false;
    }
    if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
        if (why) *why = "sl.interposer.dll is not signed by NVIDIA";
        return false;
    }
    g.dll = LoadLibraryW(interposer.c_str());
    if (!g.dll) {
        if (why) *why = "cannot load sl.interposer.dll";
        return false;
    }
    g.init = get<PFun_slInit>("slInit");
    g.shutdown = get<PFun_slShutdown>("slShutdown");
    g.isSupported = get<PFun_slIsFeatureSupported>("slIsFeatureSupported");
    g.featureFunction = get<PFun_slGetFeatureFunction>("slGetFeatureFunction");
    g.newFrameToken = get<PFun_slGetNewFrameToken>("slGetNewFrameToken");
    g.setConstants = get<PFun_slSetConstants>("slSetConstants");
    g.setTagForFrame = get<PFun_slSetTagForFrame>("slSetTagForFrame");
    g.evaluate = get<PFun_slEvaluateFeature>("slEvaluateFeature");
    if (!g.init || !g.shutdown || !g.isSupported || !g.featureFunction || !g.newFrameToken || !g.setConstants ||
        !g.setTagForFrame || !g.evaluate) {
        if (why) *why = "sl.interposer.dll lacks expected exports";
        unload();
        return false;
    }

    static const sl::Feature features[] = {sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL};
    static std::wstring pluginDir;
    pluginDir = dir.wstring();
    static const wchar_t* paths[1];
    paths[0] = pluginDir.c_str();
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eOff;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    // Manual hooking; no over-the-air updates or downloaded plugins: everything stays inside the project.
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = uint32_t(std::size(features));
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "0.2";
    pref.projectId = "5d0f6a4e-2b8c-4f1e-9a37-7c1e2d9b4a60";  // Wonders of Taelia
    pref.renderAPI = vulkan ? sl::RenderAPI::eVulkan : sl::RenderAPI::eD3D12;
    if (const sl::Result r = g.init(pref, sl::kSDKVersion); r != sl::Result::eOk) {
        if (why) *why = "slInit failed (" + std::to_string(int(r)) + ")";
        FreeLibrary(g.dll);
        g = State{};
        return false;
    }
    return true;
}

void unload() {
    if (!g.dll) return;
    if (g.shutdown) g.shutdown();
    FreeLibrary(g.dll);
    g = State{};
}

bool loaded() { return g.dll != nullptr; }

void* proc(const char* name) { return g.dll ? reinterpret_cast<void*>(GetProcAddress(g.dll, name)) : nullptr; }

void checkSupport(const void* luid, uint32_t luidBytes, void* vkPhysicalDevice) {
    if (!g.dll) return;
    sl::AdapterInfo info{};
    info.deviceLUID = static_cast<uint8_t*>(const_cast<void*>(luid));
    info.deviceLUIDSizeInBytes = luidBytes;
    info.vkPhysicalDevice = vkPhysicalDevice;
    g.dlss = g.isSupported(sl::kFeatureDLSS, info) == sl::Result::eOk;
    g.reflex = g.isSupported(sl::kFeatureReflex, info) == sl::Result::eOk && g.isSupported(sl::kFeaturePCL, info) == sl::Result::eOk;
    if (g.dlss) {
        g.dlssOptimal = feature<PFun_slDLSSGetOptimalSettings>(sl::kFeatureDLSS, "slDLSSGetOptimalSettings");
        g.dlssSetOptions = feature<PFun_slDLSSSetOptions>(sl::kFeatureDLSS, "slDLSSSetOptions");
        g.dlss = g.dlssOptimal && g.dlssSetOptions;
    }
    if (g.reflex) {
        g.reflexSetOptions = feature<PFun_slReflexSetOptions>(sl::kFeatureReflex, "slReflexSetOptions");
        g.reflexSleep = feature<PFun_slReflexSleep>(sl::kFeatureReflex, "slReflexSleep");
        g.pclMarker = feature<PFun_slPCLSetMarker>(sl::kFeaturePCL, "slPCLSetMarker");
        g.reflex = g.reflexSetOptions && g.reflexSleep && g.pclMarker;
    }
}

bool dlssAvailable() { return g.dll && g.dlss; }
bool reflexAvailable() { return g.dll && g.reflex; }

bool renderSize(UpscaleMode mode, uint32_t outW, uint32_t outH, uint32_t& renderW, uint32_t& renderH) {
    renderW = outW;
    renderH = outH;
    if (!dlssAvailable() || mode == UpscaleMode::Off) return false;
    sl::DLSSOptions opt{};
    opt.mode = toDlss(mode);
    opt.outputWidth = outW;
    opt.outputHeight = outH;
    sl::DLSSOptimalSettings s{};
    if (g.dlssOptimal(opt, s) != sl::Result::eOk || s.optimalRenderWidth == 0) return false;
    renderW = s.optimalRenderWidth;
    renderH = s.optimalRenderHeight;
    return true;
}

bool evaluate(void* cmd, const NativeImage images[4], const UpscaleInputs& in, uint64_t frame) {
    if (!dlssAvailable() || in.mode == UpscaleMode::Off) return false;
    const sl::FrameToken* token = tokenFor(frame);
    if (!token) return false;
    const sl::ViewportHandle viewport{0u};

    sl::DLSSOptions opt{};
    opt.mode = toDlss(in.mode);
    opt.outputWidth = images[3].width;
    opt.outputHeight = images[3].height;
    opt.colorBuffersHDR = sl::Boolean::eTrue;
    if (g.dlssSetOptions(viewport, opt) != sl::Result::eOk) return false;

    sl::Constants c{};
    toSl(in.viewToClip, c.cameraViewToClip);
    toSl(in.clipToView, c.clipToCameraView);
    toSl(in.clipToPrevClip, c.clipToPrevClip);
    toSl(in.prevClipToClip, c.prevClipToClip);
    for (int r = 0; r < 4; ++r) c.clipToLensClip[r] = sl::float4(r == 0, r == 1, r == 2, r == 3);
    c.jitterOffset = sl::float2(in.jitterX, in.jitterY);
    c.mvecScale = sl::float2(1.0f, 1.0f);  // motion is in uv units
    c.cameraPinholeOffset = sl::float2(0, 0);
    c.cameraPos = sl::float3(0, 0, 0);     // matrices are camera-relative
    c.cameraUp = sl::float3(in.cameraUp[0], in.cameraUp[1], in.cameraUp[2]);
    c.cameraRight = sl::float3(in.cameraRight[0], in.cameraRight[1], in.cameraRight[2]);
    c.cameraFwd = sl::float3(in.cameraForward[0], in.cameraForward[1], in.cameraForward[2]);
    c.cameraNear = in.nearZ;
    c.cameraFar = in.farZ;
    c.cameraFOV = in.fovY;
    c.cameraAspectRatio = in.aspect;
    c.depthInverted = sl::Boolean::eTrue;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = in.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    if (g.setConstants(c, *token, viewport) != sl::Result::eOk) return false;

    static const sl::BufferType kTypes[4] = {sl::kBufferTypeScalingInputColor, sl::kBufferTypeDepth, sl::kBufferTypeMotionVectors,
                                             sl::kBufferTypeScalingOutputColor};
    sl::Resource res[4];
    sl::ResourceTag tags[4];
    sl::Extent ext[4];
    for (int i = 0; i < 4; ++i) {
        const NativeImage& n = images[i];
        res[i] = sl::Resource(sl::ResourceType::eTex2d, n.resource, n.memory, n.view, n.state);
        res[i].width = n.width;
        res[i].height = n.height;
        res[i].nativeFormat = n.nativeFormat;
        res[i].mipLevels = 1;
        res[i].arrayLayers = 1;
        res[i].usage = n.usage;
        ext[i] = sl::Extent{0, 0, i == 3 ? n.width : in.renderWidth, i == 3 ? n.height : in.renderHeight};
        tags[i] = sl::ResourceTag(&res[i], kTypes[i], sl::ResourceLifecycle::eValidUntilPresent, &ext[i]);
    }
    auto* cb = static_cast<sl::CommandBuffer*>(cmd);
    if (g.setTagForFrame(*token, viewport, tags, 4, cb) != sl::Result::eOk) return false;
    const sl::BaseStructure* inputs[] = {&viewport};
    return g.evaluate(sl::kFeatureDLSS, *token, inputs, 1, cb) == sl::Result::eOk;
}

void setLatencyMode(LatencyMode mode) {
    if (!reflexAvailable()) return;
    sl::ReflexOptions o{};
    o.mode = mode == LatencyMode::Off ? sl::ReflexMode::eOff : (mode == LatencyMode::On ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eLowLatencyWithBoost);
    o.useMarkersToOptimize = true;
    g.reflexSetOptions(o);
}

void latencySleep(uint64_t frame) {
    if (!reflexAvailable()) return;
    if (const sl::FrameToken* t = tokenFor(frame)) g.reflexSleep(*t);
}

void latencyMarker(LatencyMarker marker, uint64_t frame) {
    if (!reflexAvailable()) return;
    if (const sl::FrameToken* t = tokenFor(frame)) g.pclMarker(sl::PCLMarker(uint32_t(marker)), *t);
}

}  // namespace taelia::rhi::streamline
