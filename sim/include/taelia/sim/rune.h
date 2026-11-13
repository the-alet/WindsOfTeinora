// Runes (step 7): a sign carved or drawn on a thing, holding magic as a closed system: very dense inside, yet up to its
// seal none of it reaches the world (to the magic field it does not exist: no diffusion, no drift, no fading, no change
// under pressure, no forces, no bent light, no manifestations). The only way in: magic of its aspect, or neutral magic
// (which takes the aspect), denser than the dew point where it is, drawn in at a rate proportional to how far past.
// Over the seal the excess leaks out by effusion (faster for lighter motes) as a jet in front of the sign; past its
// break point, or when the sign is damaged or its carrier falls apart, everything comes out at once and it is gone,
// the usual rules acting at once (fire past 675 comes out as lightning, past 1100 as light). First numbers: SPEC.md.
#pragma once
#include <cstdint>

#include "taelia/sim/magic.h"
#include "taelia/sim/magic_kinetics.h"
#include "taelia/sim/math.h"

namespace taelia {

enum class RuneMaterial : uint8_t { Wood, Stone, Metal };

struct Rune {
    uint32_t id = 0;
    Aspect aspect = Aspect::Neutral;
    RuneMaterial material = RuneMaterial::Stone;
    dvec3 position;          // the sign's centre
    dvec3 normal{0, 0, 1};   // the way it faces: its jet leaves that way
    double size = 0.5;       // m across
    double contents[kAspects] = {};  // kg of each aspect inside
    bool broken = false;

    double volume() const;      // m3: 0.5 m across holds about 0.1 m3
    double seal() const;        // kg/m3 it holds shut: wood 400, stone 600, metal 900
    double breakPoint() const;  // 1.5 x the seal
    double mass() const;        // kg inside
    double density() const;     // kg/m3 inside
};

// One fixed step of a rune with the field round it: what it draws in, what leaks over its seal, whether it breaks
// (then everything has gone out into the field and it is marked broken).
void stepRune(Rune& r, MagicField& field, double dt);
// Its sign damaged or its carrier gone: everything comes out at once.
void breakRune(Rune& r, MagicField& field);
// What an aspect comes out as from this density inside: fire past 675 as lightning, past 1100 as light; lightning past
// the light threshold as light; the rest as itself.
Aspect runeOutflowAspect(Aspect a, double densityInside);

}  // namespace taelia
