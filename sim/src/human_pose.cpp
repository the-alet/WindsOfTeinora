// Posing the human: procedural, from what its body is doing (sim/human.h HumanMotion). Turns are about each bone's
// pivot in the rest pose's axes, after its parent's (rot = parentRot * turn); the root also carries the body's move.
// Legs and arms reach their targets by two-bone IK (hip-knee-ankle, shoulder-elbow-wrist), the forearm twisted so the
// palm faces where it should; feet stay flat while they stand on the ground. Targets for the arms are given in the
// rest pose's space round the chest and follow the chest; targets for the legs are on the ground (standing, walking,
// crouching, jumping) or round the hips (swimming, flying), blended by how much of each the body is doing.
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>

#include "taelia/shared_constants.h"
#include "taelia/sim/hash.h"
#include "taelia/sim/human.h"

namespace taelia {

namespace {

dvec3 d3(const fvec3& v) { return {v.x, v.y, v.z}; }

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

double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }
double smooth01(double x) {
    x = clamp01(x);
    return x * x * (3.0 - 2.0 * x);
}
dvec3 mix(const dvec3& a, const dvec3& b, double t) { return a + (b - a) * t; }
double wrap01(double x) { return x - std::floor(x); }

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

// The skeleton's named joints and the lengths IK needs, found once.
struct Rig {
    bool ok = false;
    int root = -1, spine[5] = {-1, -1, -1, -1, -1};  // spine05 (low) .. spine01 (chest)
    int neck[3] = {-1, -1, -1}, head = -1;
    int clavicle[2] = {-1, -1}, upperarm[2] = {-1, -1}, lowerarm[2] = {-1, -1}, twist[2] = {-1, -1}, wrist[2] = {-1, -1};
    int upperleg[2] = {-1, -1}, lowerleg[2] = {-1, -1}, foot[2] = {-1, -1};
    int eye[2] = {-1, -1}, lidUpper[2] = {-1, -1}, lidLower[2] = {-1, -1};
    int finger[2][5][3] = {};
    dvec3 shoulder[2], elbow[2], wristAt[2], hip[2], knee[2], ankle[2];  // rest joints
    double upperArm[2] = {}, forearm[2] = {}, thigh[2] = {}, shin[2] = {};
    dvec3 palm[2];       // rest palm normals (out of the palm)
    dvec3 chest;         // spine01's head at rest: arm targets are given round it
    std::vector<int> headBones;  // the head and everything on it
};

const Rig& rig() {
    static Rig r;
    static std::once_flag once;
    std::call_once(once, [] {
        const HumanMesh& m = humanMesh();
        if (!m.loaded) return;
        auto b = [&](const std::string& name) { return m.bone(name.c_str()); };
        auto at = [&](int i) { return i >= 0 ? d3(m.joints[size_t(i)].head) : dvec3{}; };
        r.root = b("root");
        const char* sp[5] = {"spine05", "spine04", "spine03", "spine02", "spine01"};
        for (int i = 0; i < 5; ++i) r.spine[i] = b(sp[i]);
        r.neck[0] = b("neck01");
        r.neck[1] = b("neck02");
        r.neck[2] = b("neck03");
        r.head = b("head");
        bool all = r.root >= 0 && r.head >= 0;
        for (int s = 0; s < 2; ++s) {
            const std::string x = s == 0 ? ".L" : ".R";
            r.clavicle[s] = b("clavicle" + x);
            r.upperarm[s] = b("upperarm01" + x);
            r.lowerarm[s] = b("lowerarm01" + x);
            r.twist[s] = b("lowerarm02" + x);
            r.wrist[s] = b("wrist" + x);
            r.upperleg[s] = b("upperleg01" + x);
            r.lowerleg[s] = b("lowerleg01" + x);
            r.foot[s] = b("foot" + x);
            r.eye[s] = b("eye" + x);
            r.lidUpper[s] = b("orbicularis03" + x);
            r.lidLower[s] = b("orbicularis04" + x);
            for (int f = 0; f < 5; ++f)
                for (int j = 0; j < 3; ++j) r.finger[s][f][j] = b("finger" + std::to_string(f + 1) + "-" + std::to_string(j + 1) + x);
            all = all && r.upperarm[s] >= 0 && r.lowerarm[s] >= 0 && r.twist[s] >= 0 && r.wrist[s] >= 0 && r.upperleg[s] >= 0 &&
                  r.lowerleg[s] >= 0 && r.foot[s] >= 0;
            if (!all) continue;
            r.shoulder[s] = at(r.upperarm[s]);
            r.elbow[s] = at(r.lowerarm[s]);
            r.wristAt[s] = at(r.wrist[s]);
            r.hip[s] = at(r.upperleg[s]);
            r.knee[s] = at(r.lowerleg[s]);
            r.ankle[s] = at(r.foot[s]);
            r.upperArm[s] = length(r.elbow[s] - r.shoulder[s]);
            r.forearm[s] = length(r.wristAt[s] - r.elbow[s]);
            r.thigh[s] = length(r.knee[s] - r.hip[s]);
            r.shin[s] = length(r.ankle[s] - r.knee[s]);
            // The palm faces down and in at rest; its normal from the knuckles' line and the hand's length.
            const dvec3 index = at(r.finger[s][1][0]), pinky = at(r.finger[s][4][0]);
            const dvec3 along = normalize((index + pinky) * 0.5 - r.wristAt[s]), across = pinky - index;
            dvec3 palm = normalize(cross(along, across));
            if (s == 0) palm = -palm;
            r.palm[s] = palm;
        }
        r.chest = at(r.spine[4]);
        // The head and its descendants (parents come first in the list).
        std::vector<char> under(m.joints.size(), 0);
        for (size_t i = 0; i < m.joints.size(); ++i) {
            const int p = m.joints[i].parent;
            under[i] = char(int(i) == r.head || (p >= 0 && under[size_t(p)]));
            if (under[i]) r.headBones.push_back(int(i));
        }
        r.ok = all && r.spine[4] >= 0;
    });
    return r;
}

// Two-bone IK: from joint a toward `target`, segments l1 then l2, the middle joint bent toward `pole`. Returns the middle
// joint; `end` is where the chain reaches (the target, or as near as the segments allow).
dvec3 solveTwoBone(const dvec3& a, const dvec3& target, double l1, double l2, const dvec3& pole, dvec3& end) {
    const dvec3 d = target - a;
    const double len = length(d);
    const dvec3 dir = len > 1e-9 ? d * (1.0 / len) : dvec3{0, -1, 0};
    const double dist = std::clamp(len, std::fabs(l1 - l2) + 1e-4, l1 + l2 - 1e-4);
    end = a + dir * dist;
    const double x = (l1 * l1 - l2 * l2 + dist * dist) / (2.0 * dist);
    const double h = std::sqrt(std::max(l1 * l1 - x * x, 0.0));
    dvec3 p = pole - dir * dot(pole, dir);
    if (length(p) < 1e-6) p = std::fabs(dir.y) < 0.9 ? cross(dir, dvec3{0, 1, 0}) : cross(dir, dvec3{1, 0, 0});
    return a + dir * x + normalize(p) * h;
}

// The ball between the hands, round the chest at rest (the arms' targets follow the chest).
const dvec3 kBallRest{0.0, 1.22, 0.34};

double ballRadius(double fill) { return k::MAGIC_BALL_RADIUS * std::cbrt(std::clamp(fill, 0.04, 1.0)); }

}  // namespace

