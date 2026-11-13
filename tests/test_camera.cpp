// The free camera and the eye's focus.
#include "taelia/shared_constants.h"
#include "taelia/sim/camera_rig.h"
#include "test.h"

using namespace taelia;

namespace {
constexpr double kAspect = 16.0 / 9.0;

CameraRig rigAt(double yawDeg = 0, double pitchDeg = 0) {
    CameraRig r;
    r.place({0, 1.8, 0}, yawDeg * kDegToRad, pitchDeg * kDegToRad);
    return r;
}

void run(CameraRig& r, double seconds, const CameraInput& firstFrame = {}) {
    CameraInput in = firstFrame;
    for (int i = 0; i < int(seconds * 120); ++i) {
        r.update(in, 1.0 / 120, kAspect);
        in.mouseDx = in.mouseDy = 0;
    }
}
}  // namespace

TEST(fov_is_about_10_percent_narrower) {
    CHECK_NEAR(CameraRig::fovY() / kDegToRad, 54.0, 0.5);
}

TEST(gaze_inside_neutral_circle_does_not_turn) {
    CameraRig r = rigAt(30, 0);
    r.gazeYaw += 0.02 * std::atan(std::tan(CameraRig::fovY() / 2) * kAspect);  // well inside 5%
    const double yaw0 = r.yaw;
    run(r, 3.0);
    CHECK_NEAR(r.yaw, yaw0, 1e-12);
    CHECK(CameraRig::turnRate(r.gazeOnScreen(kAspect)) == 0.0);
}

TEST(camera_follows_gaze_with_damping) {
    CameraRig r = rigAt();
    CameraInput in;
    in.mouseDx = 0.3 / k::GAZE_MOUSE_RAD_PER_COUNT;  // gaze 0.3 rad to the right
    r.update(in, 1.0 / 120, kAspect);
    const double afterOneFrame = r.yaw;
    CHECK(afterOneFrame > 0 && afterOneFrame < 0.05);  // the eye moved first; the camera lags
    run(r, 4.0);
    const GazeScreen g = r.gazeOnScreen(kAspect);
    CHECK(g.circle < k::GAZE_NEUTRAL * 2.0);  // settled near the neutral circle
    CHECK(r.yaw > 0.2);
}

TEST(gaze_near_edge_is_pulled_hard) {
    GazeScreen mid, edge;
    mid.circle = 0.4;
    mid.edge = 0.4;
    edge.circle = 1.6;
    edge.edge = 0.95;
    CHECK(CameraRig::turnRate(edge) > 4.0 * CameraRig::turnRate(mid));
    GazeScreen inside;
    inside.circle = k::GAZE_NEUTRAL * 0.9;
    CHECK(CameraRig::turnRate(inside) == 0.0);
}

TEST(focus_never_leaves_the_screen) {
    CameraRig r = rigAt();
    CameraInput in;
    in.mouseDx = 2.5 / k::GAZE_MOUSE_RAD_PER_COUNT;  // a wild flick
    r.update(in, 1.0 / 120, kAspect);
    CHECK(r.gazeOnScreen(kAspect).edge <= 1.0);
}

TEST(camera_moves_at_fixed_speed) {
    CameraRig r = rigAt(90, 0);  // facing +X
    CameraInput in;
    in.forward = 1;
    in.right = 1;  // diagonal input is not faster
    for (int i = 0; i < 120; ++i) r.update(in, 1.0 / 120, kAspect);
    const dvec3 d = r.position - dvec3{0, 1.8, 0};
    CHECK_NEAR(length(d), k::CAMERA_SPEED, 1e-6);
    CHECK(d.x > 0 && d.z < 0);  // forward +X, right -Z
}

TEST(pitch_is_limited) {
    CameraRig r = rigAt();
    CameraInput in;
    in.mouseDy = -10.0 / k::GAZE_MOUSE_RAD_PER_COUNT;
    run(r, 5.0, in);
    CHECK(r.pitch <= k::GAZE_MAX_PITCH_DEG * kDegToRad + 1e-9);
    CHECK(r.gazePitch <= k::GAZE_MAX_PITCH_DEG * kDegToRad + 1e-9);
}
