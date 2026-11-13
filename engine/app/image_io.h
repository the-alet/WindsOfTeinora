#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace taelia {
// Writes tightly packed RGBA8 as an RGB PNG (creates the folder if needed).
bool writePng(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height);
}  // namespace taelia