double humanGaitCycle(double speed) { return std::clamp(0.9 + 0.5 * std::fabs(speed), 1.0, 3.4); }

double humanBreathPhase(const HumanMotion& mo) {
    // Faster when it runs on its feet (as the pose breathes).
    const double ground = 1.0 - std::max(clamp01(mo.swim), clamp01(mo.fly) * (1.0 - clamp01(mo.swim)));
    const double walk = ground * (1.0 - clamp01(mo.air) * ground) * smooth01(std::fabs(mo.speed) / 1.0);
    const double run = smooth01((std::fabs(mo.speed) - 2.0) / 2.0);
    return wrap01(mo.seconds / (k::HUMAN_BREATH_SECONDS * (1.0 - 0.45 * run * walk)));
}

HumanPose humanPose(const HumanMotion& mo) {
    const HumanMesh& mesh = humanMesh();
    const size_t n = mesh.joints.size();
    HumanPose pose(n);
    if (n == 0) return pose;
    const Rig& R = rig();
    std::vector<Mat3> turn(n), rot(n);
    std::vector<dvec3> pivot(n), move(n), scale(n, dvec3{1, 1, 1});
    for (size_t b = 0; b < n; ++b) pivot[b] = d3(mesh.joints[b].head);
    dvec3 bodyMove{0, 0, 0};
    // Down the tree: each bone's turn about its pivot after its parent's; the root carries the body's move.
    auto fk = [&] {
        for (size_t b = 0; b < n; ++b) {
            const int p = mesh.joints[b].parent;
            const Mat3 pr = p >= 0 ? rot[size_t(p)] : Mat3{};
            const dvec3 pt = p >= 0 ? move[size_t(p)] : bodyMove;
            rot[b] = mul(pr, turn[b]);
            move[b] = apply(pr, pivot[b] - apply(turn[b], pivot[b])) + pt;
        }
    };
    auto posed = [&](int b, const dvec3& restPoint) { return apply(rot[size_t(b)], restPoint) + move[size_t(b)]; };
    // A bone's turn so that its rest direction `restDir` points along `want` (model space), given its parent's rotation.
    auto aim = [&](int b, const dvec3& restDir, const dvec3& want) {
        const int p = mesh.joints[size_t(b)].parent;
        const Mat3 pr = p >= 0 ? rot[size_t(p)] : Mat3{};
        turn[size_t(b)] = between(restDir, apply(transpose(pr), want));
    };
    // A bone's turn so that its rotation in model space becomes `global`.
    auto setGlobal = [&](int b, const Mat3& global) {
        const int p = mesh.joints[size_t(b)].parent;
        const Mat3 pr = p >= 0 ? rot[size_t(p)] : Mat3{};
        turn[size_t(b)] = mul(transpose(pr), global);
    };

    const double t = mo.seconds;
    const double swim = clamp01(mo.swim), fly = clamp01(mo.fly) * (1.0 - swim);
    const double body = std::max(swim, fly);                          // carried by water or air, not standing on the ground
    const double ground = 1.0 - body;
    const double air = clamp01(mo.air) * ground, crouch = clamp01(mo.crouch) * ground, land = clamp01(mo.land) * ground;
    const double dash = clamp01(mo.dash), form = clamp01(mo.form);
    const double walk = ground * (1.0 - air) * smooth01(std::fabs(mo.speed) / 1.0);
    const double run = smooth01((std::fabs(mo.speed) - 2.0) / 2.0);
    const double gait = wrap01(mo.gait);
    const double duty = 0.62 - 0.26 * run;  // the share of a cycle a foot is down
    const double cycle = humanGaitCycle(mo.speed);
    const double breath = 0.5 - 0.5 * std::cos(2.0 * kPi * humanBreathPhase(mo));  // 0 out .. 1 in
    const double swimFlat = swim * smooth01(mo.swimSpeed / 0.9);      // swimming lies flat; treading water stands
    const double flyFlat = fly * smooth01((mo.flySpeed - 1.0) / 6.0);  // flying fast lies along the flight
    // A stroke of the breaststroke (0..1): the arms pull, recover with the kick, glide.
    const double stroke = wrap01(t / (swimFlat > 0.5 ? 1.5 : 2.2));
    // Throwing: the right arm drives out and back.
    const double toss = mo.toss < 0.0 ? 0.0 : smooth01(mo.toss / 0.07) * (1.0 - smooth01((mo.toss - 0.2) / 0.25));

    // ---- the body: tilted, leaning, twisting with the gait, its hips dropping and bobbing
    double tilt = ground * (0.04 * walk + 0.12 * walk * run + 0.38 * crouch + 0.14 * land) + 0.28 * dash;
    tilt += swimFlat * 1.22;
    const double flyTilt = 1.30 * (1.0 - 0.35 * std::clamp(mo.flyClimb, -1.0, 1.0));
    tilt += flyFlat * flyTilt;
    const double roll = -std::clamp(mo.lean, -0.6, 0.6) * (ground * 0.5 + fly * 1.1);
    const double twist = 0.09 * walk * (1.0 - 0.4 * run) * std::sin(2.0 * kPi * gait);
    const double bob = walk * (0.018 + 0.03 * run) * (1.0 - 2.0 * run) * std::cos(4.0 * kPi * (gait - duty * 0.5));
    bodyMove.y = -crouch * k::PLAYER_CROUCH_DROP - land * 0.16 + bob;
    // In water the origin is the surface: the hips float under it (lower treading water than swimming).
    bodyMove.y -= swim * (0.92 + 0.55 - 0.37 * smooth01(mo.swimSpeed / 0.9));
    bodyMove.y += fly * (1.0 - flyFlat) * 0.04 * std::sin(2.0 * kPi * t / 2.3);  // hovering, it bobs a little
    if (R.root >= 0) turn[size_t(R.root)] = mul(axisAngle({0, 1, 0}, twist), mul(axisAngle({0, 0, 1}, roll), axisAngle({1, 0, 0}, tilt)));

    // ---- the spine: breathing, turning back against the hips, arching to lift the head when lying flat
    if (const int b = R.spine[3]; b >= 0) scale[size_t(b)] = {1 + 0.008 * breath, 1 + 0.004 * breath, 1 + 0.016 * breath};
    if (const int b = R.spine[4]; b >= 0) scale[size_t(b)] = {1 + 0.012 * breath, 1 + 0.006 * breath, 1 + 0.024 * breath};
    const double arch = -(0.04 * swimFlat + 0.03 * flyFlat);
    const double sway = 0.004 * std::sin(2.0 * kPi * t / 9.7) * (1.0 - walk);
    const double spinal = 0.25 * toss;  // a throw turns the chest
    for (int i = 0; i < 5; ++i) {
        if (R.spine[i] < 0) continue;
        Mat3 m = axisAngle({1, 0, 0}, arch + (i == 4 ? -0.006 * breath : 0.0) + 0.02 * crouch);
        m = mul(axisAngle({0, 1, 0}, -twist * 0.35 + spinal * (i >= 2 ? 0.33 : 0.0)), m);
        if (i == 0) m = mul(axisAngle({0, 0, 1}, sway), m);
        turn[size_t(R.spine[i])] = m;
    }
    for (int s = 0; s < 2; ++s)
        if (R.clavicle[s] >= 0) turn[size_t(R.clavicle[s])] = axisAngle({0, 0, 1}, (s == 0 ? 1.0 : -1.0) * (0.012 * breath + 0.05 * swimFlat));

    // ---- the head: where it looks, lifted to see ahead when the body lies flat
    {
        const double yaw = std::clamp(mo.lookYaw, -1.1, 1.1) + 0.03 * std::sin(2.0 * kPi * t / 11.3);
        double pitch = std::clamp(mo.lookPitch, -0.75, 0.75) + 0.012 * std::sin(2.0 * kPi * t / 7.3);
        pitch += 0.85 * swimFlat * 1.22 + 0.75 * flyFlat * flyTilt - 0.6 * (0.38 * crouch);  // look ahead, not down
        pitch -= 0.25 * form * (1.0 - smooth01(std::fabs(mo.lookPitch) * 3.0));             // and at the ball forming
        const double share[4] = {0.22, 0.22, 0.2, 0.36};
        const int bones[4] = {R.neck[0], R.neck[1], R.neck[2], R.head};
        for (int i = 0; i < 4; ++i)
            if (bones[i] >= 0) turn[size_t(bones[i])] = mul(axisAngle({0, 1, 0}, yaw * share[i]), axisAngle({1, 0, 0}, -pitch * share[i]));
    }

    // ---- eyes and lids: glancing about, blinking
    {
        double yaw, pitch;
        glanceAt(t, yaw, pitch);
        const double blink = blinkAt(t);
        for (int s = 0; s < 2; ++s) {
            if (R.eye[s] < 0) continue;
            const dvec3 eye = pivot[size_t(R.eye[s])];
            turn[size_t(R.eye[s])] = mul(axisAngle({0, 1, 0}, yaw), axisAngle({1, 0, 0}, -pitch));
            if (R.lidUpper[s] >= 0) {
                turn[size_t(R.lidUpper[s])] = axisAngle({1, 0, 0}, 0.50 * blink - 0.3 * pitch);
                pivot[size_t(R.lidUpper[s])] = eye;
            }
            if (R.lidLower[s] >= 0) {
                turn[size_t(R.lidLower[s])] = axisAngle({1, 0, 0}, -0.10 * blink);
                pivot[size_t(R.lidLower[s])] = eye;
            }
        }
    }
    fk();
    if (!R.ok) {
        HumanPose out(n);
        for (size_t b = 0; b < n; ++b) {
            for (int row = 0; row < 3; ++row)
                for (int c = 0; c < 3; ++c) out[b].m[row * 4 + c] = float(rot[b].m[row][c]);
            out[b].m[3] = float(move[b].x);
            out[b].m[7] = float(move[b].y);
            out[b].m[11] = float(move[b].z);
        }
        return out;
    }
    const Mat3 bodyRot = rot[size_t(R.root)];
    const dvec3 rootPivot = pivot[size_t(R.root)];
    auto bodyPoint = [&](const dvec3& p) { return apply(bodyRot, p - rootPivot) + rootPivot + bodyMove; };  // round the hips
    const Mat3 chestRot = rot[size_t(R.spine[4])];
    auto chestPoint = [&](const dvec3& p) { return posed(R.spine[4], p); };                        // round the chest

    // ---- legs
    for (int s = 0; s < 2; ++s) {
        const double side = s == 0 ? 1.0 : -1.0, ay = R.ankle[s].y;
        // On the ground: standing, the gait, crouching, in the air, a dash.
        dvec3 groundAt{0.115 * side, ay, 0.03};
        double footPitch = 0.0;
        if (walk > 0.0) {
            const double ph = wrap01(gait + (s == 0 ? 0.0 : 0.5));
            const double reach = cycle * duty;  // how far the foot travels under the hip while it is down
            dvec3 g{0.11 * side, ay, 0.04};
            double pitchHere = 0.0;
            if (ph < duty) {
                const double u = ph / duty;
                g.z += reach * (0.5 - u);
                pitchHere = 0.35 * smooth01((u - 0.7) / 0.3);  // the heel lifts as it pushes off
                g.y += 0.05 * smooth01((u - 0.75) / 0.25);
            } else {
                const double u = (ph - duty) / (1.0 - duty);
                g.z += reach * (-0.5 + smooth01(u));
                g.y += (0.09 + 0.20 * run) * std::sin(kPi * u) + 0.05 * (1.0 - smooth01(u / 0.3));
                pitchHere = 0.35 * (1.0 - smooth01(u / 0.35)) - 0.2 * smooth01((u - 0.7) / 0.3);  // toes up to strike
            }
            groundAt = mix(groundAt, g, walk);
            footPitch = pitchHere * walk;
        }
        groundAt = mix(groundAt, dvec3{0.15 * side, ay, 0.07}, crouch * (1.0 - walk * 0.6));
        {
            const double tuck = smooth01((mo.rise + 1.5) / 3.0);  // rising: knees up; falling: legs reach for the ground
            const dvec3 up{0.12 * side, ay + 0.30, 0.15}, down{0.13 * side, ay - 0.02, 0.09};
            groundAt = mix(groundAt, mix(down, up, tuck), air);
            footPitch = footPitch * (1.0 - air) + 0.45 * air;
        }
        {
            const dvec3 bound = s == 0 ? dvec3{0.12, ay + 0.30, 0.42} : dvec3{-0.12, ay + 0.24, -0.42};
            groundAt = mix(groundAt, bound, dash * ground);
        }
        // Round the hips: breaststroke and treading water, flight.
        dvec3 hipsAt{0.08 * side, ay, -0.03};
        if (swim > 0.0) {
            dvec3 kick;
            if (stroke < 0.40) kick = mix(dvec3{0.07 * side, ay, -0.03}, dvec3{0.22 * side, ay + 0.42, -0.30}, smooth01(stroke / 0.40));
            else if (stroke < 0.62) kick = mix(dvec3{0.22 * side, ay + 0.42, -0.30}, dvec3{0.32 * side, ay + 0.10, -0.10}, smooth01((stroke - 0.40) / 0.22));
            else kick = mix(dvec3{0.32 * side, ay + 0.10, -0.10}, dvec3{0.07 * side, ay, -0.03}, smooth01((stroke - 0.62) / 0.2));
            const double c = 2.0 * kPi * t / 1.6 + (s == 0 ? 0.0 : kPi);
            const dvec3 tread{0.14 * side, ay + 0.22 + 0.10 * std::sin(c), 0.06 + 0.10 * std::cos(c)};
            hipsAt = mix(tread, kick, smooth01(mo.swimSpeed / 0.9));
        }
        if (fly > 0.0) {
            const dvec3 dangle{0.11 * side, ay + 0.18 + 0.03 * std::sin(1.7 * t + side), -0.10};
            const dvec3 straight{0.07 * side, ay + 0.06, -0.05};
            hipsAt = mix(hipsAt, mix(dangle, straight, flyFlat / std::max(fly, 1e-6)), fly / std::max(body, 1e-6));
        }
        const dvec3 target = mix(groundAt, bodyPoint(hipsAt), body);
        const double point = 1.0 * body;  // toes pointed in water and in the air
        const dvec3 pole = apply(bodyRot, dvec3{(0.15 + 0.5 * swim * smooth01(mo.swimSpeed / 0.9)) * side, 0.0, 1.0});
        dvec3 ankle;
        const dvec3 hip = posed(R.upperleg[s], R.hip[s]);
        const dvec3 knee = solveTwoBone(hip, target, R.thigh[s], R.shin[s], pole, ankle);
        aim(R.upperleg[s], R.knee[s] - R.hip[s], knee - hip);
        fk();
        aim(R.lowerleg[s], R.ankle[s] - R.knee[s], ankle - knee);
        fk();
        // The foot: flat (and rolling through a step) on the ground, pointed with the body when it is carried.
        const double tiltOfBody = tilt * body;
        setGlobal(R.foot[s], axisAngle({1, 0, 0}, tiltOfBody + footPitch * ground + point));
    }
    fk();

    // ---- arms: targets round the chest at rest, which the chest carries
    const double ballR = ballRadius(mo.ball);
    for (int s = 0; s < 2; ++s) {
        const double side = s == 0 ? 1.0 : -1.0;
        // Hanging, swinging with the gait (against the leg on its side), bent when running.
        const double armPhase = wrap01(gait + (s == 0 ? 0.5 : 0.0));
        const double swing = std::sin(2.0 * kPi * armPhase);
        dvec3 hand{0.25 * side, 0.885 + 0.004 * breath, 0.085};
        dvec3 palm{-side, 0.0, 0.25};
        dvec3 pole{0.3 * side, -0.2, -1.0};
        double w = 1.0;  // weights blended in below on top of hanging
        auto blend = [&](const dvec3& h, const dvec3& p, const dvec3& e, double weight) {
            if (weight <= 0.0) return;
            w += weight;
            const double k = weight / w;
            hand = mix(hand, h, k);
            palm = mix(palm, p, k);
            pole = mix(pole, e, k);
        };
        blend({0.24 * side, 0.90 + 0.02 * std::fabs(swing), 0.08 + 0.17 * swing}, {-side, 0.0, 0.2}, {0.3 * side, -0.2, -1.0}, walk * (1.0 - run) * 3.0);
        blend({0.21 * side, 1.03 + 0.05 * std::max(swing, 0.0), 0.12 + 0.24 * swing}, {-side, 0.2, 0.0}, {0.2 * side, -0.4, -1.0}, walk * run * 3.0);
        blend({0.22 * side, 0.64, 0.33}, {-side, -0.3, 0.3}, {0.5 * side, 0.0, -1.0}, crouch * (1.0 - walk) * 2.0);
        {
            const double tuck = smooth01((mo.rise + 1.5) / 3.0);
            blend(mix(dvec3{0.48 * side, 1.20, 0.05}, dvec3{0.28 * side, 1.32, 0.24}, tuck), {0.0, -1.0, 0.0}, {0.6 * side, -0.3, -0.6}, air * 3.0);
        }
        blend({0.24 * side, 0.98, -0.32}, {0.0, 1.0, -0.2}, {0.4 * side, 0.3, -0.6}, dash * 5.0);
        if (swim > 0.0) {
            dvec3 sh, sp;
            if (stroke < 0.35) {  // pull: out from ahead, then in under the chest
                const double u = stroke / 0.35;
                sh = u < 0.5 ? mix(dvec3{0.10 * side, 2.00, 0.22}, dvec3{0.45 * side, 1.85, 0.32}, smooth01(u * 2.0))
                             : mix(dvec3{0.45 * side, 1.85, 0.32}, dvec3{0.18 * side, 1.45, 0.40}, smooth01(u * 2.0 - 1.0));
                sp = mix(dvec3{side, 0.0, 0.6}, dvec3{-side, -0.4, 0.5}, u);
            } else if (stroke < 0.55) {  // recover: hands together, out ahead
                sh = mix(dvec3{0.18 * side, 1.45, 0.40}, dvec3{0.07 * side, 1.70, 0.35}, smooth01((stroke - 0.35) / 0.2));
                sp = dvec3{-side, 0.0, 0.3};
            } else {  // glide
                sh = mix(dvec3{0.07 * side, 1.70, 0.35}, dvec3{0.10 * side, 2.00, 0.22}, smooth01((stroke - 0.55) / 0.15));
                sp = dvec3{0.0, 0.0, 1.0};
            }
            const double c = 2.0 * kPi * t / 1.2;
            const dvec3 scull{(0.40 + 0.06 * std::sin(c)) * side, 1.16, 0.26 + 0.05 * std::cos(c)};
            const double flat = smooth01(mo.swimSpeed / 0.9);
            blend(mix(scull, sh, flat), mix(dvec3{0.0, -1.0, 0.2}, sp, flat), {side, 0.0, -0.2}, swim * 8.0);
        }
        if (fly > 0.0) {
            const double f = flyFlat / std::max(fly, 1e-6);
            blend(mix(dvec3{0.40 * side, 1.05, 0.15}, dvec3{0.24 * side, 0.92, -0.12}, f), mix(dvec3{0.0, -1.0, 0.0}, dvec3{-side, 0.0, 0.3}, f),
                  mix(dvec3{0.6 * side, -0.4, -0.6}, dvec3{0.2 * side, 0.0, -1.0}, f), fly * 8.0);
        }
        // Gathering a ball: palms round it in front of the chest, spreading as it grows, breathing with it.
        if (form > 0.0) {
            const double pulse = 0.008 * std::sin(2.0 * kPi * t * 1.3 + side);
            const dvec3 palmCentre = kBallRest + dvec3{side * (ballR + 0.03 + pulse), 0.0, 0.0};
            const dvec3 h = palmCentre + dvec3{side * 0.02, -0.012, -0.075};
            const double k = form;
            hand = mix(hand, h, k);
            palm = mix(palm, dvec3{-side, 0.0, 0.0}, k);
            pole = mix(pole, dvec3{0.8 * side, -1.0, -0.2}, k);
        }
        // Throwing: the right arm drives out along the aim; the left drops back.
        if (toss > 0.0) {
            if (s == 1) {
                hand = mix(hand, dvec3{-0.06, 1.40, 0.64}, toss);
                palm = mix(palm, dvec3{0.0, -0.2, 1.0}, toss);
                pole = mix(pole, dvec3{-0.6, -1.0, 0.0}, toss);
            } else {
                hand = mix(hand, dvec3{0.30, 1.0, -0.05}, toss * 0.7);
            }
        }
        const dvec3 shoulder = posed(R.upperarm[s], R.shoulder[s]);
        const dvec3 target = chestPoint(hand);
        dvec3 wristAt;
        const dvec3 elbow = solveTwoBone(shoulder, target, R.upperArm[s], R.forearm[s], apply(chestRot, pole), wristAt);
        aim(R.upperarm[s], R.elbow[s] - R.shoulder[s], elbow - shoulder);
        fk();
        aim(R.lowerarm[s], R.wristAt[s] - R.elbow[s], wristAt - elbow);
        fk();
        // Twist the forearm about its own axis so the palm faces where it should.
        {
            const dvec3 axis = normalize(wristAt - elbow);
            const dvec3 now = apply(rot[size_t(R.wrist[s])], R.palm[s]);
            const dvec3 want = normalize(apply(chestRot, palm));
            const dvec3 a = now - axis * dot(now, axis), b = want - axis * dot(want, axis);
            if (length(a) > 1e-4 && length(b) > 1e-4) {
                const dvec3 na = normalize(a), nb = normalize(b);
                const double angle = std::clamp(std::atan2(dot(cross(na, nb), axis), dot(na, nb)), -1.7, 1.7);
                const int p = mesh.joints[size_t(R.twist[s])].parent;
                turn[size_t(R.twist[s])] = mul(axisAngle(apply(transpose(rot[size_t(p)]), axis), angle), turn[size_t(R.twist[s])]);
            }
        }
        fk();
        // Fingers: relaxed, a loose fist running, flat swimming, open round a ball.
        const double curlBy = std::clamp(1.0 + 0.8 * walk * run + 0.3 * dash - 0.7 * swim - 0.3 * fly - 0.5 * form, 0.25, 2.0);
        for (int f = 0; f < 5; ++f)
            for (int j = 0; j < 3; ++j) {
                const int b = R.finger[s][f][j];
                if (b < 0) continue;
                const dvec3 dir = d3(mesh.joints[size_t(b)].tail) - d3(mesh.joints[size_t(b)].head);
                const double curl = (f == 0 ? 0.12 : (j == 0 ? 0.18 : (j == 1 ? 0.30 : 0.22)) * (1.0 + 0.12 * (f - 1))) * curlBy;
                turn[size_t(b)] = axisAngle(cross(normalize(dir), R.palm[s]), curl);
            }
    }

    // ---- the skin: rot * diag(scale) about each bone's head
    fk();
    for (size_t b = 0; b < n; ++b) {
        const dvec3 h = d3(mesh.joints[b].head), s = scale[b];
        const dvec3 tr = apply(rot[b], h - dvec3{s.x * h.x, s.y * h.y, s.z * h.z}) + move[b];
        HumanBoneMatrix& out = pose[b];
        for (int row = 0; row < 3; ++row) {
            out.m[row * 4 + 0] = float(rot[b].m[row][0] * s.x);
            out.m[row * 4 + 1] = float(rot[b].m[row][1] * s.y);
            out.m[row * 4 + 2] = float(rot[b].m[row][2] * s.z);
        }
        out.m[3] = float(tr.x);
        out.m[7] = float(tr.y);
        out.m[11] = float(tr.z);
    }
    return pose;
}

