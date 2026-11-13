#include "app/flags.h"

#include <cstdlib>
#include <vector>

namespace taelia {

namespace {

std::string narrow(const wchar_t* w) {
    std::string s;
    for (; *w; ++w) s.push_back(*w < 128 ? char(*w) : '?');
    return s;
}

bool toDouble(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && *end == 0;
}

bool toList(const std::string& s, std::vector<double>& out) {
    out.clear();
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        if (comma == std::string::npos) comma = s.size();
        double v;
        if (!toDouble(s.substr(start, comma - start), v)) return false;
        out.push_back(v);
        start = comma + 1;
    }
    return true;
}

}  // namespace

bool parseFlags(int argc, wchar_t** argv, LaunchFlags& f, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = narrow(argv[i]);
        const size_t eq = arg.find('=');
        const std::string key = arg.substr(0, eq);
        const std::string val = eq == std::string::npos ? "" : arg.substr(eq + 1);
        double d = 0;
        std::vector<double> list;
        auto bad = [&] {
            error = "bad value in " + arg;
            return false;
        };
        if (key == "--hour") {
            if (!toDouble(val, d)) return bad();
            f.hour = d;
        } else if (key == "--day") {
            if (!toDouble(val, d) || d < 0) return bad();
            f.day = int64_t(d);
        } else if (key == "--time-step") {
            if (!toDouble(val, d) || d <= 0) return bad();
            f.timeSpeed = d;
        } else if (key == "--camera") {
            if (!toList(val, list) || list.size() != 5) return bad();
            f.camera = CameraFlag{list[0], list[1], list[2], list[3], list[4]};
        } else if (key == "--ground") {
            if (!toDouble(val, d)) return bad();
            f.ground = d;
        } else if (key == "--magic") {
            f.magic = true;
        } else if (key == "--rain") {
            if (!toDouble(val, d) || d < 0 || d > 1) return bad();
            f.rain = d;
        } else if (key == "--clouds") {
            if (!toDouble(val, d) || d < 0 || d > 1) return bad();
            f.clouds = d;
        } else if (key == "--throws") {
            if (!toDouble(val, d) || d < 0) return bad();
            f.throws = int(d);
        } else if (key == "--frames") {
            if (!toDouble(val, d) || d < 1) return bad();
            f.frames = int(d);
        } else if (key == "--shot") {
            if (val.empty()) return bad();
            f.shot = val;
        } else if (key == "--api") {
            if (val != "d3d12" && val != "vulkan") return bad();
            f.api = val;
        } else if (key == "--gpu-debug") {
            f.gpuDebug = true;
        } else if (key == "--size") {
            const size_t x = val.find('x');
            double w = 0, h = 0;
            if (x == std::string::npos || !toDouble(val.substr(0, x), w) || !toDouble(val.substr(x + 1), h) || w < 16 ||
                h < 16 || w > 16384 || h > 16384)
                return bad();
            f.width = uint32_t(w);
            f.height = uint32_t(h);
        } else if (key == "--seed") {
            if (!toDouble(val, d) || d < 0) return bad();
            f.seed = uint64_t(d);
        } else if (key == "--dlss") {
            static const char* modes[] = {"off", "dlaa", "quality", "balanced", "performance", "ultra"};
            int m = -1;
            for (int k = 0; k < 6; ++k)
                if (val == modes[k]) m = k;
            if (m < 0) return bad();
            f.upscaler = m;
        } else if (key == "--reflex") {
            if (val != "off" && val != "on" && val != "boost") return bad();
            f.latency = val == "off" ? 0 : (val == "on" ? 1 : 2);
        } else if (key == "--hidden-window") {
            f.hiddenWindow = true;
        } else if (key == "--no-ui") {
            f.noUi = true;
        } else if (key == "--menu") {
            if (val != "graphics" && val != "controls" && val != "world") return bad();
            f.menu = val;
        } else if (key == "--gaze") {
            if (!toList(val, list) || list.size() != 2) return bad();
            f.gazeX = list[0];
            f.gazeY = list[1];
        } else {
            error = "unknown flag " + arg;
            return false;
        }
    }
    return true;
}

}  // namespace taelia
