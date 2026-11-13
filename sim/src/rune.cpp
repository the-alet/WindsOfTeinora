#include "taelia/sim/rune.h"

#include <algorithm>
#include <cmath>

#include "taelia/shared_constants.h"

namespace taelia {

double Rune::volume() const { return k::RUNE_VOLUME_FACTOR * size * size * size; }

double Rune::seal() const {
    switch (material) {
        case RuneMaterial::Wood: return k::RUNE_SEAL_WOOD;
        case RuneMaterial::Metal: return k::RUNE_SEAL_METAL;
        default: return k::RUNE_SEAL_STONE;
    }
}

double Rune::breakPoint() const { return k::RUNE_BREAK * seal(); }

double Rune::mass() const {
    double m = 0;
    for (const double c : contents) m += c;
    return m;
}

double Rune::density() const { return mass() / volume(); }

Aspect runeOutflowAspect(Aspect a, double densityInside) {
    using magic::Conversion;
    const double theta = magic::theta0();
    if (a == Aspect::Fire) {
        const Conversion c = magic::fireConversion(densityInside, theta);
        return c == Conversion::ToLight ? Aspect::Light : (c == Conversion::ToLightning ? Aspect::Lightning : Aspect::Fire);
    }
    if (a == Aspect::Lightning && magic::lightningConversion(densityInside, theta) == Conversion::ToLight) return Aspect::Light;
    return a;
}

namespace {

// Out into the field in front of the sign, as what it turns into at the density it had inside.
void release(const Rune& r, MagicField& field, Aspect a, double kg, double insideDensity, double reach) {
    if (kg <= 0) return;
    field.addMass(r.position + r.normal * reach, runeOutflowAspect(a, insideDensity), kg);
}

}  // namespace

void breakRune(Rune& r, MagicField& field) {
    if (r.broken) return;
    for (int a = 0; a < kAspects; ++a) {
        const double inside = r.contents[a] / r.volume();
        release(r, field, Aspect(a), r.contents[a], inside, 0.1);
        r.contents[a] = 0;
    }
    r.broken = true;
}

void stepRune(Rune& r, MagicField& field, double dt) {
    if (r.broken) return;
    const double v = r.volume();
    // Filling: its own aspect, and neutral (taking the aspect), where they are denser than the dew point.
    MagicDensities around;
    if (field.cellAt(r.position, around)) {  // the cell it is in: it takes from there
        for (const Aspect a : {r.aspect, Aspect::Neutral}) {
            const double past = double(around[a]) - k::MAGIC_DEW_POINT;
            if (past <= 0) continue;
            // Never more than the field holds past the dew point in a fine cell round it.
            const double take = std::min(k::RUNE_INTAKE * past * v * dt, past * k::RUNE_INTAKE_CAP_M3);
            if (field.addMass(r.position, a, -take)) r.contents[int(r.aspect)] += take;
            if (r.aspect == Aspect::Neutral) break;
        }
    }
    // Past its break point it bursts.
    if (r.density() > r.breakPoint()) {
        breakRune(r, field);
        return;
    }
    // Over its seal the excess leaks by effusion: per kg/m3 of excess, sqrt(Theta / 2 pi m) through its vent, so the
    // lighter the motes the sooner it vents.
    const double over = r.density() - r.seal();
    if (over > 0) {
        const double total = r.mass(), inside = r.density();
        for (int a = 0; a < kAspects; ++a) {
            if (r.contents[a] <= 0) continue;
            const Aspect asp = Aspect(a);
            const double share = r.contents[a] / total;
            const double excess = over * share;  // kg/m3 of this aspect over the seal
            const double flux = magic::effusionFlux(asp, magic::partialPressure(asp, excess, magic::theta0()), magic::theta0()) * magic::moteMass(asp);
            const double out = std::min(flux * k::RUNE_LEAK_AREA * dt, excess * v);
            r.contents[a] -= out;
            release(r, field, asp, out, inside, 0.3);  // a jet in front of the sign
        }
    }
}

}  // namespace taelia
