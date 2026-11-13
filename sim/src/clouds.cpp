#include "taelia/sim/clouds.h"

#include <algorithm>
#include <cmath>

#include "taelia/sim/clock.h"
#include "taelia/sim/noise.h"

namespace taelia {

void CloudMap::step(double dt, const dvec3& wind, double seconds) {
    disp_.x += wind.x * dt;
    disp_.z += wind.z * dt;
    t_ = seconds;
}

double CloudMap::cover(double seconds) const {
    if (override_ >= 0) return std::clamp(override_, 0.0, 1.0);
    // Weather drifts over days: mostly fair, sometimes overcast, sometimes clear.
    const double n = fbm3(seconds, 0.0, 0.0, 2.6 * WorldClock::secondsPerDay(), 3, 0.5, mix64(seed_ ^ 0xc10d));
    return std::clamp(0.36 + 0.9 * n, 0.04, 0.96);
}

double CloudMap::coverageAt(double u, double v, double seconds) const {
    // The third axis is time: the pattern changes over hours while the wind carries it.
    const double n = fbm3(u, v, seconds * 0.4, 9000.0, 5, 0.5, mix64(seed_ ^ 0xc1a0));
    const double c = n * 1.4 + 0.5;
    const double th = 1.0 - cover(seconds) * 1.05;
    return smoothstep(th - 0.12, th + 0.18, c);
}

double CloudMap::baseAt(double u, double v) const {
    return 1500.0 + 450.0 * fbm3(u, v, 0.0, 30000.0, 2, 0.5, mix64(seed_ ^ 0xba5e));
}

double CloudMap::thicknessFor(double coverage) const { return 350.0 + 2300.0 * std::pow(coverage, 1.6); }

CloudSample CloudMap::sample(double x, double z) const {
    const double u = x - disp_.x, v = z - disp_.z;
    CloudSample s;
    s.coverage = coverageAt(u, v, t_);
    s.base = baseAt(u, v);
    s.thickness = thicknessFor(s.coverage);
    return s;
}

}  // namespace taelia
