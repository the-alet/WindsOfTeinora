#include <cstdio>

#include "rhi/backends.h"

namespace taelia::rhi {


uint32_t formatBytesPerTexel(Format f) {
    switch (f) {
        case Format::RGBA8_UNORM:
        case Format::RGBA8_SRGB:
        case Format::BGRA8_UNORM:
        case Format::RGB10A2_UNORM:
        case Format::R11G11B10_FLOAT:
        case Format::RG16_FLOAT:
        case Format::R32_FLOAT:
        case Format::R32_UINT:
        case Format::D32_FLOAT: return 4;
        case Format::RGBA16_FLOAT: return 8;
        case Format::RGBA32_FLOAT: return 16;
        case Format::R16_FLOAT:
        case Format::RG8_UNORM: return 2;
        case Format::R8_UNORM: return 1;
        case Format::Unknown: break;
    }
    return 0;
}

bool isDepthFormat(Format f) { return f == Format::D32_FLOAT; }

const char* apiName(Api a) { return a == Api::D3D12 ? "D3D12" : "Vulkan"; }

bool loadShaderBinary(const std::string& shaderDir, const std::string& name, const char* extension,
                      std::vector<uint8_t>& out, std::string* error) {
    const std::string path = shaderDir + "/" + name + extension;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "cannot open shader " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? size_t(size) : 0);
    const size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (got != out.size() || out.empty()) {
        if (error) *error = "cannot read shader " + path;
        return false;
    }
    return true;
}

}  // namespace taelia::rhi
