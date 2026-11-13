#include "app/image_io.h"

#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_WIDE  // paths are narrow ASCII here
#pragma warning(push, 0)
#include "stb_image_write.h"
#pragma warning(pop)

namespace taelia {

bool writePng(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
    if (rgba.size() < size_t(width) * height * 4) return false;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    // Drop alpha: frames are opaque.
    std::vector<uint8_t> rgb(size_t(width) * height * 3);
    for (size_t i = 0, n = size_t(width) * height; i < n; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    return stbi_write_png(path.c_str(), int(width), int(height), 3, rgb.data(), int(width * 3)) != 0;
}

}  // namespace taelia
