#include "render/magic_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gpu_types.h"
#include "taelia/shared_constants.h"

namespace taelia {

namespace {

constexpr int kSide = k::MAGIC_COLUMNS, kLayers = k::MAGIC_VIEW_LAYERS;

uint16_t toHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = int32_t((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffffu;
    if (exp <= 0) return uint16_t(sign);
    if (exp >= 31) return uint16_t(sign | 0x7bffu);
    mant += 0x1000u;
    if (mant & 0x800000u) return uint16_t(sign | (uint32_t(exp + 1) << 10));
    return uint16_t(sign | (uint32_t(exp) << 10) | (mant >> 13));
}

// An aspect's colour; water goes from light to deep blue as it thickens.
void aspectColor(int a, float rho, float out[3]) {
    static const float kColor[kAspects][3] = {
        {k::MAGIC_COLOR_EARTH[0], k::MAGIC_COLOR_EARTH[1], k::MAGIC_COLOR_EARTH[2]},
        {k::MAGIC_COLOR_WATER[0], k::MAGIC_COLOR_WATER[1], k::MAGIC_COLOR_WATER[2]},
        {k::MAGIC_COLOR_NATURE[0], k::MAGIC_COLOR_NATURE[1], k::MAGIC_COLOR_NATURE[2]},
        {k::MAGIC_COLOR_FIRE[0], k::MAGIC_COLOR_FIRE[1], k::MAGIC_COLOR_FIRE[2]},
        {k::MAGIC_COLOR_AIR[0], k::MAGIC_COLOR_AIR[1], k::MAGIC_COLOR_AIR[2]},
        {k::MAGIC_COLOR_LIGHTNING[0], k::MAGIC_COLOR_LIGHTNING[1], k::MAGIC_COLOR_LIGHTNING[2]},
        {k::MAGIC_COLOR_LIGHT[0], k::MAGIC_COLOR_LIGHT[1], k::MAGIC_COLOR_LIGHT[2]},
        {k::MAGIC_COLOR_NEUTRAL[0], k::MAGIC_COLOR_NEUTRAL[1], k::MAGIC_COLOR_NEUTRAL[2]},
    };
    const float deep = a == int(Aspect::Water) ? std::clamp(rho / 3.0f, 0.0f, 1.0f) : 0.0f;
    for (int i = 0; i < 3; ++i) out[i] = kColor[a][i] + (k::MAGIC_COLOR_WATER_DEEP[i] - kColor[a][i]) * deep;
}

}  // namespace

bool MagicRenderer::init(rhi::Device* device, std::string* error) {
    device_ = device;
    for (Window& w : win_) {
        rhi::TextureDesc d;
        d.width = kSide;
        d.height = kSide;
        d.depthOrLayers = kLayers;
        d.dim = rhi::TextureDim::Tex3D;
        d.format = rhi::Format::RGBA16_FLOAT;
        d.usage = rhi::TextureUsage::Sampled;
        d.name = "magic.volume";
        w.volume = device->createTexture(d);
        d.name = "magic.flow";
        w.flow = device->createTexture(d);
        if (!w.volume || !w.flow) {
            if (error) *error = "magic: could not create the volumes";
            return false;
        }
    }
    texels_.assign(size_t(kSide) * kSide * kLayers * 4, 0);
    flowTexels_.assign(texels_.size(), 0);
    return true;
}

void MagicRenderer::shutdown() {
    for (Window& w : win_) {
        w.volume.reset();
        w.flow.reset();
    }
    device_ = nullptr;
}

void MagicRenderer::update(const MagicField* field, const dvec3& camera) {
    for (int l = 0; l < 2; ++l) {
        Window& w = win_[l];
        const MagicLevelView v = field ? field->view(l) : MagicLevelView{};
        if (!field || !field->started() || !v.state || v.columns != kSide) {
            w.valid = false;
            continue;
        }
        const int64_t k0 = int64_t(std::floor(camera.y / v.dy)) - kLayers / 2;
        if (w.valid && w.version == v.version && w.ox == v.originX && w.oz == v.originZ && w.k0 == k0) continue;
        w.version = v.version;
        w.ox = v.originX;
        w.oz = v.originZ;
        w.k0 = k0;
        w.dx = v.dx;
        w.dy = v.dy;
        const int cols = v.columns * v.columns, cells = cols * v.layers;
        for (int k = 0; k < kLayers; ++k)
            for (int j = 0; j < kSide; ++j)
                for (int i = 0; i < kSide; ++i) {
                    const int64_t X = v.originX + i, Z = v.originZ + j;
                    const int slot = int(((Z % kSide) + kSide) % kSide) * kSide + int(((X % kSide) + kSide) % kSide);
                    // Underground holds nothing; above the level's top the top cell carries on.
                    const int64_t local = k0 + k - v.columnBase[slot];
                    float rgb[3] = {0, 0, 0}, total = 0, u[3] = {0, 0, 0}, air = 0;
                    if (local >= 0) {
                        const int c = int(std::min<int64_t>(local, v.layers - 1)) * cols + slot;
                        for (int a = 0; a < kAspects; ++a) {
                            const float rho = v.state[size_t(a) * cells + c];
                            float col[3];
                            aspectColor(a, rho, col);
                            for (int e = 0; e < 3; ++e) rgb[e] += col[e] * rho;
                            total += rho;
                        }
                        for (int e = 0; e < 3; ++e) u[e] = v.state[size_t(MagicField::kU + e) * cells + c];
                        air = v.state[size_t(Aspect::Air) * cells + c];
                    }
                    uint16_t* fl = &flowTexels_[(size_t(k) * kSide * kSide + size_t(j) * kSide + i) * 4];
                    fl[0] = toHalf(u[0]);
                    fl[1] = toHalf(u[1]);
                    fl[2] = toHalf(u[2]);
                    fl[3] = toHalf(air);
                    uint16_t* t = &texels_[(size_t(k) * kSide * kSide + size_t(j) * kSide + i) * 4];
                    t[0] = toHalf(rgb[0]);
                    t[1] = toHalf(rgb[1]);
                    t[2] = toHalf(rgb[2]);
                    t[3] = toHalf(total);
                }
        device_->uploadTexture(w.volume.get(), texels_.data(), texels_.size() * sizeof(uint16_t));
        device_->uploadTexture(w.flow.get(), flowTexels_.data(), flowTexels_.size() * sizeof(uint16_t));
        w.valid = true;
    }
}

void MagicRenderer::frameData(gpu::FrameConstants& fc, const dvec3& camera) const {
    for (int l = 0; l < 2; ++l) {
        const Window& w = win_[l];
        fc.magicWindow[l] = {float(double(w.ox) * w.dx - camera.x), float(double(w.k0) * w.dy - camera.y),
                             float(double(w.oz) * w.dx - camera.z), float(w.dx)};
    }
    fc.magicInfo = {float(win_[0].dy), float(win_[1].dy), float(k::MAGIC_VIEW_REACH), win_[0].valid && win_[1].valid ? 1.0f : 0.0f};
    fc.magicVolume = {win_[0].volume ? win_[0].volume->srv() : 0u, win_[1].volume ? win_[1].volume->srv() : 0u,
                      win_[0].flow ? win_[0].flow->srv() : 0u, win_[1].flow ? win_[1].flow->srv() : 0u};
}

}  // namespace taelia
