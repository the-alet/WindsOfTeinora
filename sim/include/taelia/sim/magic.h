// The magic field (step 5): three rings of cells around the focus (L0 2 m, L1 8 m, L2 64 m x 128 m), columns
// standing on the ground and layers aligned in absolute height, the ground a wall. Each level steps at its own fixed
// rate. Transport is conservative within a level (flux form, MUSCL/minmod advection by one velocity per cell, Fick
// diffusion with the kinetic D); the velocity follows the partial-pressure gradient, viscosity, drag toward the air
// and buoyancy. The land, the water and the plants hold the gas at its baseline (magic_baseline.h): pressure and
// diffusion act on the departure from it, so an undisturbed world stays quiet and quiet bricks are skipped. A level's
// edges read the next coarser level (or the baseline), and coarse cells under a finer level take its average.
// Deterministic: a fixed pool of threads, Jacobi updates, reductions in index order. No engine or graphics code.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "taelia/shared_constants.h"
#include "taelia/sim/magic_baseline.h"
#include "taelia/sim/magic_kinetics.h"
#include "taelia/sim/math.h"

namespace taelia {

class Terrain;
class WindField;

struct MagicOptions {
    int threads = 0;  // threads for the field, the caller included; 0: from the hardware. Results never depend on it.
    int levels = 3;   // 1..3, from the finest (tests use one)
    int columns = k::MAGIC_COLUMNS;  // per ring side and level, a multiple of 8
    int layers[3] = {k::MAGIC_L0_LAYERS, k::MAGIC_L1_LAYERS, k::MAGIC_L2_LAYERS};
    bool closed = false;      // walls at the outer edges and the top (a closed box, for tests); else open
    bool takingRule = true;   // neutral takes aspects toward the baseline
    bool skipQuiet = true;    // leave bricks near the baseline as they are
    bool flatGround = false;  // ground at flatHeight, no water or plants (tests)
    double flatHeight = 0;
    uint64_t seed = 1;        // the world's: light drops come from it
};

// A hot thing (a campfire, later lava): where its core sits and how hot it burns (1: a campfire at full strength).
struct HeatSource {
    dvec3 position;
    double strength = 1.0;
};

// Where light drops come from now: the direction toward the source (a sun, the planet) and drops per second.
struct DropSource {
    dvec3 direction;
    double rate = 0;
};

// What dense magic leaves and keeps: a mineral (earth) or a sprout (life). It holds its own magic like any object.
enum class ManifestKind : uint8_t { Mineral, Sprout };
struct Manifestation {
    uint32_t id = 0;
    ManifestKind kind = ManifestKind::Mineral;
    dvec3 position;    // its foot
    float volume = 0;  // m3
    float held = 0;    // kg/m3 of its aspect it holds
    float necrotic = 0;  // kg/m3 of necrotic magic it has taken up (it eats a sprout's life magic)
    uint32_t seed = 0; // its shape
};
struct Firefly {  // light, not insects
    dvec3 position;
    bool big = false;  // ten merged
    uint32_t seed = 0;
};
struct HotSpot {  // fire magic past MAGIC_FIRE_STEAM: steam over water, burning plants elsewhere
    dvec3 position;
    double fire = 0;     // kg/m3 (in the gas and condensed)
    bool water = false;  // at a pond's surface
};
struct Flame {  // fire condensed somewhere: it burns
    dvec3 position;
    double strength = 0;  // kg/m3 of condensate
};

struct MagicSample {
    MagicDensities rho;
    dvec3 u;           // m/s
    double theta = 0;  // J
    double airT = 0;   // K over the air's own temperature
    double necroticHeld = 0;  // kg/m3 of necrotic magic the things here hold (it tints them)
    int level = -1;    // -1: no field here, the baseline
};

struct MagicStats {
    int64_t ticks[3] = {};
    int64_t cellUpdates = 0;    // stepped cells times sub-steps, summed
    int64_t bricksSkipped = 0;  // quiet bricks left as they were, summed over ticks
    int lastSubsteps[3] = {};
    int recentres = 0;
};

// One level as a renderer sees it: values as kValues planes of cells, cell = (k * columns + sz) * columns + sx, where
// sx = X mod columns and sz = Z mod columns for world column (X, Z) (x = (X + 0.5) * dx), and local layer k sits at
// absolute layer columnBase[sz * columns + sx] + k (y = (layer + 0.5) * dy).
struct MagicLevelView {
    double dx = 0, dy = 0;
    int columns = 0, layers = 0;
    int64_t originX = 0, originZ = 0;  // world column of the ring's min corner
    const int32_t* columnBase = nullptr;
    const float* state = nullptr;
    uint32_t version = 0;  // changes whenever the level's values change
};

class MagicField {
public:
    // Per cell: the densities in Aspect order, the velocity, Theta, the air's temperature, and what the things there
    // hold of necrotic magic (it evens out with the necrotic magic round them like any held magic).
    enum Value : int {
        kRho = 0,
        kU = kAspects,
        kTheta = kAspects + 3,
        kAirT = kAspects + 4,
        kNecroticHeld = kAspects + 5,
        kValues = kAspects + 6
    };

    MagicField(const Terrain* terrain, const WindField* wind, const MagicOptions& o = {});
    ~MagicField();
    MagicField(const MagicField&) = delete;
    MagicField& operator=(const MagicField&) = delete;

    // The rings follow this point (the camera); each level re-centres on its own tick.
    void setFocus(const dvec3& p);
    bool started() const;
    // Hot things now: each holds a hot core of fire magic (thinner farther out), sends free fire magic up and heats the
    // air, which rises.
    void setHeat(const std::vector<HeatSource>& sources);
    // Light drops now (the suns, the planet's reflection).
    void setDrops(const std::vector<DropSource>& sources);
    // One world step (stepIndex counts from 1): runs the level ticks due now. Nothing before the first focus.
    void step(int64_t stepIndex);

    MagicSample sample(const dvec3& p) const;  // the finest level holding p, else the baseline
    const MagicBaseline& baseline() const;

    uint64_t stateHash() const;
    double totalMass(Aspect a) const;  // kg over the field, each place counted once (in its finest level)
    double minDensity() const;         // the lowest density of any aspect in any cell
    // Adds density to the finest cell holding p (tests and scripted checks; no key reaches it). False if none.
    bool addDensity(const dvec3& p, Aspect a, double kgPerM3);
    // Adds kg of an aspect to the finest cell holding p (a lightning strike's fire). False if none.
    bool addMass(const dvec3& p, Aspect a, double kg);
    // The densities in the finest cell holding p, as they are (no blending with its neighbours), and its volume (m3).
    // False if none.
    bool cellAt(const dvec3& p, MagicDensities& out, double* volume = nullptr) const;
    // Rain over the camera now (0..1): raindrops carry water magic down.
    void setRain(double rain);

    // Manifestations: what stays (the world evens out what each holds), and what shows this tick.
    const std::vector<Manifestation>& manifestations() const;
    std::vector<Manifestation>& manifestations();
    const std::vector<Firefly>& fireflies() const;
    const std::vector<Flame>& flames() const;
    const std::vector<HotSpot>& hotSpots() const;
    // Where magic is past the dew point (visible, bends light): a box around those cells. False if nowhere.
    bool denseBox(dvec3& lo, dvec3& hi) const;

    MagicLevelView view(int level) const;
    const MagicStats& stats() const;
    int threads() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace taelia
