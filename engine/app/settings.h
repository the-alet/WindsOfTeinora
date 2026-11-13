// Graphics settings, saved next to the exe as settings.ini.
#pragma once
#include <string>

namespace taelia {

struct GraphicsSettings {
    std::string api = "d3d12";   // d3d12 | vulkan (applies on restart)
    int detail = 2;              // 0 low, 1 medium, 2 high, 3 highest (more polygons at the highest)
    float renderScale = 1.0f;    // 0.5 .. 1
    bool fullscreen = false;     // borderless
    int upscaler = 2;            // 0 off, 1 DLAA, 2 quality, 3 balanced, 4 performance, 5 ultra performance (DLSS)
    int latency = 1;             // 0 off, 1 Reflex on, 2 Reflex on + boost

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

const char* detailName(int detail);

}  // namespace taelia
