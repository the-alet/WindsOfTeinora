#include "taelia/sim/world.h"

#include <cstring>

#include "taelia/sim/hash.h"

namespace taelia {

World::World(const WorldDesc& d) : seed_(d.seed), clock_(d.startHour, d.startDay), settings_(d.settings), terrain_(d.seed), clouds_(d.seed) {}

int World::advance(double realSeconds) {
    const int n = clock_.advance(realSeconds);
    for (int i = 0; i < n; ++i) step();
    return n;
}

void World::step() {
    clock_.onStep();
    clouds_.setOverride(settings_.cloudCover);
    clouds_.step(WorldClock::stepSeconds(), wind(), clock_.seconds());
}


dvec3 World::wind() const {
    // Blows toward the opposite of where it comes from.
    const double to = (settings_.windFromDeg + 180.0) * kDegToRad;
    return dvec3{std::sin(to), 0.0, std::cos(to)} * settings_.windSpeed;
}

uint64_t World::stateHash() const {
    auto bits = [](double v) {
        uint64_t u;
        std::memcpy(&u, &v, sizeof u);
        return u;
    };
    uint64_t h = mix64(seed_);
    h = hashCombine(h, static_cast<uint64_t>(clock_.steps()));
    h = hashCombine(h, bits(clock_.seconds()));
    h = hashCombine(h, bits(settings_.windSpeed));
    h = hashCombine(h, bits(settings_.windFromDeg));
    h = hashCombine(h, bits(clouds_.displacement().x));
    h = hashCombine(h, bits(clouds_.displacement().z));
    return h;
}

}  // namespace taelia
