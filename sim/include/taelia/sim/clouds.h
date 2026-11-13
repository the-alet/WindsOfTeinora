// The one cloud map: the sky draws its clouds from it and the weather (rain, storms: step 5) will read it, so
// rain can never fall from a clear sky. Carried by the wind, slowly changing, a tendency per day from clear to
// overcast (or a fixed cover from the world settings). Deterministic: the same steps give the same clouds.
#pragma once
#include <cstdint>

#include "taelia/sim/math.h"

namespace taelia {

struct CloudSample {
    double coverage = 0;   // 0..1: how much cloud stands over this point
    double base = 0;       // metres above sea level
    double thickness = 0;  // metres
};

class CloudMap {
public:
    explicit CloudMap(uint64_t seed) : seed_(seed) {}
    // One fixed step: the wind carries the clouds.
    void step(double dt, const dvec3& wind, double seconds);
    CloudSample sample(double x, double z) const;  // here, now
    // Coverage in the clouds' own coordinates (world position minus how far the wind carried them).
    double coverageAt(double u, double v, double seconds) const;
    double baseAt(double u, double v) const;
    double thicknessFor(double coverage) const;
    double cover(double seconds) const;  // the day's weather, 0 clear .. 1 overcast (or the override)
    void setOverride(double cover) { override_ = cover; }  // < 0: the natural weather
    dvec3 displacement() const { return disp_; }
    double seconds() const { return t_; }

private:
    uint64_t seed_;
    dvec3 disp_;
    double t_ = 0;
    double override_ = -1;
};

}  // namespace taelia
