#include "taelia/sim/camera_rig.h"

#include <algorithm>

#include "taelia/shared_constants.h"

namespace taelia {

double CameraRig::fovY() { return k::CAMERA_FOV_V_DEG * kDegToRad; }

void CameraRig::place(const dvec3& pos, double yawRad, double pitchRad) {
    const double maxPitch = k::GAZE_MAX_PITCH_DEG * kDegToRad;
    position = pos;
    yaw = gazeYaw = wrapAngle(yawRad);
    pitch = gazePitch = clampd(pitchRad, -maxPitch, maxPitch);
    yawRate = pitchRate = 0;
    velocity = {};
}

namespace {

// One implicit step of a damped spring pulling an angle toward its target (stable for any step): stiffness from the
// follow rate (a critically damped spring with this natural frequency settles about as fast as a plain exponential
// follow at `rate`), damping never below the stop rate so a turn always eases to rest.
double springRate(double rate, double error, double angleRate, double dt) {
    const double w = 2.15 * rate, stop = 2.0 * std::max(w, double(k::GAZE_STOP_RATE));
    return (angleRate + dt * w * w * error) / (1.0 + dt * stop + dt * dt * w * w);
}

}  // namespace

dvec3 CameraRig::right() const { return {std::cos(yaw), 0.0, -std::sin(yaw)}; }
dvec3 CameraRig::up() const { return cross(forward(), right()); }

GazeScreen CameraRig::gazeOnScreen(double aspect) const {
    const dvec3 g = gazeDirection(), f = forward(), r = right(), u = up();
    const double gz = std::max(dot(g, f), 1e-4);
    const double tanY = std::tan(fovY() * 0.5), tanX = tanY * aspect;
    GazeScreen s;
    s.ndcX = dot(g, r) / gz / tanX;
    s.ndcY = dot(g, u) / gz / tanY;
    s.circle = std::sqrt(s.ndcX * aspect * s.ndcX * aspect + s.ndcY * s.ndcY);
    s.edge = std::max(std::fabs(s.ndcX), std::fabs(s.ndcY));
    return s;
}

double CameraRig::turnRate(const GazeScreen& g) {
    const double n = k::GAZE_NEUTRAL;
    double rate = k::GAZE_FOLLOW_RATE * smoothstep(n, n * 2.0, g.circle);
    rate += (k::GAZE_HARD_RATE - k::GAZE_FOLLOW_RATE) * smoothstep(k::GAZE_PULL_HARD, 1.0, g.edge);
    return rate;
}

void CameraRig::update(const CameraInput& in, double dt, double aspect) {
    const double maxPitch = k::GAZE_MAX_PITCH_DEG * kDegToRad;
    // 1. The eye moves first.
    gazeYaw = wrapAngle(gazeYaw + in.mouseDx * k::GAZE_MOUSE_RAD_PER_COUNT);
    gazePitch = clampd(gazePitch - in.mouseDy * k::GAZE_MOUSE_RAD_PER_COUNT, -maxPitch, maxPitch);
    // The focus stays on screen: a fast flick drags the camera along at the edge.
    const double halfY = fovY() * 0.5, halfX = std::atan(std::tan(halfY) * aspect);
    double dyaw = wrapAngle(gazeYaw - yaw), dpitch = gazePitch - pitch;
    const double limX = halfX * 0.98, limY = halfY * 0.98;
    if (dyaw > limX) yaw = wrapAngle(gazeYaw - limX);
    if (dyaw < -limX) yaw = wrapAngle(gazeYaw + limX);
    if (dpitch > limY) pitch = gazePitch - limY;
    if (dpitch < -limY) pitch = gazePitch + limY;

    // 2. The camera follows with damping, easing into and out of the turn.
    if (dt > 0) {
        const double rate = turnRate(gazeOnScreen(aspect));
        yawRate = springRate(rate, wrapAngle(gazeYaw - yaw), yawRate, dt);
        pitchRate = springRate(rate, gazePitch - pitch, pitchRate, dt);
        yaw = wrapAngle(yaw + yawRate * dt);
        pitch = pitch + pitchRate * dt;
        if (std::fabs(pitch) > maxPitch) {
            pitch = clampd(pitch, -maxPitch, maxPitch);
            pitchRate = 0;
        }
    }

    // 3. Flight along the camera's axes at a fixed speed, easing to it and to a stop.
    dvec3 move = forward() * in.forward + right() * in.right + dvec3{0, 1, 0} * in.up;
    const double len = length(move);
    const dvec3 want = len > 1e-9 ? move * (k::CAMERA_SPEED / std::max(len, 1.0)) : dvec3{};
    if (dt > 0) {
        velocity += (want - velocity) * (1.0 - std::exp(-dt / k::CAMERA_EASE_SECONDS));
        if (len <= 1e-9 && length(velocity) < 0.01) velocity = {};
        position += velocity * dt;
    }
}

}  // namespace taelia
