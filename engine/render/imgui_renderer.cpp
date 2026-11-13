#include "render/imgui_renderer.h"

#include <algorithm>
#include <cstring>

#include "imgui.h"

namespace taelia {

namespace {
struct ImGuiPush {
    float scale[2];
    float translate[2];
    uint32_t vbSrv;
    uint32_t vbOffset;
    uint32_t texSrv;
    uint32_t pad;
};
static_assert(sizeof(ImDrawVert) == 20, "imgui.hlsl reads 20-byte vertices");
static_assert(sizeof(ImDrawIdx) == 2, "16-bit ImGui indices expected");
}  // namespace

bool ImGuiRenderer::init(rhi::Device* device, std::string* error) {
    device_ = device;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "taelia_rhi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    (void)error;
    return true;
}

void ImGuiRenderer::shutdown() {
    if (!device_) return;
    if (ImGui::GetCurrentContext()) {
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
            if (tex->BackendUserData) {
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
                tex->BackendUserData = nullptr;
            }
        }
    }
    textures_.clear();
    pipelines_.clear();
    device_ = nullptr;
}

rhi::Pipeline* ImGuiRenderer::pipelineFor(rhi::Format format) {
    auto it = pipelines_.find(int(format));
    if (it != pipelines_.end()) return it->second.get();
    rhi::GraphicsPipelineDesc d;
    d.vs = "imgui.VSMain";
    d.ps = "imgui.PSMain";
    d.colorFormats[0] = format;
    d.colorCount = 1;
    d.blend[0] = rhi::Blend::Alpha;
    d.name = "imgui";
    std::string err;
    auto p = device_->createGraphicsPipeline(d, &err);
    rhi::Pipeline* raw = p.get();
    pipelines_[int(format)] = std::move(p);
    return raw;
}

void ImGuiRenderer::updateTexture(ImTextureData* tex) {
    if (tex->Status == ImTextureStatus_WantDestroy) {
        auto* t = static_cast<rhi::Texture*>(tex->BackendUserData);
        textures_.erase(std::remove_if(textures_.begin(), textures_.end(), [&](auto& p) { return p.get() == t; }), textures_.end());
        tex->BackendUserData = nullptr;
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
        return;
    }
    // Create, or re-upload the whole texture on updates (font atlas updates are rare).
    std::vector<uint8_t> rgba(size_t(tex->Width) * tex->Height * 4);
    if (tex->Format == ImTextureFormat_RGBA32) {
        std::memcpy(rgba.data(), tex->GetPixels(), rgba.size());
    } else {
        const uint8_t* a = static_cast<const uint8_t*>(tex->GetPixels());
        for (size_t i = 0; i < size_t(tex->Width) * tex->Height; ++i) {
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = a[i];
        }
    }
    auto* existing = static_cast<rhi::Texture*>(tex->BackendUserData);
    if (!existing || existing->width() != uint32_t(tex->Width) || existing->height() != uint32_t(tex->Height)) {
        if (existing)
            textures_.erase(std::remove_if(textures_.begin(), textures_.end(), [&](auto& p) { return p.get() == existing; }),
                            textures_.end());
        rhi::TextureDesc d;
        d.width = uint32_t(tex->Width);
        d.height = uint32_t(tex->Height);
        d.format = rhi::Format::RGBA8_UNORM;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        d.name = "imgui.texture";
        textures_.push_back(device_->createTexture(d));
        existing = textures_.back().get();
        tex->BackendUserData = existing;
    }
    device_->uploadTexture(existing, rgba.data(), rgba.size());
    tex->SetTexID(ImTextureID(existing->srv()));
    tex->SetStatus(ImTextureStatus_OK);
}

void ImGuiRenderer::updateTextures(ImDrawData* data) {
    if (!data || !data->Textures) return;
    for (ImTextureData* tex : *data->Textures)
        if (tex->Status != ImTextureStatus_OK) updateTexture(tex);
}

