#include "taelia/sim/human.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "taelia/shared_constants.h"
#include "taelia/sim/hash.h"

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

// ---------------------------------------------------------------- posing

struct Mat3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}

Mat3 transpose(const Mat3& a) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    return r;
}

dvec3 apply(const Mat3& a, const dvec3& v) {
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z, a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z};
}

// Rotation by `angle` (radians) about a unit axis (right-handed).
Mat3 axisAngle(const dvec3& axis, double angle) {
    const dvec3 a = normalize(axis);
    const double c = std::cos(angle), s = std::sin(angle), t = 1.0 - c;
    Mat3 r;
    r.m[0][0] = t * a.x * a.x + c;
    r.m[0][1] = t * a.x * a.y - s * a.z;
    r.m[0][2] = t * a.x * a.z + s * a.y;
    r.m[1][0] = t * a.x * a.y + s * a.z;
    r.m[1][1] = t * a.y * a.y + c;
    r.m[1][2] = t * a.y * a.z - s * a.x;
    r.m[2][0] = t * a.x * a.z - s * a.y;
    r.m[2][1] = t * a.y * a.z + s * a.x;
    r.m[2][2] = t * a.z * a.z + c;
    return r;
}

// The shortest rotation taking direction u to direction v.
Mat3 between(const dvec3& u, const dvec3& v) {
    const dvec3 a = normalize(u), b = normalize(v);
    const dvec3 axis = cross(a, b);
    const double s = length(axis), c = std::clamp(dot(a, b), -1.0, 1.0);
    if (s < 1e-9) return Mat3{};
    return axisAngle(axis * (1.0 / s), std::atan2(s, c));
}

double hash01(uint64_t k) { return hashToUnit(mix64(k)); }

// How closed the eyes are (0 open .. 1 shut): a blink every three to six seconds, quick to close, slower to open.
double blinkAt(double t) {
    const double period = 4.2;
    const int64_t k = int64_t(std::floor(t / period));
    const double local = t - double(k) * period - (0.3 + 2.8 * hash01(uint64_t(k) * 7 + 3));
    if (local < 0.0 || local > 0.26) return 0.0;
    if (local < 0.07) return local / 0.07;
    if (local < 0.11) return 1.0;
    return 1.0 - (local - 0.11) / 0.15;
}

// Where the eyes look (yaw, pitch in radians): a new glance every second or two, reached in a few hundredths of a second.
void glanceAt(double t, double& yaw, double& pitch) {
    const double period = 1.7;
    const int64_t k = int64_t(std::floor(t / period));
    auto target = [](int64_t n, double& y, double& p) {
        y = (hash01(uint64_t(n) * 11 + 1) - 0.5) * 0.16;
        p = (hash01(uint64_t(n) * 13 + 5) - 0.5) * 0.07;
    };
    double y0, p0, y1, p1;
    target(k - 1, y0, p0);
    target(k, y1, p1);
    const double s = std::clamp((t - double(k) * period) / 0.045, 0.0, 1.0);
    yaw = y0 + (y1 - y0) * s;
    pitch = p0 + (p1 - p0) * s;
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
        m.loaded = true;
    });
    return mesh;
}

