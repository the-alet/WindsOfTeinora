// The one cloud map: the sky draws its clouds from it and the weather reads it, so rain never falls from a clear sky.
// Its state is the cloud water (water magic gathered aloft, kg/m3) on a grid of texels that drift with the clouds and
// follow the camera. Where there is more of it the sky is more covered; past 30 under a closed sky it rains (the rain
// takes water from the cloud and carries water magic down); past 50, where clouds run into each other during rain, a
// storm starts, and friction there charges lightning. The natural weather pulls the cloud water toward the day's
// tendency (so a fair day looks as it did before there was weather); the world settings can fix the cover or bring
// rain. Deterministic: the same steps give the same clouds.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "taelia/sim/math.h"

namespace taelia {

struct CloudSample {
    double coverage = 0;   // 0..1: how much cloud stands over this point
    double base = 0;       // metres above sea level
    double thickness = 0;  // metres
};

// A copy of the cloud water for readers on other threads (the renderer builds its map on a worker).
struct CloudWater {
    int64_t u0 = 0, v0 = 0;   // the grid's corner texel in the clouds' coordinates
    std::vector<float> water;  // kN x kN, row-major by v
    static constexpr int kN = 128;
    static constexpr double kTexel = 800.0;  // m
    double at(double u, double v) const;     // bilinear, -1 outside the grid
};

class CloudMap {
public:
    explicit CloudMap(uint64_t seed) : seed_(seed) {}

    // One fixed step: the wind carries the clouds; the cloud water rains, gathers, storms.
    void step(double dt, const dvec3& wind, double seconds);
    // The grid of cloud water follows this point (the camera).
    void setFocus(double x, double z);

    CloudSample sample(double x, double z) const;  // here, now
    // Coverage in the clouds' own coordinates (world position minus how far the wind carried them).
    double coverageAt(double u, double v, double seconds) const;
    double coverageAt(double u, double v, double seconds, const CloudWater* water) const;
    std::shared_ptr<const CloudWater> water() const { return snapshot_.load(); }
    double baseAt(double u, double v) const;
    double thicknessFor(double coverage) const;
    double cover(double seconds) const;  // the day's tendency, 0 clear .. 1 overcast (or the override)
    void setOverride(double cover);  // < 0: the natural weather; a new value fills the sky at once (a settings slider)
    void setRain(double rain) { rain_ = rain; }             // > 0: rain over the camera (world settings, --rain)

    // The weather at a world point: cloud water (kg/m3), rain and storm (0..1).
    double waterAt(double x, double z) const;
    double rainAt(double x, double z) const;
    double stormAt(double x, double z) const;
    // Lightning strikes since the last call: world (x, z) of each.
    std::vector<dvec3> takeStrikes();

    dvec3 displacement() const { return disp_; }
    double seconds() const { return t_; }
    uint64_t stateHash() const;

private:
    void place(double u, double v);
    void publish();
    void fill(int i, int j);
    double target(double u, double v) const;
    float& w(int i, int j) { return water_[size_t(j) * CloudWater::kN + size_t(i)]; }
    int slot(int64_t t) const { return int(((t % CloudWater::kN) + CloudWater::kN) % CloudWater::kN); }

    uint64_t seed_;
    dvec3 disp_;
    double t_ = 0;
    double override_ = -1, rain_ = 0;
    double focusU_ = 0, focusV_ = 0;
    bool placed_ = false;
    int64_t u0_ = 0, v0_ = 0;  // corner texel of the ring
    std::vector<float> water_, charge_;  // toroidal by slot
    int64_t updates_ = 0;
    double acc_ = 0;
    std::vector<dvec3> strikes_;
    std::atomic<std::shared_ptr<const CloudWater>> snapshot_;
};

}  // namespace taelia