void ImGuiRenderer::render(rhi::CommandList* cmd, rhi::Texture* target, ImDrawData* data) {
    if (!data || data->TotalVtxCount == 0 || data->DisplaySize.x <= 0 || data->DisplaySize.y <= 0) return;
    rhi::Pipeline* pipe = pipelineFor(target->desc().format);
    if (!pipe) return;

    const uint64_t vbBytes = uint64_t(data->TotalVtxCount) * sizeof(ImDrawVert);
    const uint64_t ibBytes = (uint64_t(data->TotalIdxCount) * sizeof(ImDrawIdx) + 3) & ~3ull;
    rhi::Transient vb = device_->allocTransient(vbBytes);
    rhi::Transient ib = device_->allocTransient(ibBytes);
    if (!vb.cpu || !ib.cpu) return;
    auto* vdst = static_cast<uint8_t*>(vb.cpu);
    auto* idst = static_cast<uint8_t*>(ib.cpu);
    for (const ImDrawList* list : data->CmdLists) {
        std::memcpy(vdst, list->VtxBuffer.Data, size_t(list->VtxBuffer.Size) * sizeof(ImDrawVert));
        std::memcpy(idst, list->IdxBuffer.Data, size_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
        vdst += size_t(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        idst += size_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
    }

    // The target may be bigger than ImGui's display (headless frames): scale the UI to fit it.
    const float sx = float(target->width()) / data->DisplaySize.x, sy = float(target->height()) / data->DisplaySize.y;
    ImGuiPush push{};
    push.scale[0] = 2.0f / data->DisplaySize.x;
    push.scale[1] = -2.0f / data->DisplaySize.y;
    push.translate[0] = -1.0f - data->DisplayPos.x * push.scale[0];
    push.translate[1] = 1.0f - data->DisplayPos.y * push.scale[1];
    push.vbSrv = vb.buffer->srv();

    cmd->beginMarker("imgui");
    cmd->setPipeline(pipe);
    cmd->setViewport(0, 0, float(target->width()), float(target->height()));
    cmd->setIndexBuffer(ib.buffer, ib.offset, rhi::IndexType::U16);
    uint32_t globalVtx = 0, globalIdx = 0;
    for (const ImDrawList* list : data->CmdLists) {
        for (const ImDrawCmd& dc : list->CmdBuffer) {
            if (dc.UserCallback) {
                if (dc.UserCallback != ImDrawCallback_ResetRenderState) dc.UserCallback(list, &dc);
                continue;
            }
            const float x0 = (dc.ClipRect.x - data->DisplayPos.x) * sx, y0 = (dc.ClipRect.y - data->DisplayPos.y) * sy;
            const float x1 = (dc.ClipRect.z - data->DisplayPos.x) * sx, y1 = (dc.ClipRect.w - data->DisplayPos.y) * sy;
            const int32_t cx0 = std::max(0, int32_t(x0)), cy0 = std::max(0, int32_t(y0));
            const int32_t cx1 = std::min(int32_t(target->width()), int32_t(x1)), cy1 = std::min(int32_t(target->height()), int32_t(y1));
            if (cx1 <= cx0 || cy1 <= cy0) continue;
            cmd->setScissor(cx0, cy0, uint32_t(cx1 - cx0), uint32_t(cy1 - cy0));
            push.vbOffset = uint32_t(vb.offset + uint64_t(globalVtx + dc.VtxOffset) * sizeof(ImDrawVert));
            push.texSrv = uint32_t(dc.GetTexID());
            cmd->pushConstants(&push, sizeof push);
            cmd->drawIndexed(dc.ElemCount, 1, globalIdx + dc.IdxOffset);
        }
        globalVtx += uint32_t(list->VtxBuffer.Size);
        globalIdx += uint32_t(list->IdxBuffer.Size);
    }
    cmd->setScissor(0, 0, target->width(), target->height());
    cmd->endMarker();
}

}  // namespace taelia
