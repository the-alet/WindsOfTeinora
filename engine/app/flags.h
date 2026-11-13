// Launch flags (SPEC.md, architecture). With --frames the game renders headless, writes --shot and quits.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace taelia {

struct CameraFlag {
    double x = 0, y = 0, z = 0, yawDeg = 0, pitchDeg = 0;
};

struct LaunchFlags {
    std::optional<double> hour;        // --hour=9.5
    int64_t day = 0;                   // --day=N: start on day N (eclipses: day 15 at noon, then every 30/45/90 days)
    double timeSpeed = 1.0;            // --time-step=1 (x1 by default)
    std::optional<CameraFlag> camera;  // --camera=x,y,z,yaw,pitch (degrees)
    std::optional<double> ground;      // --ground=<m above ground>
    bool magic = false;                // --magic
    double rain = 0.0;                 // --rain=0..1
    std::optional<double> clouds;      // --clouds=0..1: a fixed cloud cover (else the natural weather)
    int throws = 0;                    // --throws=N
    // --inject=aspect,x,y,z,kg (repeatable, checks only): add kg/m3 of an aspect at (x, height above ground y, z)
    // once the magic field runs, so frames can show what dense magic does. No key does this.
    struct Injection {
        int aspect = 0;
        double x = 0, y = 0, z = 0, kg = 0;
    };
    std::vector<Injection> injects;
    int frames = 0;                    // --frames=N: automated run
    std::string shot = "shot.png";     // --shot=<file>
    std::optional<std::string> api;    // --api=d3d12|vulkan (else the saved setting)
    bool gpuDebug = false;             // --gpu-debug: D3D12 debug layer / Vulkan validation
    uint32_t width = 1920, height = 1080;  // --size=WxH: headless frame size
    uint64_t seed = 1;                 // --seed=N
    bool noUi = false;                 // --no-ui: no overlay or menus in the frame
    bool hiddenWindow = false;         // --hidden-window: with --frames, present to a window that is never shown
    std::optional<int> upscaler;       // --dlss=off|dlaa|quality|balanced|performance|ultra
    std::optional<int> latency;        // --reflex=off|on|boost
    std::string menu;                  // --menu=graphics|controls|world: open a menu (to check it in a frame)
    std::optional<double> gazeX, gazeY;  // --gaze=x,y: the eye's focus on screen, ndc -1..1 (to check the blur)

    bool automated() const { return frames > 0; }
};

// Parses the command line (UTF-16 from the OS). Returns false and fills `error` on a bad flag.
bool parseFlags(int argc, wchar_t** argv, LaunchFlags& out, std::string& error);

}  // namespace taelia
