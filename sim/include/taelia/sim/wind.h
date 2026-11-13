// The wind near the ground (step 4). The set wind blows at WIND_REFERENCE_HEIGHT; lower down it slows over the grass
// (a log profile), and it is never one vector everywhere: gusts sweep across the land and eddies of smooth noise ride
// the mean wind (frozen turbulence), stronger in a stronger wind, with a little stirring even in still air.
// Deterministic from the seed. The whole pattern rides one wind (the set one), integrated step by step, so heights
// stay coherent and a change of the setting doesn't make it jump.
#pragma once
#include <cstdint>

#include "taelia/sim/math.h"

namespace taelia {

class WindField {
public:
    explicit WindField(uint64_t seed) : seed_(seed) {}

    void setMean(const dvec3& atReferenceHeight) { mean_ = atReferenceHeight; }
    const dvec3& mean() const { return mean_; }

    // Carries the gusts and eddies along with the set wind for dt seconds (once per fixed step).
    void step(double dt);

    // The mean wind alone, `aboveGround` metres over the ground or water.
    dvec3 meanAt(double aboveGround) const;
    // The air's velocity at p (m/s), `aboveGround` metres over the ground or water.
    dvec3 at(const dvec3& p, double aboveGround) const;
    uint64_t stateHash() const;

private:
    uint64_t seed_;
    dvec3 mean_{};
    dvec3 carried_{};     // how far the pattern has ridden the wind
    double evolved_ = 0;  // how far it has changed on the way
};

}  // namespace taelia
