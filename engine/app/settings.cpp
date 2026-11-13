#include "app/settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace taelia {

const char* detailName(int detail) {
    static const char* names[] = {"Low", "Medium", "High", "Highest"};
    return names[std::clamp(detail, 0, 3)];
}

bool GraphicsSettings::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (key == "api" && (val == "d3d12" || val == "vulkan")) api = val;
        if (key == "detail") detail = std::clamp(std::atoi(val.c_str()), 0, 3);
        if (key == "render_scale") renderScale = std::clamp(float(std::atof(val.c_str())), 0.5f, 1.0f);
        if (key == "fullscreen") fullscreen = val == "1";
        if (key == "upscaler") upscaler = std::clamp(std::atoi(val.c_str()), 0, 5);
        if (key == "latency") latency = std::clamp(std::atoi(val.c_str()), 0, 2);
    }
    return true;
}

bool GraphicsSettings::save(const std::string& path) const {
    std::ofstream out(path);
    if (!out) return false;
    out << "api=" << api << "\ndetail=" << detail << "\nrender_scale=" << renderScale << "\nfullscreen=" << (fullscreen ? 1 : 0)
        << "\n";
    return bool(out);
}

}  // namespace taelia
