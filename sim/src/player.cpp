#include "taelia/sim/player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

#include "taelia/shared_constants.h"
#include "taelia/sim/terrain.h"

namespace taelia {

namespace {

constexpr double kGravity = 9.81;

double easeTo(double v, double target, double rate, double dt) { return target + (v - target) * std::exp(-rate * dt); }

// A tree's trunk: its radius at the base and how tall the tree stands (m). Skeletons grow once per kind and variant.
struct Trunk {
    double radius = 0, height = 0;
};

Trunk trunkOf(const TreeInstance& t) {
    static std::array<std::array<Trunk, kTreeVariants>, size_t(TreeKind::Count)> sizes;
    static std::once_flag once;
    std::call_once(once, [] {
        for (int kind = 0; kind < int(TreeKind::Count); ++kind)
            for (int v = 0; v < kTreeVariants; ++v) {
                const TreeSkeleton s = growTree(TreeKind(kind), treeSkeletonSeed(TreeKind(kind), v));
                sizes[size_t(kind)][size_t(v)] = {double(s.trunkRadius), double(s.height)};
            }
    });
    const Trunk s = sizes[size_t(t.kind) % sizes.size()][size_t(t.variant) % size_t(kTreeVariants)];
    return {s.radius * t.scale, s.height * t.scale};
}

}  // namespace

void stepPlayer(Player& p, const PlayerInput& in, const Terrain& terrain, const std::vector<TreeInstance>& trees, double dt) {
    const dvec3 fwd{std::sin(in.yaw), 0, std::cos(in.yaw)}, right{std::cos(in.yaw), 0, -std::sin(in.yaw)};
    dvec3 wish = fwd * in.forward + right * in.right;
    if (length(wish) > 1.0) wish = normalize(wish);
    const TerrainSample here = terrain.sample(p.position.x, p.position.z);
    const double depthHere = here.underwater() ? here.waterLevel - here.height : 0.0;

    // F: takes off, or stops flying and falls.
    if (in.fly) {
        if (p.mode == PlayerMode::Fly) {
            p.mode = PlayerMode::Air;
        } else {
            p.mode = PlayerMode::Fly;
            p.velocity.y = std::max(p.velocity.y, 1.5);
        }
    }
    // Shift: a burst toward where it goes (or where it looks); on foot along the ground, in flight wherever it looks.
    p.dashWait = std::max(0.0, p.dashWait - dt);
    if (in.dash && p.dashWait <= 0.0) {
        dvec3 dir = length(wish) > 0.1 ? normalize(wish) : fwd;
        if (p.mode == PlayerMode::Fly) {
            const dvec3 look = directionFromAngles(in.yaw, in.pitch);
            const dvec3 w = look * in.forward + right * in.right;
            dir = length(w) > 0.1 ? normalize(w) : look;
        }
        const double speed = p.mode == PlayerMode::Swim ? k::PLAYER_DASH_SPEED * 0.35 : double(k::PLAYER_DASH_SPEED);
        if (p.mode == PlayerMode::Fly) {
            p.velocity = dir * speed;
        } else {
            p.velocity.x = dir.x * speed;
            p.velocity.z = dir.z * speed;
        }
        p.dashLeft = k::PLAYER_DASH_SECONDS;
        p.dashWait = k::PLAYER_DASH_COOLDOWN;
    }
    const bool dashing = p.dashLeft > 0.0;
    p.dashLeft = std::max(0.0, p.dashLeft - dt);

    auto steer = [&](double speed, double rate) {
        if (dashing) return;
        const double a = 1.0 - std::exp(-rate * dt);
        p.velocity.x += (wish.x * speed - p.velocity.x) * a;
        p.velocity.z += (wish.z * speed - p.velocity.z) * a;
    };
    switch (p.mode) {
        case PlayerMode::Ground: {
            double speed = in.crouch ? k::PLAYER_CROUCH_SPEED : k::PLAYER_SPEED;
            if (depthHere > 0.45) speed *= k::PLAYER_WADE;  // wading
            steer(speed, k::PLAYER_ACCEL);
            p.velocity.y = 0;
            if (in.jump && !p.jumpHeld) {
                p.velocity.y = k::PLAYER_JUMP_SPEED;
                p.mode = PlayerMode::Air;
                p.jumpHeld = true;
            }
            break;
        }
        case PlayerMode::Air:
            steer(k::PLAYER_SPEED, k::PLAYER_AIR_ACCEL);
            p.velocity.y -= kGravity * dt;
            break;
        case PlayerMode::Swim:
            steer(k::PLAYER_SWIM_SPEED, 3.0);
            p.velocity.y = 0;
            break;
        case PlayerMode::Fly: {
            const dvec3 look = directionFromAngles(in.yaw, in.pitch);
            dvec3 w = look * in.forward + right * in.right + dvec3{0, (in.jump ? 1.0 : 0.0) - (in.crouch ? 1.0 : 0.0), 0};
            if (length(w) > 1.0) w = normalize(w);
            if (!dashing) p.velocity += (w * double(k::PLAYER_FLY_SPEED) - p.velocity) * (1.0 - std::exp(-k::PLAYER_FLY_ACCEL * dt));
            break;
        }
    }
    if (!in.jump) p.jumpHeld = false;

    // Move, keeping out of trunks (it slides along them).
    dvec3 next = p.position + p.velocity * dt;
    for (const TreeInstance& t : trees) {
        const Trunk tr = trunkOf(t);
        if (next.y > t.position.y + tr.height * 0.6 || next.y + 1.8 < t.position.y) continue;
        const dvec3 d{next.x - t.position.x, 0, next.z - t.position.z};
        const double r = tr.radius + k::PLAYER_RADIUS, dist = length(d);
        if (dist >= r || dist < 1e-6) continue;
        const dvec3 nrm = d * (1.0 / dist);
        next.x = t.position.x + nrm.x * r;
        next.z = t.position.z + nrm.z * r;
        const double into = p.velocity.x * nrm.x + p.velocity.z * nrm.z;
        if (into < 0) {
            p.velocity.x -= nrm.x * into;
            p.velocity.z -= nrm.z * into;
        }
    }
    const TerrainSample at = terrain.sample(next.x, next.z);
    const double depth = at.underwater() ? at.waterLevel - at.height : 0.0;
    switch (p.mode) {
        case PlayerMode::Ground:
            if (depth > k::PLAYER_SWIM_DEPTH) {
                p.mode = PlayerMode::Swim;
                next.y = at.waterLevel;
            } else if (next.y - at.height > 0.45) {
                p.mode = PlayerMode::Air;  // off an edge: it falls
            } else {
                next.y = at.height;  // down a step or a slope it follows the ground
            }
            break;
        case PlayerMode::Air:
            if (depth > k::PLAYER_SWIM_DEPTH && next.y <= at.waterLevel) {
                p.mode = PlayerMode::Swim;
                next.y = at.waterLevel;
                p.velocity.y = 0;
            } else if (next.y <= at.height) {
                p.land = std::clamp(-p.velocity.y / 7.0, 0.0, 1.0);
                next.y = at.height;
                p.velocity.y = 0;
                p.mode = PlayerMode::Ground;
            }
            break;
        case PlayerMode::Swim:
            if (depth < k::PLAYER_SWIM_DEPTH - 0.15) {
                p.mode = PlayerMode::Ground;
                next.y = at.height;
            } else {
                next.y = at.waterLevel;
            }
            break;
        case PlayerMode::Fly: {
            const double floor = std::max(at.height, at.underwater() ? at.waterLevel : -1e30);
            if (next.y < floor) {
                next.y = floor;
                p.velocity.y = std::max(p.velocity.y, 0.0);
            }
            break;
        }
    }
    p.position = next;

    // Its body turns toward where it goes (or where it looks while it aims), leaning into the turn.
    const double hs = std::hypot(p.velocity.x, p.velocity.z);
    double want = p.yaw;
    if (in.face) want = in.yaw;
    else if (hs > 0.4) want = std::atan2(p.velocity.x, p.velocity.z);
    const double before = p.yaw;
    p.yaw = wrapAngle(p.yaw + wrapAngle(want - p.yaw) * (1.0 - std::exp(-k::PLAYER_TURN_RATE * dt)));
    const double rate = wrapAngle(p.yaw - before) / std::max(dt, 1e-6);
    p.lean = easeTo(p.lean, std::clamp(rate * hs * 0.06, -0.5, 0.5), 6.0, dt);
    p.lookYaw = wrapAngle(in.yaw - p.yaw);
    p.lookPitch = in.pitch;

    // The pose's weights ease in and out.
    p.crouch = easeTo(p.crouch, p.mode == PlayerMode::Ground && in.crouch ? 1.0 : 0.0, 9.0, dt);
    p.air = easeTo(p.air, p.mode == PlayerMode::Air ? 1.0 : 0.0, 10.0, dt);
    p.swim = easeTo(p.swim, p.mode == PlayerMode::Swim ? 1.0 : 0.0, 4.0, dt);
    p.fly = easeTo(p.fly, p.mode == PlayerMode::Fly ? 1.0 : 0.0, 4.0, dt);
    p.dash = easeTo(p.dash, dashing ? 1.0 : 0.0, dashing ? 25.0 : 6.0, dt);
    p.land = std::max(0.0, p.land - dt * 3.5);
    p.form = easeTo(p.form, in.gather ? 1.0 : 0.0, 8.0, dt);
    if (p.toss >= 0.0) {
        p.toss += dt;
        if (p.toss > 1.0) p.toss = -1.0;
    }
    if (p.mode == PlayerMode::Ground) p.gait += hs / humanGaitCycle(hs) * dt;
}

HumanMotion playerMotion(const Player& p, double seconds, double ball) {
    HumanMotion m;
    m.seconds = seconds;
    m.gait = p.gait;
    const double hs = std::hypot(p.velocity.x, p.velocity.z), all = length(p.velocity);
    m.speed = hs;
    m.crouch = p.crouch;
    m.air = p.air;
    m.rise = p.velocity.y;
    m.land = p.land;
    m.swim = p.swim;
    m.swimSpeed = hs;
    m.fly = p.fly;
    m.flySpeed = all;
    m.flyClimb = all > 0.5 ? std::clamp(p.velocity.y / all, -1.0, 1.0) : 0.0;
    m.dash = p.dash;
    m.form = p.form;
    m.ball = ball;
    m.toss = p.toss;
    m.lookYaw = p.lookYaw;
    m.lookPitch = p.lookPitch;
    m.lean = p.lean;
    return m;
}

dvec3 playerToWorld(const Player& p, const dvec3& m) {
    const double c = std::cos(p.yaw), s = std::sin(p.yaw);
    return p.position + dvec3{m.x * c + m.z * s, m.y, -m.x * s + m.z * c};
}

dvec3 playerEyes(const Player& p, const HumanPose& pose) {
    const dvec3 eyes = humanEyes(pose);
    return playerToWorld(p, eyes + dvec3{0, 0, 0.07});  // just in front of the face: the eye sees out
}

dvec3 playerCamera(const Player& p, const Terrain& terrain, double yaw, double pitch) {
    // Round its head: lower crouching, near the surface swimming.
    const double head = (1.55 - 0.40 * p.crouch) * (1.0 - p.swim) + 0.35 * p.swim;
    const dvec3 pivot = p.position + dvec3{0, head, 0};
    const dvec3 look = directionFromAngles(yaw, pitch), right{std::cos(yaw), 0, -std::sin(yaw)};
    const dvec3 want = pivot - look * double(k::PLAYER_CAMERA_DISTANCE) + right * double(k::PLAYER_CAMERA_SIDE) + dvec3{0, k::PLAYER_CAMERA_RISE, 0};
    // Pulled in where a hill stands between it and the head; never under the ground.
    dvec3 cam = want;
    for (int i = 1; i <= 12; ++i) {
        const dvec3 q = pivot + (want - pivot) * (double(i) / 12.0);
        if (q.y < terrain.height(q.x, q.z) + 0.25) {
            cam = pivot + (want - pivot) * (double(i - 1) / 12.0);
            break;
        }
    }
    cam.y = std::max(cam.y, terrain.height(cam.x, cam.z) + 0.25);
    return cam;
}

}  // namespace taelia
