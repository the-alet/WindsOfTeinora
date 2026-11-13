// The free camera and the eye's focus: the mouse moves the gaze first, the camera follows it with damping.
// Inside a neutral circle around the camera's direction it doesn't turn; near the screen edge it is pulled hard.
// Nothing is abrupt: turns ease in and out (a critically damped spring toward the gaze), flight eases to its fixed
// speed and to a stop.
// Engine-free so every front end (our renderer, the Unreal port) shares it.
#pragma once
#include "taelia/sim/math.h"

namespace taelia {

struct CameraInput {
    double mouseDx = 0, mouseDy = 0;              // mouse counts since the last update (right, down positive)
    double forward = 0, right = 0, up = 0;        // movement axes in [-1, 1]
};

struct GazeScreen {
    double ndcX = 0, ndcY = 0;  // where the gaze falls on screen, [-1, 1], y up
    double circle = 0;          // distance from the centre in half screen heights
    double edge = 0;            // max(|ndcX|, |ndcY|): 1 at the screen edge
};

class CameraRig {
public:
    dvec3 position{0, 1.8, 0};
    double yaw = 0, pitch = 0;          // camera direction, radians
    double gazeYaw = 0, gazePitch = 0;  // the eye's focus, radians
    double yawRate = 0, pitchRate = 0;  // how fast the camera turns now, rad/s
    dvec3 velocity;                     // how fast it flies now, m/s

    void place(const dvec3& pos, double yawRad, double pitchRad);
    // Real-time update (the camera ignores the world's time speed).
    void update(const CameraInput& in, double realDt, double aspect);

    dvec3 forward() const { return directionFromAngles(yaw, pitch); }
    dvec3 right() const;
    dvec3 up() const;
    dvec3 gazeDirection() const { return directionFromAngles(gazeYaw, gazePitch); }

    static double fovY();  // vertical field of view, radians
    GazeScreen gazeOnScreen(double aspect) const;
    // Turn rate (1/s) for a gaze at this place on screen: 0 inside the neutral circle, hard near the edge.
    static double turnRate(const GazeScreen& g);
};

}  // namespace taelia
