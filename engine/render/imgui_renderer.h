// Draws Dear ImGui with the RHI (one pipeline per output format, vertices from the upload ring).
#pragma once
#include <memory>
#include <unordered_map>
#include <vector>

#include "rhi/rhi.h"

struct ImDrawData;
struct ImTextureData;

namespace taelia {

class ImGuiRenderer {
public:
    bool init(rhi::Device* device, std::string* error);
    void shutdown();
    // Creates / updates / destroys ImGui's textures. Call after ImGui::Render(), outside beginFrame/endFrame.
    void updateTextures(ImDrawData* data);
    // Draws into `target` (already in RenderTarget state, contents kept).
    void render(rhi::CommandList* cmd, rhi::Texture* target, ImDrawData* data);

private:
    rhi::Pipeline* pipelineFor(rhi::Format format);
    void updateTexture(ImTextureData* tex);

    rhi::Device* device_ = nullptr;
    std::unordered_map<int, std::unique_ptr<rhi::Pipeline>> pipelines_;  // by output format
    std::vector<std::unique_ptr<rhi::Texture>> textures_;
};

}  // namespace taelia
