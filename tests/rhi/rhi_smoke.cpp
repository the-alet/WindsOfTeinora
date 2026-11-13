// Headless smoke test of one RHI backend: compute -> storage image, bindless buffer/texture reads, frame and
// push constants, an indexed draw from the upload ring, depth, copies, deferred deletion, readback.
// Built twice: rhi_smoke_d3d12 and rhi_smoke_vulkan. Exit code 0 = pixels right and no debug-layer messages.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <windows.h>

#include "gpu_types.h"
#include "no_dialogs.h"
#include "rhi/backends.h"

#ifdef SMOKE_D3D12
extern "C" {
__declspec(dllexport) extern const unsigned D3D12SDKVersion = TAELIA_AGILITY_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
#endif

using namespace taelia;

namespace {
int g_fail = 0;

void expectPixel(const std::vector<uint8_t>& px, uint32_t w, uint32_t x, uint32_t y, const int want[4], const char* what) {
    const uint8_t* p = &px[(size_t(y) * w + x) * 4];
    bool ok = true;
    for (int c = 0; c < 4; ++c) ok &= std::abs(int(p[c]) - want[c]) <= 2;
    std::printf("  %s pixel (%u,%u) = %d %d %d %d, want %d %d %d %d: %s\n", what, x, y, p[0], p[1], p[2], p[3], want[0],
                want[1], want[2], want[3], ok ? "ok" : "WRONG");
    g_fail += ok ? 0 : 1;
}

std::string exeDir() {
    wchar_t buf[1024];
    GetModuleFileNameW(nullptr, buf, 1024);
    return std::filesystem::path(buf).parent_path().string();
}
}  // namespace

int main(int argc, char** argv) {
    taelia::sendErrorsToStderr();
    bool debug = true;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--no-debug") == 0) debug = false;

    rhi::DeviceDesc dd;
    dd.debug = debug;
    dd.streamline = true;
    dd.shaderDir = exeDir() + "/shaders";
    std::string err;
#ifdef SMOKE_D3D12
    dd.api = rhi::Api::D3D12;
    auto dev = rhi::createD3D12Device(dd, &err);
#else
    dd.api = rhi::Api::Vulkan;
    auto dev = rhi::createVulkanDevice(dd, &err);
#endif
    if (!dev) {
        std::printf("device creation failed: %s\n", err.c_str());
        return 2;
    }
    std::printf("%s on %s, debug layer %s\n", rhi::apiName(dev->api()), dev->adapterName().c_str(),
                dev->debugLayerActive() ? "active" : "OFF");

    const uint32_t W = 64, H = 64;
    const float bufData[4] = {0.25f, 0.0f, 0.0f, 0.0f};
    auto buf = dev->createBuffer({16, rhi::BufferUsage::Storage, rhi::Memory::GpuOnly, "smoke.buffer"}, bufData);
    const float quad[8] = {-1, -1, 0, -1, 0, 1, -1, 1};
    auto vb = dev->createBuffer({sizeof quad, rhi::BufferUsage::Storage, rhi::Memory::GpuOnly, "smoke.quad"}, quad);

    rhi::TextureDesc td;
    td.width = td.height = 4;
    td.format = rhi::Format::RGBA8_UNORM;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
    td.name = "smoke.texture";
    auto tex = dev->createTexture(td);
    uint8_t texels[4 * 4 * 4];
    for (int i = 0; i < 16; ++i) texels[i * 4 + 0] = 0, texels[i * 4 + 1] = 255, texels[i * 4 + 2] = 0, texels[i * 4 + 3] = 255;
    dev->uploadTexture(tex.get(), texels, sizeof texels);

    rhi::TextureDesc sd;
    sd.width = W;
    sd.height = H;
    sd.format = rhi::Format::RGBA16_FLOAT;
    sd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
    sd.name = "smoke.storage";
    auto storage = dev->createTexture(sd);

    rhi::TextureDesc rd;
    rd.width = W;
    rd.height = H;
    rd.format = rhi::Format::RGBA8_UNORM;
    rd.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled | rhi::TextureUsage::CopySrc;
    rd.name = "smoke.target";
    auto rt = dev->createTexture(rd);
    rd.usage = rhi::TextureUsage::CopyDst | rhi::TextureUsage::CopySrc;
    rd.name = "smoke.copy";
    auto rtCopy = dev->createTexture(rd);
    rhi::TextureDesc dsd;
    dsd.width = W;
    dsd.height = H;
    dsd.format = rhi::Format::D32_FLOAT;
    dsd.usage = rhi::TextureUsage::DepthStencil;
    dsd.name = "smoke.depth";
    auto depth = dev->createTexture(dsd);

    rhi::GraphicsPipelineDesc gp;
    gp.vs = "smoke.VSMain";
    gp.ps = "smoke.PSMain";
    gp.colorFormats[0] = rhi::Format::RGBA8_UNORM;
    gp.colorCount = 1;
    gp.depthFormat = rhi::Format::D32_FLOAT;
    gp.depthTest = gp.depthWrite = true;
    gp.name = "smoke.gfx";
    auto gfx = dev->createGraphicsPipeline(gp, &err);
    auto cs = dev->createComputePipeline({"smoke.CSMain", "smoke.cs"}, &err);
    if (!gfx || !cs) {
        std::printf("pipeline creation failed: %s\n", err.c_str());
        return 2;
    }

    struct Push {
        uint32_t bufferSrv, textureSrv, storageSrv, storageUav;
        float quadColor[4];
        uint32_t mode, vertexBuffer, pad[2];
    } push{buf->srv(), tex->srv(), storage->srv(), storage->uav(0), {1.0f, 0.5f, 0.0f, 1.0f}, 0, vb->srv(), {0, 0}};

    for (int frame = 0; frame < 6; ++frame) {
        rhi::CommandList* cmd = dev->beginFrame(nullptr);
        cmd->beginMarker("smoke");
        if (frame == 2) {  // deferred deletion while frames are in flight
            auto temp = dev->createBuffer({4096, rhi::BufferUsage::Storage, rhi::Memory::GpuOnly, "smoke.temp"});
            cmd->barrier(temp.get(), rhi::State::UnorderedAccess);
        }
        cmd->barrier(storage.get(), rhi::State::UnorderedAccess);
        cmd->setPipeline(cs.get());
        cmd->pushConstants(&push, sizeof push);
        cmd->dispatch(W / 8, H / 8);
        cmd->barrier(storage.get(), rhi::State::ShaderRead);

        cmd->barrier(rt.get(), rhi::State::RenderTarget);
        cmd->barrier(depth.get(), rhi::State::DepthWrite);
        rhi::RenderPass pass;
        pass.colorCount = 1;
        pass.color[0].texture = rt.get();
        pass.color[0].load = rhi::LoadOp::Clear;
        pass.color[0].clear[2] = pass.color[0].clear[3] = 1.0f;
        pass.depth.texture = depth.get();
        pass.depth.load = rhi::LoadOp::Clear;
        cmd->beginRenderPass(pass);

        rhi::Transient fc = dev->allocTransient(sizeof(gpu::FrameConstants));
        gpu::FrameConstants consts{};
        consts.ambient.w = 1.0f;
        std::memcpy(fc.cpu, &consts, sizeof consts);
        cmd->setPipeline(gfx.get());
        cmd->setFrameConstants(fc);
        push.mode = 0;
        cmd->pushConstants(&push, sizeof push);
        cmd->draw(3);

        rhi::Transient ib = dev->allocTransient(12);
        const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
        std::memcpy(ib.cpu, idx, sizeof idx);
        cmd->setIndexBuffer(ib.buffer, ib.offset, rhi::IndexType::U16);
        push.mode = 1;
        cmd->pushConstants(&push, sizeof push);
        cmd->drawIndexed(6);
        cmd->endRenderPass();

        cmd->barrier(rt.get(), rhi::State::CopySrc);
        cmd->barrier(rtCopy.get(), rhi::State::CopyDst);
        cmd->copyTexture(rtCopy.get(), rt.get());
        cmd->barrier(rt.get(), rhi::State::ShaderRead);
        cmd->endMarker();
        dev->endFrame(nullptr);
    }
    dev->waitIdle();

    std::vector<uint8_t> px;
    const int right[4] = {64, 255, 128, 255}, left[4] = {255, 128, 0, 255};
    for (auto* t : {rt.get(), rtCopy.get()}) {
        if (!dev->readbackRGBA8(t, px) || px.size() != size_t(W) * H * 4) {
            std::printf("  readback failed\n");
            ++g_fail;
            continue;
        }
        expectPixel(px, W, 48, 32, right, t == rt.get() ? "target" : "copy");
        expectPixel(px, W, 16, 32, left, t == rt.get() ? "target" : "copy");
    }
    // A second readback must still work (state restored).
    if (!dev->readbackRGBA8(rt.get(), px)) ++g_fail;

    // ---- Ray queries, GPU-written indirect arguments, indirect dispatch and draw.
    if (!dev->rayTracingSupported()) {
        std::printf("  ray tracing NOT supported\n");
        ++g_fail;
    } else {
        // A quad x in [-0.5, 0.5], y in [-0.5, 0.5] at z = 0; instance 7 opaque at x - 0.5, instance 9 non-opaque at x + 0.5.
        const float qv[12] = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0.5f, 0.5f, 0, -0.5f, 0.5f, 0};
        const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
        const uint32_t geomUsage = rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationInput;
        auto qvb = dev->createBuffer({sizeof qv, geomUsage, rhi::Memory::GpuOnly, "rt.vertices"}, qv);
        auto qib = dev->createBuffer({sizeof qi, geomUsage, rhi::Memory::GpuOnly, "rt.indices"}, qi);
        rhi::BlasDesc opaqueDesc;
        opaqueDesc.geometries.push_back({qvb.get(), 0, 12, 4, qib.get(), 0, 6, rhi::IndexType::U32, true});
        opaqueDesc.name = "rt.blas.opaque";
        rhi::BlasDesc alphaDesc = opaqueDesc;
        alphaDesc.geometries[0].opaque = false;
        alphaDesc.name = "rt.blas.alpha";
        auto blasA = dev->createBlas(opaqueDesc);
        auto blasB = dev->createBlas(alphaDesc);
        auto tlas = dev->createTlas(4, "rt.tlas");
        rhi::TlasInstance inst[2];
        inst[0].transform[3] = -0.5f;
        inst[0].customIndex = 7;
        inst[0].blas = blasA.get();
        inst[1].transform[3] = 0.5f;
        inst[1].customIndex = 9;
        inst[1].blas = blasB.get();

        rhi::TextureDesc od;
        od.width = od.height = 64;
        od.format = rhi::Format::RGBA8_UNORM;
        od.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::CopySrc;
        od.name = "rt.out";
        auto rtOut = dev->createTexture(od);
        auto args = dev->createBuffer({32, rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect, rhi::Memory::GpuOnly, "rt.args"});
        auto argsCs = dev->createComputePipeline({"smoke_rt.ArgsMain", "rt.args"}, &err);
        auto traceCs = dev->createComputePipeline({"smoke_rt.TraceMain", "rt.trace"}, &err);
        rd.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
        rd.name = "rt.drawTarget";
        auto drawTarget = dev->createTexture(rd);
        if (!argsCs || !traceCs || !blasA || !blasB || !tlas) {
            std::printf("  ray tracing setup failed: %s\n", err.c_str());
            ++g_fail;
        } else {
            for (int frame = 0; frame < 3; ++frame) {
                rhi::CommandList* cmd = dev->beginFrame(nullptr);
                cmd->barrier(qvb.get(), rhi::State::ShaderRead);
                cmd->barrier(qib.get(), rhi::State::ShaderRead);
                cmd->buildBlas(blasA.get(), opaqueDesc);
                cmd->buildBlas(blasB.get(), alphaDesc);
                cmd->buildTlas(tlas.get(), inst, 2);
                struct RtPush {
                    uint32_t tlas, outUav, argsUav, pad;
                } rp{tlas->srv(), rtOut->uav(0), args->uav(), 0};
                cmd->barrier(args.get(), rhi::State::UnorderedAccess);
                cmd->setPipeline(argsCs.get());
                cmd->pushConstants(&rp, sizeof rp);
                cmd->dispatch(1);
                cmd->barrier(args.get(), rhi::State::IndirectArgument);
                cmd->barrier(rtOut.get(), rhi::State::UnorderedAccess);
                cmd->setPipeline(traceCs.get());
                cmd->dispatchIndirect(args.get(), 0);

                cmd->barrier(drawTarget.get(), rhi::State::RenderTarget);
                cmd->barrier(depth.get(), rhi::State::DepthWrite);
                rhi::RenderPass dp;
                dp.colorCount = 1;
                dp.color[0].texture = drawTarget.get();
                dp.color[0].load = rhi::LoadOp::Clear;
                dp.depth.texture = depth.get();
                dp.depth.load = rhi::LoadOp::Clear;
                cmd->beginRenderPass(dp);
                rhi::Transient fc = dev->allocTransient(sizeof(gpu::FrameConstants));
                gpu::FrameConstants consts{};
                consts.ambient.w = 1.0f;
                std::memcpy(fc.cpu, &consts, sizeof consts);
                cmd->setPipeline(gfx.get());
                cmd->setFrameConstants(fc);
                push.mode = 0;
                cmd->pushConstants(&push, sizeof push);
                cmd->drawIndirect(args.get(), 16);
                cmd->endRenderPass();
                dev->endFrame(nullptr);
            }
            dev->waitIdle();
            const int red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255}, black[4] = {0, 0, 0, 255};
            if (dev->readbackRGBA8(rtOut.get(), px)) {
                expectPixel(px, 64, 16, 32, red, "ray query, opaque instance");
                expectPixel(px, 64, 48, 32, green, "ray query, non-opaque instance");
                expectPixel(px, 64, 32, 4, black, "ray query, miss");
            } else {
                ++g_fail;
            }
            if (dev->readbackRGBA8(drawTarget.get(), px)) expectPixel(px, 64, 48, 32, right, "indirect draw");
            else ++g_fail;
        }
    }

    // ---- DLSS Super Resolution and Reflex through Streamline (this machine has an RTX GPU: DLSS must work).
    if (!dev->upscalerSupported()) {
        std::printf("  DLSS NOT available\n");
        ++g_fail;
    } else {
        const uint32_t ow = 1280, oh = 720;
        uint32_t rw = 0, rh = 0;
        dev->upscaleRenderSize(rhi::UpscaleMode::Performance, ow, oh, rw, rh);
        std::printf("  DLSS Performance renders %ux%u for %ux%u\n", rw, rh, ow, oh);
        if (rw == 0 || rw >= ow || rh == 0 || rh >= oh) ++g_fail;
        auto make = [&](uint32_t w, uint32_t h, rhi::Format f, uint32_t usage, const char* name) {
            rhi::TextureDesc d;
            d.width = w;
            d.height = h;
            d.format = f;
            d.usage = usage;
            d.name = name;
            return dev->createTexture(d);
        };
        using namespace rhi::TextureUsage;
        auto color = make(rw, rh, rhi::Format::RGBA16_FLOAT, RenderTarget | Sampled, "dlss.color");
        auto motion = make(rw, rh, rhi::Format::RG16_FLOAT, RenderTarget | Sampled, "dlss.motion");
        auto ddepth = make(rw, rh, rhi::Format::D32_FLOAT, DepthStencil | Sampled, "dlss.depth");
        auto out = make(ow, oh, rhi::Format::RGBA16_FLOAT, Storage | Sampled, "dlss.output");
        auto out8 = make(ow, oh, rhi::Format::RGBA8_UNORM, Storage | CopySrc, "dlss.output8");
        auto convert = dev->createComputePipeline({"smoke_rt.ConvertMain", "dlss.convert"}, &err);
        rhi::UpscaleInputs in;
        in.mode = rhi::UpscaleMode::Performance;
        in.color = color.get();
        in.depth = ddepth.get();
        in.motion = motion.get();
        in.output = out.get();
        in.renderWidth = rw;
        in.renderHeight = rh;
        // A plain reverse-Z infinite projection (ours: clip = M * v) and its inverse; no camera motion.
        const float a = 1.0f / (0.5463f * (16.0f / 9.0f)), b = 1.0f / 0.5463f, n = 0.1f;
        const float proj[16] = {a, 0, 0, 0, 0, b, 0, 0, 0, 0, 0, n, 0, 0, 1, 0};
        const float inv[16] = {1 / a, 0, 0, 0, 0, 1 / b, 0, 0, 0, 0, 0, 1, 0, 0, 1 / n, 0};
        const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(in.viewToClip, proj, sizeof proj);
        std::memcpy(in.clipToView, inv, sizeof inv);
        std::memcpy(in.clipToPrevClip, id, sizeof id);
        std::memcpy(in.prevClipToClip, id, sizeof id);
        in.nearZ = n;
        in.fovY = 1.0f;
        in.aspect = 16.0f / 9.0f;
        const bool reflex = dev->latencySupported();
        if (reflex) dev->setLatencyMode(rhi::LatencyMode::On);
        bool ok = convert != nullptr;
        for (int frame = 0; frame < 6 && ok; ++frame) {
            rhi::CommandList* cmd = dev->beginFrame(nullptr);
            if (reflex) {
                dev->latencySleep();
                dev->latencyMarker(rhi::LatencyMarker::SimulationStart);
                dev->latencyMarker(rhi::LatencyMarker::SimulationEnd);
                dev->latencyMarker(rhi::LatencyMarker::RenderSubmitStart);
            }
            cmd->barrier(color.get(), rhi::State::RenderTarget);
            cmd->barrier(motion.get(), rhi::State::RenderTarget);
            cmd->barrier(ddepth.get(), rhi::State::DepthWrite);
            rhi::RenderPass rp;
            rp.colorCount = 2;
            rp.color[0].texture = color.get();
            rp.color[0].load = rhi::LoadOp::Clear;
            rp.color[0].clear[0] = 0.25f;
            rp.color[0].clear[1] = 0.5f;
            rp.color[0].clear[2] = 0.75f;
            rp.color[0].clear[3] = 1.0f;
            rp.color[1].texture = motion.get();
            rp.color[1].load = rhi::LoadOp::Clear;
            rp.depth.texture = ddepth.get();
            rp.depth.load = rhi::LoadOp::Clear;
            rp.depth.clear = 0.5f;
            cmd->beginRenderPass(rp);
            cmd->endRenderPass();
            cmd->barrier(color.get(), rhi::State::ShaderRead);
            cmd->barrier(motion.get(), rhi::State::ShaderRead);
            cmd->barrier(ddepth.get(), rhi::State::ShaderRead);
            cmd->barrier(out.get(), rhi::State::UnorderedAccess);
            in.jitterX = (frame % 2) ? 0.25f : -0.25f;
            in.jitterY = (frame % 3) ? 0.25f : -0.25f;
            in.reset = frame == 0;
            ok &= cmd->upscale(in);
            // Recording must carry on as before after the upscaler: a dispatch with our own bindings.
            cmd->barrier(out.get(), rhi::State::ShaderRead);
            cmd->barrier(out8.get(), rhi::State::UnorderedAccess);
            cmd->setPipeline(convert.get());
            const uint32_t cp[4] = {out->srv(), out8->uav(0), 0, 0};
            cmd->pushConstants(cp, sizeof cp);
            cmd->dispatch(ow / 8, oh / 8);
            if (reflex) dev->latencyMarker(rhi::LatencyMarker::RenderSubmitEnd);
            dev->endFrame(nullptr);
        }
        dev->waitIdle();
        if (!ok) {
            std::printf("  DLSS evaluate failed\n");
            ++g_fail;
        } else if (dev->readbackRGBA8(out8.get(), px)) {
            const int want[4] = {64, 128, 191, 255};
            const uint8_t* p = &px[(size_t(oh / 2) * ow + ow / 2) * 4];
            bool closeEnough = true;
            for (int c = 0; c < 3; ++c) closeEnough &= std::abs(int(p[c]) - want[c]) <= 16;
            std::printf("  DLSS output pixel = %d %d %d, want about %d %d %d: %s\n", p[0], p[1], p[2], want[0], want[1], want[2],
                        closeEnough ? "ok" : "WRONG");
            g_fail += closeEnough ? 0 : 1;
        } else {
            ++g_fail;
        }
        std::printf("  Reflex %s\n", reflex ? "available, low latency on" : "not available (headless runs may not have it)");
    }

    const uint32_t msgs = dev->debugMessageCount();
    if (debug && !dev->debugLayerActive()) std::printf("  WARNING: debug layer not active\n");
    if (msgs) std::printf("  %u debug-layer messages\n", msgs);
    const bool ok = g_fail == 0 && msgs == 0;
    std::printf("rhi_smoke %s: %s\n", rhi::apiName(dev->api()), ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
