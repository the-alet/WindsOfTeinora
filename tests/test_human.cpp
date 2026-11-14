// The human (step 6): one adult made in code, a closed skinned body, breathing slowly; balls bounce off it; it holds
// life magic like any object.
#include <map>
#include <utility>

#include "taelia/shared_constants.h"
#include "taelia/sim/human.h"
#include "taelia/sim/world.h"
#include "test.h"

using namespace taelia;

TEST(human_is_one_adult_about_175_cm_with_ordinary_proportions) {
    const HumanMesh& m = humanMesh();
    CHECK(m.loaded);  // assets/human/human.bin, from MakeHuman's CC0 assets (.utilities/makehuman.py)
    CHECK_NEAR(m.height, 1.75, 0.01);
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    const HumanPose standing = humanIdlePose(0.0);
    for (const HumanVertex& v : m.vertices) {
        const fvec3 q = humanSkin(standing, v);
        const float p[3] = {q.x, q.y, q.z};
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
    CHECK(lo[1] > -0.005f && lo[1] < 0.005f);  // standing on its soles
    CHECK(hi[0] - lo[0] > 0.40f && hi[0] - lo[0] < 0.70f);  // standing, its arms hanging by its sides
    CHECK(hi[2] - lo[2] > 0.25f && hi[2] - lo[2] < 0.45f);
    // Skin, eyes, brows, lashes, hair, clothes, shoes: every piece there, with its texture.
    for (const char* name : {"skin", "eyes", "brows", "lashes", "hair", "clothes", "shoes"}) {
        bool found = false;
        for (const HumanSubmesh& s : m.submeshes) found = found || (m.materials[s.material].name == name && s.indexCount > 0);
        CHECK(found);
    }
    for (const HumanMaterial& mat : m.materials) CHECK(!mat.albedo.empty());
    CHECK(m.vertices.size() > 15000);
}

TEST(human_has_more_than_twenty_moving_parts) {
    // MakeHuman's full skeleton: spine, neck, jaw, lips, tongue, eyes and lids, every finger and toe.
    const HumanMesh& m = humanMesh();
    CHECK(m.joints.size() == 163);
    int roots = 0;
    for (size_t b = 0; b < m.joints.size(); ++b) {
        if (m.joints[b].parent < 0) ++roots;
        else CHECK(size_t(m.joints[b].parent) < b);  // parents come first
    }
    CHECK(roots == 1);
    for (const char* name : {"jaw", "eye.L", "eye.R", "orbicularis03.L", "finger2-3.L", "finger5-3.R", "toe1-2.L", "spine01", "head"})
        CHECK(m.bone(name) >= 0);
    std::vector<bool> used(m.joints.size(), false);
    for (const HumanVertex& v : m.vertices) {
        const float sum = v.weight[0] + v.weight[1] + v.weight[2] + v.weight[3];
        CHECK_NEAR(sum, 1.0, 0.01);
        for (int s = 0; s < 4; ++s)
            if (v.weight[s] > 0) used[v.bone[s]] = true;
    }
    int moving = 0;
    for (const bool u : used) moving += u ? 1 : 0;
    CHECK(moving > 100);  // the skin follows far more than twenty of them
}

TEST(human_breathes_and_blinks) {
    // Over one breath the chest rises and falls by millimetres and the feet stay put; the upper lids come down in a blink.
    const HumanMesh& m = humanMesh();
    size_t chest = 0, toe = 0;
    for (size_t i = 0; i < m.vertices.size(); ++i) {
        const HumanVertex& v = m.vertices[i];
        if (std::fabs(v.position.y - 1.30f) < 0.02f && std::fabs(v.position.x) < 0.04f && v.position.z > m.vertices[chest].position.z) chest = i;
        if (v.position.y < 0.05f && v.position.z > m.vertices[toe].position.z) toe = i;
    }
    const double out = 0.0, in = 0.5 * k::HUMAN_BREATH_SECONDS;
    const fvec3 c0 = humanSkin(humanIdlePose(out), m.vertices[chest]), c1 = humanSkin(humanIdlePose(in), m.vertices[chest]);
    const double moved = std::hypot(std::hypot(c1.x - c0.x, c1.y - c0.y), c1.z - c0.z);
    CHECK(moved > 0.002 && moved < 0.03);
    const fvec3 t0 = humanSkin(humanIdlePose(out), m.vertices[toe]), t1 = humanSkin(humanIdlePose(in), m.vertices[toe]);
    CHECK(std::hypot(std::hypot(t1.x - t0.x, t1.y - t0.y), t1.z - t0.z) < 0.002);
    // A blink: somewhere in the first few seconds the lashes over the left eye drop.
    const int lid = m.bone("orbicularis03.L");
    CHECK(lid >= 0);
    double lowest = 1e9, open = 0;
    bool first = true;
    for (int i = 0; i < 600; ++i) {
        const HumanPose pose = humanIdlePose(i * 0.02);
        const float* q = pose[size_t(lid)].m;
        const fvec3 tip = m.joints[size_t(lid)].tail;
        const double y = q[4] * tip.x + q[5] * tip.y + q[6] * tip.z + q[7];
        if (first) open = y;
        first = false;
        lowest = std::min(lowest, y);
    }
    CHECK(lowest < open - 0.002);
}

TEST(human_stands_near_the_spawn_and_a_ball_bounces_off_it) {
    WorldDesc d;
    World w(d);
    const dvec3 feet = w.humanPosition();
    CHECK(std::hypot(feet.x - k::CAMERA_SPAWN[0], feet.z - k::CAMERA_SPAWN[2]) < 8.0);
    CHECK_NEAR(feet.y, w.groundHeight(feet.x, feet.z), 1e-9);
    // Thrown at its chest from 4 m in front of it: the ball comes back toward the thrower, it does not pass through.
    const dvec3 facing{std::sin(w.humanYaw()), 0.0, std::cos(w.humanYaw())};
    const dvec3 from = feet + facing * 4.0 + dvec3{0, 1.3, 0};
    w.throwBall(from, facing * -9.0 + dvec3{0, 1.0, 0});
    bool cameBack = false, passed = false;
    for (int i = 0; i < 90 && !w.balls().empty(); ++i) {
        w.step();
        const dvec3 p = w.balls().front().position;
        const double ahead = dot(p - feet, facing);  // > 0: in front of it
        if (ahead < -0.3) passed = true;
        if (ahead > 0.5 && dot(w.balls().front().velocity, facing) > 0.5) cameBack = true;
    }
    CHECK(cameBack);
    CHECK(!passed);
}

namespace {

// The lowest point and the extent of the skinned body in a pose.
void bodyBox(const HumanPose& pose, dvec3& lo, dvec3& hi) {
    lo = {1e9, 1e9, 1e9};
    hi = {-1e9, -1e9, -1e9};
    for (const HumanVertex& v : humanMesh().vertices) {
        const fvec3 q = humanSkin(pose, v);
        lo = {std::min(lo.x, double(q.x)), std::min(lo.y, double(q.y)), std::min(lo.z, double(q.z))};
        hi = {std::max(hi.x, double(q.x)), std::max(hi.y, double(q.y)), std::max(hi.z, double(q.z))};
    }
}

}  // namespace

TEST(human_runs_crouches_swims_flies_and_gathers_a_ball) {
    const HumanMesh& m = humanMesh();
    CHECK(m.loaded);
    if (!m.loaded) return;
    dvec3 lo, hi;
    // Walking: a foot on the ground all through the cycle; running: never through the ground, a foot down at mid-stance
    // (between, both are in the air: a run has a flight).
    for (double gait = 0.0; gait < 1.0; gait += 0.125) {
        HumanMotion walk, run;
        walk.speed = 1.4;
        walk.gait = run.gait = gait;
        run.speed = k::PLAYER_SPEED;
        bodyBox(humanPose(walk), lo, hi);
        CHECK(lo.y > -0.03);
        CHECK(lo.y < 0.12);
        bodyBox(humanPose(run), lo, hi);
        CHECK(lo.y > -0.03);
    }
    HumanMotion midStance;
    midStance.speed = k::PLAYER_SPEED;
    midStance.gait = 0.18;
    bodyBox(humanPose(midStance), lo, hi);
    CHECK(lo.y < 0.08);
    // Crouching: its head drops about as far as its hips.
    HumanMotion stand, crouch;
    crouch.crouch = 1.0;
    dvec3 slo, shi;
    bodyBox(humanPose(stand), slo, shi);
    bodyBox(humanPose(crouch), lo, hi);
    CHECK(shi.y - hi.y > 0.3);
    CHECK(lo.y > -0.03);
    // Swimming and flying fast it lies along the way it goes; treading water it stands, its head out.
    HumanMotion swim, fly, tread;
    swim.swim = 1.0;
    swim.swimSpeed = k::PLAYER_SWIM_SPEED;
    fly.fly = 1.0;
    fly.flySpeed = k::PLAYER_FLY_SPEED;
    tread.swim = 1.0;
    bodyBox(humanPose(swim), lo, hi);
    CHECK(hi.z - lo.z > 1.3);
    CHECK(hi.y < 0.6);  // at the surface
    bodyBox(humanPose(fly), lo, hi);
    CHECK(hi.z - lo.z > 1.3);
    bodyBox(humanPose(tread), lo, hi);
    CHECK(hi.y > 0.15);  // the head above the water
    CHECK(hi.y < 0.6);
    // Gathering: the ball sits in front of its chest, between its hands.
    HumanMotion form;
    form.form = 1.0;
    form.ball = 1.0;
    const dvec3 ball = humanBallPoint(form);
    CHECK(ball.z > 0.2);
    CHECK(ball.y > 1.0 && ball.y < 1.45);
    // First person: the head is shrunk to a point.
    HumanPose fp = humanPose(stand);
    hideHumanHead(fp);
    bodyBox(fp, lo, hi);
    CHECK(hi.y < shi.y - 0.1);
}

TEST(human_holds_life_magic_like_any_object) {
    WorldDesc d;
    World w(d);
    for (int i = 0; i < 120; ++i) w.step();
    // In an ordinary place it keeps close to its share.
    CHECK(w.humanHeld() > 0.5 * k::MAGIC_HELD);
    CHECK(w.humanHeld() <= k::MAGIC_HELD + 1e-9);
}

TEST(human_life_magic_is_densest_in_the_heart_and_brain) {
    const HumanMesh& m = humanMesh();
    const dvec3 heart = m.heart, brain = m.brain;
    CHECK(heart.y > 1.15 && heart.y < 1.35 && heart.x > 0.0);  // in the chest, a little to its left
    CHECK(brain.y > 1.58 && brain.y < 1.72);                    // in the skull
    const int hand = m.bone("wrist.L"), knee = m.bone("lowerleg01.L");
    const fvec3 wh = m.joints[size_t(hand)].head, kh = m.joints[size_t(knee)].head;
    const double atHeart = humanLifeMagicAt(heart), atBrain = humanLifeMagicAt(brain);
    const double atHand = humanLifeMagicAt({wh.x, wh.y, wh.z}), atKnee = humanLifeMagicAt({kh.x, kh.y, kh.z});
    CHECK_NEAR(atHeart, k::HUMAN_HEART_MAGIC, 0.2);
    CHECK_NEAR(atBrain, k::HUMAN_BRAIN_MAGIC, 0.2);
    CHECK(atHeart > atBrain && atBrain > 2.0 * atHand);
    CHECK_NEAR(atHand, k::MAGIC_HELD, 0.01);  // elsewhere its plain share
    CHECK_NEAR(atKnee, k::MAGIC_HELD, 0.01);
    CHECK(atHeart < k::MAGIC_DEW_POINT);  // dense, yet unseen in plain sight
    // The heart beats about once a second, with two knocks.
    int knocks = 0;
    bool high = false;
    for (int i = 0; i < 600; ++i) {
        const bool now = humanHeartbeat((0.5 + i / 600.0) * k::HUMAN_HEARTBEAT_SECONDS) > 0.5;  // one beat, from between two
        if (now && !high) ++knocks;
        high = now;
    }
    CHECK(knocks == 2);
}

TEST(human_life_magic_flows_like_blood) {
    // Arteries leave the heart and reach the brain, both hands and both feet; veins run beside them back; each stretch
    // takes up where the one before it ended.
    const HumanMesh& m = humanMesh();
    CHECK(m.vessels.size() > 40);
    int arteries = 0, veins = 0;
    double longest = 0;
    for (const HumanVessel& v : m.vessels) {
        (v.artery ? arteries : veins) += 1;
        CHECK(v.fromHeartB > v.fromHeartA);
        const double len = std::hypot(std::hypot(v.b.x - v.a.x, v.b.y - v.a.y), v.b.z - v.a.z);
        CHECK_NEAR(v.fromHeartB - v.fromHeartA, len, 1e-4);
        longest = std::max(longest, double(v.fromHeartB));
    }
    CHECK(arteries == veins);
    CHECK(longest > 1.0 && longest < 2.5);  // from the heart to a toe
    auto reaches = [&](const fvec3& target, double within) {
        for (const HumanVessel& v : m.vessels)
            if (v.artery && std::hypot(std::hypot(v.b.x - target.x, v.b.y - target.y), v.b.z - target.z) < within) return true;
        return false;
    };
    CHECK(reaches({float(m.brain.x), float(m.brain.y), float(m.brain.z)}, 0.01));
    for (const char* end : {"finger3-1.L", "finger3-1.R", "toe2-1.L", "toe2-1.R"}) CHECK(reaches(m.joints[size_t(m.bone(end))].head, 0.01));
    bool fromHeart = false;
    for (const HumanVessel& v : m.vessels)
        fromHeart = fromHeart || (v.artery && v.fromHeartA == 0.0f && std::hypot(std::hypot(v.a.x - m.heart.x, v.a.y - m.heart.y), v.a.z - m.heart.z) < 1e-4);
    CHECK(fromHeart);
}
