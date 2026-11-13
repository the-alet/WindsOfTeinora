#include "render/human_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "render/gbuffer.h"
#include "taelia/shared_constants.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

namespace taelia {

namespace {

constexpr uint32_t kHumanInstance = 0xFFFFFC;  // rt.hlsli HUMAN_INSTANCE

struct HumanPush {
    uint32_t vbSrv, bonesSrv, bonesOffset, vesselsSrv;
    uint32_t albedo, normal, flags, vesselCount;
    float place[4];  // feet relative to the camera, yaw
    float info[4];   // fill, heartbeat, seconds (wrapped), roughness
    float heart[4];  // xyz, specular
    float brain[4];
};

struct VertexGpu {  // 40 bytes, read by human.hlsl
    float pos[3];
    uint32_t normal;   // octahedral, 2 x unorm16
    uint32_t tangent;  // likewise
    float uv[2];
    uint32_t bones;    // 4 x u8
    uint32_t weights;  // 4 x unorm8
    uint32_t extra;    // sky seen (unorm8), bit 8: the tangent's handedness
};

uint32_t unorm8(double v) { return uint32_t(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

// Octahedral (gbuffer.hlsli decodeOctNormal: y is the octahedron's up axis).
uint32_t octNormal(double x, double y, double z) {
    const double s = std::fabs(x) + std::fabs(y) + std::fabs(z);
    if (s < 1e-12) return octNormal(0, 1, 0);
    x /= s;
    y /= s;
    z /= s;
    double ex = x, ez = z;
    if (y < 0) {
        ex = (1.0 - std::fabs(z)) * (x >= 0 ? 1.0 : -1.0);
        ez = (1.0 - std::fabs(x)) * (z >= 0 ? 1.0 : -1.0);
    }
    const uint32_t u = uint32_t(std::lround((ex * 0.5 + 0.5) * 65535.0)), v = uint32_t(std::lround((ez * 0.5 + 0.5) * 65535.0));
    return u | (v << 16);
}

}  // namespace

bool HumanRenderer::init(rhi::Device* device, std::string* error) {
    device_ = device;
    mesh_ = std::async(std::launch::async, [] { return &humanMesh(); });
    rhi::GraphicsPipelineDesc d;
    d.vs = "human.VSMain";
    d.ps = "human.PSMain";
    gbuffer::fillFormats(d);
    d.cull = rhi::Cull::None;
    d.name = "human";
    pipe_ = device_->createGraphicsPipeline(d, error);
    return pipe_ != nullptr;
}

uint32_t HumanRenderer::loadTexture(const std::string& path, bool color) {
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) return 0;
    rhi::TextureDesc d;
    d.width = uint32_t(w);
    d.height = uint32_t(h);
    d.mips = 1;
    while ((std::max(d.width, d.height) >> d.mips) > 0) ++d.mips;
    d.format = rhi::Format::RGBA8_UNORM;  // colours stay sRGB-encoded: human.hlsl decodes them
    d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
    d.name = color ? "human.albedo" : "human.normal";
    std::unique_ptr<rhi::Texture> t = device_->createTexture(d);
    if (!t) {
        stbi_image_free(px);
        return 0;
    }
    device_->uploadTexture(t.get(), px, size_t(w) * size_t(h) * 4);
    stbi_image_free(px);
    const uint32_t srv = t->srv();
    needMips_.push_back(t.get());
    textures_.push_back(std::move(t));
    return srv;
}

void HumanRenderer::upload(const HumanMesh& mesh) {
    if (!mesh.loaded) return;
    std::vector<VertexGpu> verts(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const HumanVertex& v = mesh.vertices[i];
        VertexGpu& g = verts[i];
        g.pos[0] = v.position.x;
        g.pos[1] = v.position.y;
        g.pos[2] = v.position.z;
        g.normal = octNormal(v.normal.x, v.normal.y, v.normal.z);
        g.tangent = octNormal(v.tangent[0], v.tangent[1], v.tangent[2]);
        g.uv[0] = v.uv[0];
        g.uv[1] = v.uv[1];
        g.bones = uint32_t(v.bone[0]) | uint32_t(v.bone[1]) << 8 | uint32_t(v.bone[2]) << 16 | uint32_t(v.bone[3]) << 24;
        // Weights in 255ths that still sum to one: the largest takes up the rounding.
        uint32_t w[4], sum = 0;
        for (int s = 0; s < 4; ++s) sum += (w[s] = unorm8(v.weight[s]));
        w[0] = uint32_t(std::max(0, int(w[0]) + 255 - int(sum)));
        g.weights = w[0] | w[1] << 8 | w[2] << 16 | w[3] << 24;
        g.extra = unorm8(v.occlusion) | (v.tangent[3] < 0 ? 256u : 0u);
    }
    vertexCount_ = uint32_t(verts.size());
    // Ray tracing sees it standing (the rest pose holds its arms out): its shadows fall from its own shape.
    {
        const HumanPose standing = humanIdlePose(0.0);
        std::vector<float> posed;
        posed.reserve(mesh.vertices.size() * 3);
        for (const HumanVertex& v : mesh.vertices) {
            const fvec3 p = humanSkin(standing, v);
            posed.insert(posed.end(), {p.x, p.y, p.z});
        }
        standing_ = device_->createBuffer({posed.size() * 4, rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationInput,
                                           rhi::Memory::GpuOnly, "human.standing"}, posed.data());
    }
    vertices_ = device_->createBuffer({verts.size() * sizeof(VertexGpu), rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationInput,
                                       rhi::Memory::GpuOnly, "human.vertices"}, verts.data());
    indices_ = device_->createBuffer({mesh.indices.size() * 4, rhi::BufferUsage::Index | rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationInput,
                                      rhi::Memory::GpuOnly, "human.indices"}, mesh.indices.data());
    // Its pieces with their textures: the opaque ones first.
    std::vector<uint32_t> albedo(mesh.materials.size()), normal(mesh.materials.size());
    for (size_t m = 0; m < mesh.materials.size(); ++m) {
        const HumanMaterial& mat = mesh.materials[m];
        albedo[m] = mat.albedo.empty() ? 0 : loadTexture(mesh.directory + "/" + mat.albedo, true);
        normal[m] = mat.normal.empty() ? 0 : loadTexture(mesh.directory + "/" + mat.normal, false);
    }
    for (int pass = 0; pass < 2; ++pass)
        for (const HumanSubmesh& s : mesh.submeshes) {
            const HumanMaterial& mat = mesh.materials[s.material];
            if (((mat.flags & kHumanAlpha) != 0) != (pass == 1)) continue;
            pieces_.push_back({s.firstIndex, s.indexCount, albedo[s.material], normal[s.material], mat.flags, mat.roughness, mat.specular});
        }
    // Its blood vessels for the magic view: a, distance from the heart there; b, the same (negative for a vein).
    std::vector<float> vs;
    for (const HumanVessel& v : mesh.vessels)
        vs.insert(vs.end(), {v.a.x, v.a.y, v.a.z, v.fromHeartA, v.b.x, v.b.y, v.b.z, v.artery ? v.fromHeartB : -v.fromHeartB});
    vesselCount_ = uint32_t(mesh.vessels.size());
    if (!vs.empty())
        vessels_ = device_->createBuffer({vs.size() * 4, rhi::BufferUsage::Storage, rhi::Memory::GpuOnly, "human.vessels"}, vs.data());
    heart_[0] = float(mesh.heart.x);
    heart_[1] = float(mesh.heart.y);
    heart_[2] = float(mesh.heart.z);
    brain_[0] = float(mesh.brain.x);
    brain_[1] = float(mesh.brain.y);
    brain_[2] = float(mesh.brain.z);
}

void HumanRenderer::shutdown() {
    if (mesh_.valid()) mesh_.wait();
    pipe_.reset();
    blas_.reset();
    vertices_.reset();
    indices_.reset();
    vessels_.reset();
    standing_.reset();
    textures_.clear();
    needMips_.clear();
    pieces_.clear();
}

std::vector<rhi::Texture*> HumanRenderer::takeTexturesNeedingMips() {
    std::vector<rhi::Texture*> out;
    out.swap(needMips_);
    return out;
}

void HumanRenderer::update(const dvec3& feet, double yaw, double held, double seconds, const dvec3& cam, const Frustum& f, bool wait) {
    if (mesh_.valid() && (wait || mesh_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) upload(*mesh_.get());
    if (!vertices_) {
        visible_ = nearEnough_ = false;
        return;
    }
    pose_ = humanIdlePose(seconds);
    heartbeat_ = humanHeartbeat(seconds);
    seconds_ = float(std::fmod(seconds, 3600.0));
    const dvec3 rel = feet - cam;
    place_[0] = float(rel.x);
    place_[1] = float(rel.y);
    place_[2] = float(rel.z);
    place_[3] = float(yaw);
    held_ = float(std::clamp(held, 0.0, 1.0));
    visible_ = f.boxVisible(rel - dvec3{0.6, 0.0, 0.6}, rel + dvec3{0.6, k::HUMAN_HEIGHT + 0.05, 0.6});
    nearEnough_ = length(rel) < 1000.0;
    const float c = std::cos(float(yaw)), s = std::sin(float(yaw));
    const float xf[12] = {c, 0, s, place_[0], 0, 1, 0, place_[1], -s, 0, c, place_[2]};
    std::memcpy(transform_, xf, sizeof xf);
}

void HumanRenderer::draw(rhi::CommandList* cmd, const rhi::Transient& fc) {
    if (!visible_ || !pipe_ || pose_.empty()) return;
    cmd->beginMarker("human");
    rhi::Transient bones = device_->allocTransient(pose_.size() * sizeof(HumanBoneMatrix));
    std::memcpy(bones.cpu, pose_.data(), pose_.size() * sizeof(HumanBoneMatrix));
    cmd->setIndexBuffer(indices_.get(), 0, rhi::IndexType::U32);
    cmd->setPipeline(pipe_.get());
    cmd->setFrameConstants(fc);
    for (const Piece& p : pieces_) {
        HumanPush push{vertices_->srv(), bones.buffer->srv(), uint32_t(bones.offset), vessels_ ? vessels_->srv() : 0u,
                       p.albedo, p.normal, p.flags, vessels_ ? vesselCount_ : 0u,
                       {place_[0], place_[1], place_[2], place_[3]}, {held_, float(heartbeat_), seconds_, p.roughness},
                       {heart_[0], heart_[1], heart_[2], p.specular}, {brain_[0], brain_[1], brain_[2], 0}};
        cmd->pushConstants(&push, sizeof push);
        cmd->drawIndexed(p.indexCount, 1, p.firstIndex);
    }
    cmd->endMarker();
}

void HumanRenderer::buildStructures(rhi::CommandList* cmd) {
    if (blas_ || !device_->rayTracingSupported() || !standing_) return;
    cmd->barrier(standing_.get(), rhi::State::ShaderRead);
    cmd->barrier(indices_.get(), rhi::State::ShaderRead);
    rhi::BlasDesc desc;
    for (const Piece& p : pieces_)
        if (!(p.flags & kHumanAlpha))
            desc.geometries.push_back({standing_.get(), 0, 12, vertexCount_, indices_.get(), uint64_t(p.firstIndex) * 4,
                                       p.indexCount, rhi::IndexType::U32, true});
    if (desc.geometries.empty()) return;
    desc.name = "human.blas";
    blas_ = device_->createBlas(desc);
    if (blas_) cmd->buildBlas(blas_.get(), desc);
}

void HumanRenderer::addInstances(std::vector<rhi::TlasInstance>& out) const {
    if (!blas_ || !nearEnough_) return;
    rhi::TlasInstance inst;
    std::memcpy(inst.transform, transform_, sizeof inst.transform);
    inst.customIndex = kHumanInstance;
    inst.forceOpaque = true;
    inst.blas = blas_.get();
    out.push_back(inst);
}

}  // namespace taelia
