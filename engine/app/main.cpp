// Wonders of Taelia: the renderer front end over the simulation library.
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>

#include "app/flags.h"
#include "no_dialogs.h"
#include "app/image_io.h"
#include "app/settings.h"
#include "app/ui.h"
#include "imgui.h"
#include "platform/window.h"
#include "render/renderer.h"
#include "rhi/rhi.h"
#include "taelia/shared_constants.h"
#include "taelia/sim/camera_rig.h"
#include "taelia/sim/world.h"

// The D3D12 Agility SDK next to the exe (bin/D3D12).
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = TAELIA_AGILITY_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace taelia {
namespace {

FILE* g_log = nullptr;

void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "%s\n", buf);
    std::fflush(stderr);
    if (g_log) {
        std::fprintf(g_log, "%s\n", buf);
        std::fflush(g_log);
    }
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

std::filesystem::path exeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

CameraInput readCameraInput(Window& w) {
    CameraInput in;
    if (!w.mouseCaptured()) return in;
    w.takeMouseDelta(in.mouseDx, in.mouseDy);
    in.forward = (w.keyDown('W') ? 1 : 0) - (w.keyDown('S') ? 1 : 0);
    in.right = (w.keyDown('D') ? 1 : 0) - (w.keyDown('A') ? 1 : 0);
    in.up = (w.keyDown('E') || w.keyDown(VK_SPACE) ? 1 : 0) - (w.keyDown('Q') || w.keyDown('C') ? 1 : 0);
    return in;
}

class App {
public:
    int run(const LaunchFlags& flags);

private:
    void placeCamera(const LaunchFlags& f);
    void handleKeys();
    RenderView makeView(double aspect, double realSeconds) const;

    std::filesystem::path dir_;
    GraphicsSettings settings_;
    std::unique_ptr<World> world_;
    CameraRig rig_;
    Window window_;
    std::unique_ptr<rhi::Device> device_;
    std::unique_ptr<rhi::Swapchain> swapchain_;
    std::unique_ptr<rhi::Texture> offscreen_;
    Renderer renderer_;
    Ui ui_;
    bool magicView_ = false;
    bool automated_ = false;
    int appliedDetail_ = -1;
    uint64_t frame_ = 0;
};

void App::placeCamera(const LaunchFlags& f) {
    // The spawn point stands on the ground: CAMERA_SPAWN.y is eye height above it.
    dvec3 pos{k::CAMERA_SPAWN[0], 0.0, k::CAMERA_SPAWN[2]};
    pos.y = world_->groundHeight(pos.x, pos.z) + k::CAMERA_SPAWN[1];
    double yaw = 0, pitch = 0;
    if (f.camera) {
        pos = {f.camera->x, f.camera->y, f.camera->z};
        yaw = f.camera->yawDeg * kDegToRad;
        pitch = f.camera->pitchDeg * kDegToRad;
    }
    if (f.ground) pos.y = world_->groundHeight(pos.x, pos.z) + *f.ground;
    rig_.place(pos, yaw, pitch);
}

void App::handleKeys() {
    Window& w = window_;
    if (w.keyPressed(VK_ESCAPE)) ui_.menu = ui_.menu == Menu::None ? Menu::Main : Menu::None;
    if (w.keyPressed(VK_OEM_2) || w.keyPressed(VK_DIVIDE)) ui_.menu = ui_.menu == Menu::World ? Menu::None : Menu::World;
    if (w.keyPressed(VK_OEM_PLUS) || w.keyPressed(VK_ADD)) world_->clock().faster();
    if (w.keyPressed(VK_OEM_MINUS) || w.keyPressed(VK_SUBTRACT)) world_->clock().slower();
    if (w.keyPressed('M')) magicView_ = !magicView_;
}

RenderView App::makeView(double aspect, double realSeconds) const {
    RenderView v;
    v.cameraPos = rig_.position;
    v.forward = rig_.forward();
    v.right = rig_.right();
    v.up = rig_.up();
    v.fovY = CameraRig::fovY();
    const GazeScreen g = rig_.gazeOnScreen(aspect);
    v.gazeNdcX = g.ndcX;
    v.gazeNdcY = g.ndcY;
    v.hourOfDay = world_->clock().hourOfDay();
    v.simSeconds = world_->clock().seconds();
    v.realSeconds = realSeconds;
    v.timeSpeed = world_->clock().timeSpeed();
    v.wind = world_->wind();
    v.groundHeight = world_->groundHeight(rig_.position.x, rig_.position.z);
    v.magicView = magicView_;
    v.waitForStreaming = automated_;
    v.upscale = rhi::UpscaleMode(settings_.upscaler);
    v.sky = Sky::at(world_->clock().seconds());
    v.seed = world_->seed();
    v.frame = frame_;
    return v;
}

int App::run(const LaunchFlags& f) {
    dir_ = exeDir();
    const bool automated = f.automated();
    automated_ = automated;
    settings_.load((dir_ / "settings.ini").string());
    if (f.api) settings_.api = *f.api;  // the flag is the current choice (saved with the next settings change)
    if (f.upscaler) settings_.upscaler = *f.upscaler;
    if (f.latency) settings_.latency = *f.latency;
    const std::string api = settings_.api;
    const bool windowed = !automated || f.hiddenWindow;

    WorldDesc wd;
    wd.seed = f.seed;
    wd.startHour = f.hour ? *f.hour : k::DEFAULT_HOUR;
    wd.startDay = f.day;
    wd.settings.rain = f.rain;
    if (f.clouds) wd.settings.cloudCover = *f.clouds;
    world_ = std::make_unique<World>(wd);
    world_->clock().setTimeSpeed(f.timeSpeed);
    placeCamera(f);
    magicView_ = f.magic;

    if (windowed) {
        Window::enableDpiAwareness();
        // Automated runs with --hidden-window never show it, so they can't take focus.
        if (!window_.create(L"Wonders of Taelia", automated ? int(f.width) : 1600, automated ? int(f.height) : 900, !automated,
                            !automated)) {
            logf("cannot create the window");
            return 1;
        }
        if (settings_.fullscreen && !automated) window_.setBorderlessFullscreen(true);
    }

    rhi::DeviceDesc dd;
    dd.api = api == "vulkan" ? rhi::Api::Vulkan : rhi::Api::D3D12;
    dd.debug = f.gpuDebug;
    dd.shaderDir = (dir_ / "shaders").string();
    dd.streamline = true;  // DLSS and Reflex when the GPU and the DLLs allow
    std::string err;
    device_ = rhi::Device::create(dd, &err);
    if (!device_) {
        logf("cannot start %s: %s", api.c_str(), err.c_str());
        if (!automated) MessageBoxA(nullptr, err.c_str(), "Wonders of Taelia", MB_ICONERROR);
        return 1;
    }
    device_->setLatencyMode(rhi::LatencyMode(settings_.latency));
    logf("%s on %s%s", rhi::apiName(device_->api()), device_->adapterName().c_str(),
         f.gpuDebug ? (device_->debugLayerActive() ? ", debug layer on" : ", debug layer NOT available") : "");

    if (!windowed) {
        rhi::TextureDesc od;
        od.width = f.width;
        od.height = f.height;
        od.format = rhi::Format::RGBA8_UNORM;
        od.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc | rhi::TextureUsage::Sampled;
        od.name = "output";
        offscreen_ = device_->createTexture(od);
    } else {
        swapchain_ = device_->createSwapchain(window_.hwnd(), window_.width(), window_.height());
        if (!swapchain_) {
            logf("cannot create the swapchain");
            return 1;
        }
    }
    // The UI first: it creates the ImGui context the renderer's UI backend registers with.
    const float dpi = automated ? 1.0f : float(GetDpiForWindow(static_cast<HWND>(window_.hwnd()))) / 96.0f;
    ui_.init(automated ? nullptr : window_.hwnd(), dpi);
    if (!renderer_.init(device_.get(), &world_->terrain(), &world_->clouds(), settings_.detail, &err)) {
        logf("renderer: %s", err.c_str());
        return 1;
    }
    appliedDetail_ = settings_.detail;
    if (f.menu == "graphics") ui_.menu = Menu::Main, ui_.mainTab = 0;
    if (f.menu == "controls") ui_.menu = Menu::Main, ui_.mainTab = 1;
    if (f.menu == "world") ui_.menu = Menu::World;

    // --gaze puts the eye's focus at a point on screen (to check the blur in a frame).
    const double aspect0 = !windowed ? double(f.width) / f.height : double(window_.width()) / std::max(1u, window_.height());
    if (f.gazeX && f.gazeY) {
        const double tanY = std::tan(CameraRig::fovY() * 0.5);
        rig_.gazeYaw = rig_.yaw + std::atan(*f.gazeX * tanY * aspect0);
        rig_.gazePitch = rig_.pitch + std::atan(*f.gazeY * tanY);
    }

    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    const auto start = last;
    int exitCode = 0;
    for (;;) {
        // Reflex: wait here for the right moment to start the frame, then mark the work.
        device_->latencySleep();
        device_->latencyMarker(rhi::LatencyMarker::SimulationStart);
        double dt = 1.0 / 60.0;
        uint32_t outW = f.width, outH = f.height;
        if (windowed) {
            window_.beginInputFrame();
            if (!window_.pump()) break;
            if (window_.minimized()) {
                window_.setMouseCaptured(false);
                Sleep(16);
                last = clock::now();
                continue;
            }
            if (window_.takeResized()) swapchain_->resize(window_.width(), window_.height());
            outW = swapchain_->width();
            outH = swapchain_->height();
            if (!automated) {
                const auto now = clock::now();
                dt = std::min(std::chrono::duration<double>(now - last).count(), 0.25);
                last = now;
                handleKeys();
                window_.setMouseCaptured(window_.focused() && ui_.menu == Menu::None);
            }
        }
        const double aspect = double(outW) / double(outH);
        const double realSeconds = automated ? frame_ * (1.0 / 60.0) : std::chrono::duration<double>(clock::now() - start).count();

        world_->advance(dt);
        device_->latencyMarker(rhi::LatencyMarker::SimulationEnd);
        if (!automated) rig_.update(readCameraInput(window_), dt, aspect);
        else rig_.update(CameraInput{}, dt, aspect);

        // In automated runs the UI is laid out at the frame size; it never reads the real mouse.
        ui_.newFrame(outW, outH, dt, automated || window_.mouseCaptured());
        UiActions act = ui_.build(*world_, settings_, api, !f.noUi, device_->upscalerSupported(), device_->latencySupported());
        if (f.noUi) ui_.menu = Menu::None;
        ImDrawData* draw = ui_.render();
        if (act.exit) break;
        if (act.settingsChanged) {
            settings_.save((dir_ / "settings.ini").string());
            if (!automated) window_.setBorderlessFullscreen(settings_.fullscreen);
            device_->setLatencyMode(rhi::LatencyMode(settings_.latency));
            if (settings_.detail != appliedDetail_) {
                renderer_.setDetail(&world_->terrain(), settings_.detail);
                appliedDetail_ = settings_.detail;
            }
        }
        renderer_.imgui().updateTextures(draw);

        device_->latencyMarker(rhi::LatencyMarker::RenderSubmitStart);
        rhi::CommandList* cmd = device_->beginFrame(swapchain_.get());
        rhi::Texture* target = swapchain_ ? swapchain_->backbuffer() : offscreen_.get();
        if (target)  // no image while the swapchain is being rebuilt (resize, minimize)
            renderer_.render(cmd, target, makeView(aspect, realSeconds), f.noUi ? nullptr : draw, settings_.renderScale);
        device_->latencyMarker(rhi::LatencyMarker::RenderSubmitEnd);
        device_->endFrame(swapchain_.get());
        ++frame_;

        // --hidden-window: halfway through, resize the (never shown) window to exercise swapchain rebuilds.
        if (automated && windowed && frame_ == uint64_t(f.frames / 2))
            SetWindowPos(static_cast<HWND>(window_.hwnd()), nullptr, 0, 0, int(f.width * 3 / 4), int(f.height * 3 / 4),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (automated && windowed && frame_ >= uint64_t(f.frames)) {
            logf("presented %d frames to a hidden window, ending at %ux%u", f.frames, outW, outH);
            break;
        }
        if (automated && frame_ >= uint64_t(f.frames)) {
            device_->waitIdle();
            std::vector<uint8_t> px;
            if (!device_->readbackRGBA8(offscreen_.get(), px) || !writePng(f.shot, px, f.width, f.height)) {
                logf("cannot write %s", f.shot.c_str());
                exitCode = 1;
            } else {
                logf("wrote %s (%ux%u, %d frames)", f.shot.c_str(), f.width, f.height, f.frames);
            }
            break;
        }
    }

    device_->waitIdle();
    const uint32_t msgs = device_->debugMessageCount();
    if (msgs) {
        logf("%u debug-layer messages", msgs);
        if (exitCode == 0) exitCode = 3;
    }
    renderer_.shutdown();
    ui_.shutdown();
    offscreen_.reset();
    swapchain_.reset();
    device_.reset();
    window_.destroy();
    return exitCode;
}

}  // namespace
}  // namespace taelia

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    taelia::LaunchFlags flags;
    std::string error;
    const auto logPath = taelia::exeDir() / "taelia.log";
    taelia::g_log = _wfopen(logPath.c_str(), L"w");
    int code = 1;
    const bool parsed = taelia::parseFlags(argc, argv, flags, error);
    if (!parsed || flags.automated()) taelia::sendErrorsToStderr();  // automated runs never open a dialog
    if (!parsed) {
        taelia::logf("%s", error.c_str());
    } else {
        code = taelia::App().run(flags);
    }
    LocalFree(argv);
    if (taelia::g_log) std::fclose(taelia::g_log);
    return code;
}
