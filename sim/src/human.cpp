#include "taelia/sim/human.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "taelia/shared_constants.h"

#ifndef TAELIA_ASSET_DIR
#define TAELIA_ASSET_DIR "assets"
#endif

namespace taelia {

namespace {

std::string& assetDir() {
    static std::string dir = std::string(TAELIA_ASSET_DIR) + "/human";
    return dir;
}

dvec3 d3(const fvec3& v) { return {v.x, v.y, v.z}; }
fvec3 f3(const dvec3& v) { return {float(v.x), float(v.y), float(v.z)}; }

// ---------------------------------------------------------------- reading assets/human/human.bin

struct Reader {
    std::vector<char> data;
    size_t at = 0;
    bool ok = true;

    template <typename T>
    T get() {
        T v{};
        if (at + sizeof(T) > data.size()) {
            ok = false;
            return v;
        }
        std::memcpy(&v, data.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    std::string text(size_t n) {
        if (at + n > data.size()) {
            ok = false;
            return {};
        }
        std::string s(data.data() + at, n);
        at += n;
        s.resize(std::strlen(s.c_str()));
        return s;
    }
    fvec3 vec() {
        const float x = get<float>(), y = get<float>(), z = get<float>();
        return {x, y, z};
    }
};

// ---------------------------------------------------------------- the blood vessels, from the skeleton

// The airways: from the nose and the mouth back to the throat, down the windpipe in front of the spine to where it
// forks behind the breastbone, the bronchi into each lung and their branches; the lungs either side of the heart.
void buildAirways(HumanMesh& m) {
    auto joint = [&](const char* name) -> dvec3 {
        const int b = m.bone(name);
        return b < 0 ? m.heart : d3(m.joints[size_t(b)].head);
    };
    auto add = [&](const std::vector<dvec3>& pts, float start) {
        float along = start;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const float len = float(length(pts[i + 1] - pts[i]));
            m.airways.push_back({f3(pts[i]), f3(pts[i + 1]), along, along + len, true});
            along += len;
        }
        return along;
    };
    const dvec3 head = joint("head"), neck = joint("neck01"), chest = joint("spine01");
    const dvec3 nose = head + dvec3{0, 0.02, 0.105}, mouth = head + dvec3{0, -0.035, 0.10};
    const dvec3 throat = head + dvec3{0, -0.04, 0.02};
    const dvec3 fork = chest + dvec3{0, 0.115, 0.035};
    add({mouth, throat}, float(length(nose - throat) - length(mouth - throat)));
    const float atThroat = add({nose, head + dvec3{0, 0.0, 0.05}, throat}, 0.0f);
    const float atFork = add({throat, neck + dvec3{0, 0.0, 0.05}, fork}, atThroat);
    for (int s = 0; s < 2; ++s) {
        const double side = s == 0 ? 1.0 : -1.0;
        const dvec3 lung = chest + dvec3{0.095 * side, 0.035, 0.005};
        m.lungs[s] = lung;
        const dvec3 hilum = lung + dvec3{-0.035 * side, 0.02, 0.0};
        const float atHilum = add({fork, hilum}, atFork);
        // Branches out through the lung: up, down, out, forward and back.
        const dvec3 tips[5] = {{0.01 * side, 0.10, 0.0}, {0.02 * side, -0.11, -0.02}, {0.06 * side, -0.01, 0.0},
                               {0.03 * side, -0.03, 0.06}, {0.03 * side, 0.02, -0.06}};
        for (const dvec3& t : tips) add({hilum, hilum + (lung + t - hilum) * 0.5, lung + t}, atHilum);
    }
}

void buildVessels(HumanMesh& m) {
    auto joint = [&](const char* name, bool head = true) -> dvec3 {
        const int b = m.bone(name);
        return b < 0 ? m.heart : d3(head ? m.joints[size_t(b)].head : m.joints[size_t(b)].tail);
    };
    // Polylines of arteries from points already reached (their distance from the heart known); the veins run beside
    // them, a little behind.
    auto add = [&](const std::vector<dvec3>& pts, float start) {
        float along = start;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const float len = float(length(pts[i + 1] - pts[i]));
            m.vessels.push_back({f3(pts[i]), f3(pts[i + 1]), along, along + len, true});
            const dvec3 behind{0, 0, -0.013};
            m.vessels.push_back({f3(pts[i] + behind), f3(pts[i + 1] + behind), along, along + len, false});
            along += len;
        }
        return along;
    };
    const dvec3 heart = m.heart, arch = heart + dvec3{-0.004, 0.055, -0.018};
    const float archAt = add({heart, arch}, 0.0f);
    const float bifAt = add({arch, joint("spine01") + dvec3{0, 0, 0.02}, joint("spine02") + dvec3{0, 0, 0.025},
                             joint("spine03") + dvec3{0, 0, 0.03}, joint("spine04") + dvec3{0, 0, 0.03}, joint("spine05") + dvec3{0, 0, 0.02}},
                            archAt);
    for (const char* s : {"L", "R"}) {
        const double side = s[0] == 'L' ? 1.0 : -1.0;
        auto j = [&](const std::string& base, bool head = true) { return joint((base + "." + s).c_str(), head); };
        // Down a leg to the forefoot.
        add({joint("spine05") + dvec3{0, 0, 0.02}, j("upperleg01"), j("upperleg02"), j("lowerleg01") + dvec3{0, 0, -0.02}, j("lowerleg02"),
             j("foot"), j("toe2-1")},
            bifAt);
        // Up the neck to the brain.
        add({arch, joint("neck01") + dvec3{0.022 * side, 0, 0.02}, joint("neck02") + dvec3{0.02 * side, 0, 0.018},
             joint("neck03") + dvec3{0.018 * side, 0, 0.012}, joint("head") + dvec3{0.015 * side, 0.015, 0.005}, m.brain},
            archAt);
        // Out along an arm to the hand.
        add({arch, j("clavicle") + dvec3{0.01 * side, 0, 0.01}, j("shoulder01"), j("upperarm01"), j("upperarm02"), j("lowerarm01"),
             j("lowerarm02"), j("wrist"), j("finger3-1")},
            archAt);
    }
}

}  // namespace

int HumanMesh::bone(const char* name) const {
    for (size_t i = 0; i < joints.size(); ++i)
        if (joints[i].name == name) return int(i);
    return -1;
}

void setHumanAssetDirectory(const std::string& dir) { assetDir() = dir; }

const HumanMesh& humanMesh() {
    static HumanMesh mesh;
    static std::once_flag once;
    std::call_once(once, [] {
        HumanMesh& m = mesh;
        m.directory = assetDir();
        std::ifstream f(m.directory + "/human.bin", std::ios::binary);
        if (!f) return;
        Reader r;
        r.data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (r.text(8) != "TAELHUM1" || r.get<uint32_t>() != 1) return;
        const uint32_t bones = r.get<uint32_t>();
        m.joints.resize(bones);
        for (HumanJoint& j : m.joints) {
            j.name = r.text(32);
            j.parent = r.get<int32_t>();
            j.head = r.vec();
            j.tail = r.vec();
        }
        m.heart = d3(r.vec());
        m.brain = d3(r.vec());
        const uint32_t materials = r.get<uint32_t>();
        m.materials.resize(materials);
        for (HumanMaterial& mat : m.materials) {
            mat.name = r.text(32);
            mat.albedo = r.text(64);
            mat.normal = r.text(64);
            mat.flags = r.get<uint32_t>();
            mat.roughness = r.get<float>();
            mat.specular = r.get<float>();
        }
        const uint32_t verts = r.get<uint32_t>();
        m.vertices.resize(verts);
        for (HumanVertex& v : m.vertices) {
            v.position = r.vec();
            v.normal = r.vec();
            for (float& t : v.tangent) t = r.get<float>();
            v.uv[0] = r.get<float>();
            v.uv[1] = r.get<float>();
            for (uint8_t& b : v.bone) b = r.get<uint8_t>();
            for (float& w : v.weight) w = float(r.get<uint8_t>()) / 255.0f;
            v.occlusion = r.get<float>();
            m.height = std::max(m.height, v.position.y);
        }
        const uint32_t subs = r.get<uint32_t>();
        m.submeshes.resize(subs);
        for (HumanSubmesh& s : m.submeshes) {
            s.material = r.get<uint32_t>();
            s.firstIndex = r.get<uint32_t>();
            s.indexCount = r.get<uint32_t>();
        }
        m.indices.resize(r.get<uint32_t>());
        for (uint32_t& i : m.indices) i = r.get<uint32_t>();
        if (!r.ok || m.joints.empty() || m.vertices.empty()) {
            m = HumanMesh{};
            return;
        }
        buildVessels(m);
        buildAirways(m);
        m.loaded = true;
    });
    return mesh;
}

fvec3 humanSkin(const HumanPose& pose, const HumanVertex& v) {
    double out[3] = {0, 0, 0};
    const double p[3] = {v.position.x, v.position.y, v.position.z};
    for (int s = 0; s < 4; ++s) {
        if (v.weight[s] <= 0 || v.bone[s] >= pose.size()) continue;
        const float* m = pose[v.bone[s]].m;
        for (int row = 0; row < 3; ++row)
            out[row] += v.weight[s] * (m[row * 4] * p[0] + m[row * 4 + 1] * p[1] + m[row * 4 + 2] * p[2] + m[row * 4 + 3]);
    }
    return {float(out[0]), float(out[1]), float(out[2])};
}

double humanLifeMagicAt(const dvec3& p) {
    const HumanMesh& m = humanMesh();
    const dvec3 h = (p - m.heart) * (1.0 / k::HUMAN_HEART_RADIUS);
    const dvec3 b{(p.x - m.brain.x) / k::HUMAN_BRAIN_RADII[0], (p.y - m.brain.y) / k::HUMAN_BRAIN_RADII[1], (p.z - m.brain.z) / k::HUMAN_BRAIN_RADII[2]};
    return k::MAGIC_HELD + (k::HUMAN_HEART_MAGIC - k::MAGIC_HELD) * std::exp(-dot(h, h)) +
           (k::HUMAN_BRAIN_MAGIC - k::MAGIC_HELD) * std::exp(-dot(b, b));
}

double humanHeartbeat(double seconds) {
    const double ph = seconds / k::HUMAN_HEARTBEAT_SECONDS - std::floor(seconds / k::HUMAN_HEARTBEAT_SECONDS);
    const auto knock = [&](double at) {
        const double d = (ph - at) / 0.055;
        return std::exp(-d * d);
    };
    return std::min(1.0, knock(0.0) + knock(1.0) + 0.65 * knock(0.30));
}

}  // namespace taelia