HumanPose humanIdlePose(double seconds) {
    HumanMotion m;
    m.seconds = seconds;
    return humanPose(m);
}

namespace {

dvec3 applyBone(const HumanBoneMatrix& m, const dvec3& p) {
    return {m.m[0] * p.x + m.m[1] * p.y + m.m[2] * p.z + m.m[3], m.m[4] * p.x + m.m[5] * p.y + m.m[6] * p.z + m.m[7],
            m.m[8] * p.x + m.m[9] * p.y + m.m[10] * p.z + m.m[11]};
}

}  // namespace

dvec3 humanBallPoint(const HumanMotion& motion) {
    const Rig& R = rig();
    if (!R.ok) return {0, 1.22, 0.34};
    const HumanPose pose = humanPose(motion);
    return applyBone(pose[size_t(R.spine[4])], kBallRest);
}

dvec3 humanEyes(const HumanPose& pose) {
    const Rig& R = rig();
    if (!R.ok || R.eye[0] < 0 || R.eye[1] < 0 || pose.size() <= size_t(std::max(R.eye[0], R.eye[1]))) return {0, 1.63, 0.13};
    const HumanMesh& m = humanMesh();
    const dvec3 l = applyBone(pose[size_t(R.eye[0])], d3(m.joints[size_t(R.eye[0])].head));
    const dvec3 r = applyBone(pose[size_t(R.eye[1])], d3(m.joints[size_t(R.eye[1])].head));
    return (l + r) * 0.5;
}

void hideHumanHead(HumanPose& pose) {
    const Rig& R = rig();
    if (!R.ok || pose.size() <= size_t(R.head)) return;
    const dvec3 at = applyBone(pose[size_t(R.head)], d3(humanMesh().joints[size_t(R.head)].head));
    for (const int b : R.headBones) {
        HumanBoneMatrix& m = pose[size_t(b)];
        for (int row = 0; row < 3; ++row)
            for (int c = 0; c < 3; ++c) m.m[row * 4 + c] = 0.0f;
        m.m[3] = float(at.x);
        m.m[7] = float(at.y);
        m.m[11] = float(at.z);
    }
}

}  // namespace taelia