HumanPose humanIdlePose(double seconds) {
    const HumanMesh& mesh = humanMesh();
    const size_t n = mesh.joints.size();
    HumanPose pose(n);
    if (n == 0) return pose;
    // Each bone's turn about its pivot (its head unless said otherwise), in the rest pose's axes, and a scale of its
    // own skin (not passed on to its children).
    std::vector<Mat3> turn(n);
    std::vector<dvec3> pivot(n), scale(n, dvec3{1, 1, 1});
    for (size_t b = 0; b < n; ++b) pivot[b] = d3(mesh.joints[b].head);
    auto bone = [&](const std::string& name) { return mesh.bone(name.c_str()); };
    auto head = [&](const std::string& name) {
        const int b = bone(name);
        return b < 0 ? dvec3{} : d3(mesh.joints[size_t(b)].head);
    };
    auto set = [&](const std::string& name, const Mat3& r) {
        const int b = bone(name);
        if (b >= 0) turn[size_t(b)] = r;
    };

    const double breath = 0.5 - 0.5 * std::cos(2.0 * kPi * seconds / k::HUMAN_BREATH_SECONDS);  // 0 out .. 1 in
    // Breathing: the chest fills, the shoulders rise a little.
    if (const int b = bone("spine02"); b >= 0) scale[size_t(b)] = {1 + 0.008 * breath, 1 + 0.004 * breath, 1 + 0.016 * breath};
    if (const int b = bone("spine01"); b >= 0) scale[size_t(b)] = {1 + 0.012 * breath, 1 + 0.006 * breath, 1 + 0.024 * breath};
    set("spine01", axisAngle({1, 0, 0}, -0.006 * breath));
    set("clavicle.L", axisAngle({0, 0, 1}, 0.012 * breath));
    set("clavicle.R", axisAngle({0, 0, 1}, -0.012 * breath));
    // Its weight shifting from foot to foot; the head nodding and turning a little.
    set("spine05", axisAngle({0, 0, 1}, 0.004 * std::sin(2.0 * kPi * seconds / 9.7)));
    set("neck02", mul(axisAngle({1, 0, 0}, 0.012 * std::sin(2.0 * kPi * seconds / 7.3)), axisAngle({0, 1, 0}, 0.03 * std::sin(2.0 * kPi * seconds / 11.3))));

    // Arms hanging (the rest pose holds them out), elbows soft, hands turned to the thighs, fingers relaxed.
    std::vector<Mat3> global(n);  // so far: the turns down the chain to each bone
    auto globalOf = [&](const std::string& name) {
        Mat3 g;
        std::vector<int> chain;
        for (int b = bone(name); b >= 0; b = mesh.joints[size_t(b)].parent) chain.push_back(b);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) g = mul(g, turn[size_t(*it)]);
        return g;
    };
    auto aim = [&](const std::string& name, const dvec3& restDir, const dvec3& want) {
        const int b = bone(name);
        if (b < 0) return;
        const int p = mesh.joints[size_t(b)].parent;
        const Mat3 parentTurn = p >= 0 ? globalOf(mesh.joints[size_t(p)].name) : Mat3{};
        turn[size_t(b)] = between(restDir, apply(transpose(parentTurn), want));
    };
    for (const char* s : {"L", "R"}) {
        const double side = s[0] == 'L' ? 1.0 : -1.0;
        const std::string sx = std::string(".") + s;
        aim("upperarm01" + sx, head("lowerarm01" + sx) - head("upperarm01" + sx), {0.17 * side, -1.0, 0.02});
        aim("lowerarm01" + sx, head("wrist" + sx) - head("lowerarm01" + sx), {0.08 * side, -1.0, 0.24});
        // The palm faces down and in in the rest pose; fingers curl toward it.
        const dvec3 index = head("finger2-1" + sx), pinky = head("finger5-1" + sx), wrist = head("wrist" + sx);
        const dvec3 along = normalize((index + pinky) * 0.5 - wrist), across = pinky - index;
        dvec3 palm = normalize(cross(along, across));
        if (side > 0) palm = -palm;
        for (int f = 1; f <= 5; ++f)
            for (int j = 1; j <= 3; ++j) {
                const std::string name = "finger" + std::to_string(f) + "-" + std::to_string(j) + sx;
                const int b = bone(name);
                if (b < 0) continue;
                const dvec3 dir = d3(mesh.joints[size_t(b)].tail) - d3(mesh.joints[size_t(b)].head);
                const double curl = f == 1 ? 0.12 : (j == 1 ? 0.18 : (j == 2 ? 0.30 : 0.22)) * (1.0 + 0.12 * (f - 2));
                turn[size_t(b)] = axisAngle(cross(normalize(dir), palm), curl);
            }
    }

    // Eyes: glancing about; lids: blinking, turned about the eyes' centres.
    double yaw, pitch;
    glanceAt(seconds, yaw, pitch);
    const double blink = blinkAt(seconds);
    for (const char* s : {"L", "R"}) {
        const std::string sx = std::string(".") + s;
        const dvec3 eye = head("eye" + sx);
        set("eye" + sx, mul(axisAngle({0, 1, 0}, yaw), axisAngle({1, 0, 0}, -pitch)));
        for (const auto& [lid, angle] : {std::pair{"orbicularis03" + sx, 0.50 * blink - 0.3 * pitch}, std::pair{"orbicularis04" + sx, -0.10 * blink}}) {
            const int b = bone(lid);
            if (b < 0) continue;
            turn[size_t(b)] = axisAngle({1, 0, 0}, angle);
            pivot[size_t(b)] = eye;
        }
    }

    // Down the tree: each bone's turn about its pivot after its parent's; its skin with its own scale.
    std::vector<Mat3> rot(n);
    std::vector<dvec3> move(n);
    for (size_t b = 0; b < n; ++b) {
        const int p = mesh.joints[b].parent;
        const Mat3 pr = p >= 0 ? rot[size_t(p)] : Mat3{};
        const dvec3 pt = p >= 0 ? move[size_t(p)] : dvec3{};
        rot[b] = mul(pr, turn[b]);
        move[b] = apply(pr, pivot[b] - apply(turn[b], pivot[b])) + pt;
        // Skin: rot * diag(scale) about the bone's head.
        const dvec3 h = d3(mesh.joints[b].head), s = scale[b];
        const dvec3 t = apply(rot[b], h - dvec3{s.x * h.x, s.y * h.y, s.z * h.z}) + move[b];
        HumanBoneMatrix& out = pose[b];
        for (int row = 0; row < 3; ++row) {
            out.m[row * 4 + 0] = float(rot[b].m[row][0] * s.x);
            out.m[row * 4 + 1] = float(rot[b].m[row][1] * s.y);
            out.m[row * 4 + 2] = float(rot[b].m[row][2] * s.z);
        }
        out.m[3] = float(t.x);
        out.m[7] = float(t.y);
        out.m[11] = float(t.z);
    }
    return pose;
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
