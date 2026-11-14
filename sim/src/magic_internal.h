// The magic field's insides, shared by magic_field.cpp (rings, filling, ghosts, nesting, sampling) and magic_step.cpp
// (the level tick). Not part of the public API.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include "jobs.h"
#include "taelia/sim/magic.h"

namespace taelia {
namespace mf {

constexpr int kV = MagicField::kValues;
constexpr int kA = kAspects;
constexpr int kB = 8;     // brick edge in cells
constexpr int kDer = 4 + kA;  // derived per cell: rho, p - pBase, sound speed, kinematic viscosity, D per aspect
enum Der : int { dRho = 0, dP = 1, dC = 2, dNu = 3, dD = 4 };

inline int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline int posMod(int64_t a, int64_t m) {
    const int64_t r = a % m;
    return int(r < 0 ? r + m : r);
}

// A neighbour: >= 0 a cell of the level, kWall the ground or a closed edge, else ghost g stored as -3 - g.
constexpr int32_t kWall = -1;
inline bool isGhost(int32_t n) { return n <= -3; }
inline int32_t ghostRef(int g) { return -3 - g; }
inline int ghostIndex(int32_t n) { return -3 - n; }

struct Level {
    int id = 0;
    double dx = 0, dy = 0;
    int nc = 0, ny = 0, cols = 0, cells = 0, nbx = 0, nby = 0, bricks = 0;
    int every = 1, offset = 0;
    int64_t ox = 0, oz = 0;  // world column of the ring's min corner (a multiple of kB)
    bool placed = false;
    uint32_t version = 0;          // changes whenever the level's values change
    uint32_t restrictedFrom = ~0u;  // the finer level's version last copied in
    uint32_t ghostsFrom = ~0u;      // the coarser level's version the ghosts last read
    bool ghostsDirty = true;        // rebuilt: refresh the ghosts

    std::vector<int32_t> kb;          // per slot column (sz * nc + sx): absolute layer of local layer 0
    std::vector<MagicColumn> column;  // per slot column: what the baseline knows about it
    std::vector<float> st, nx;        // [kV][cells] now and next
    std::vector<float> base;          // [kA][cells] the baseline
    std::vector<float> pBase;         // [cells] the baseline's pressure, Pa
    std::vector<float> thetaBase;     // [cells] the temperature the cell's magic is held at (hot near a fire)
    std::vector<float> emit, heat;    // [cells] free fire magic made here (kg m^-3 s^-1), air heated here (K/s)
    std::vector<int32_t> heated;      // the cells hot things touched this tick
    std::vector<float> cond;          // [6][cells] condensate (finest level): earth, water, life, fire, air, light
    std::vector<int32_t> nb;          // [6][cells]: -x +x -y +y -z +z
    std::vector<uint8_t> covered;     // [cells]: a finer level holds this place (its average is copied here)

    // Ghosts: the cells just outside (world column X, Z and absolute layer K); values refreshed every tick.
    std::vector<int64_t> gX, gZ, gK;
    std::vector<float> gv;      // [kV][ghosts]
    std::vector<float> gBase;   // [kA][ghosts] the baseline there, the value when no coarser level holds the place
    std::vector<float> gpBase;  // [ghosts]
    std::vector<int32_t> gOwner;
    std::vector<double> gGround;  // the ground (or water) under each ghost

    std::vector<uint8_t> active;    // [bricks] stepped this tick
    std::vector<uint8_t> wake;      // [bricks] a ghost next to it is off the baseline
    std::vector<float> deviation;   // [bricks] largest |rho - base| after the last tick
    std::vector<float> air;         // [3][(nbx+1)^2 (nby+1)] the air's velocity at brick corners

    int ghosts() const { return int(gX.size()); }
    size_t at(int v, int c) const { return size_t(v) * size_t(cells) + size_t(c); }
    size_t gat(int v, int g) const { return size_t(v) * gX.size() + size_t(g); }
    int cell(int sx, int sz, int k) const { return (k * nc + sz) * nc + sx; }
    int brickOf(int c) const {
        const int sx = c % nc, sz = (c / nc) % nc, k = c / cols;
        return ((k / kB) * nbx + sz / kB) * nbx + sx / kB;
    }
    int64_t worldX(int sx) const { return ox + posMod(sx - ox, nc); }
    int64_t worldZ(int sz) const { return oz + posMod(sz - oz, nc); }
    bool inRing(int64_t X, int64_t Z) const { return placed && X >= ox && X < ox + nc && Z >= oz && Z < oz + nc; }
    int slotColumn(int64_t X, int64_t Z) const { return posMod(Z, nc) * nc + posMod(X, nc); }
    // The cell at world column (X, Z) and absolute layer K: >= 0, or -1 outside the ring, -2 underground, -3 above.
    int find(int64_t X, int64_t Z, int64_t K) const {
        if (!inRing(X, Z)) return -1;
        const int col = slotColumn(X, Z);
        const int64_t k = K - kb[size_t(col)];
        if (k < 0) return -2;
        if (k >= ny) return -3;
        return int(k) * cols + col;
    }
};

}  // namespace mf

struct MagicField::Impl {
    MagicOptions opt;
    const Terrain* terrain = nullptr;
    const WindField* wind = nullptr;
    MagicBaseline baseline;
    std::unique_ptr<JobPool> pool;
    int levels = 3;
    mf::Level lv[3];
    MagicStats stats;
    dvec3 focus;
    bool hasFocus = false, started = false;
    std::vector<HeatSource> heatSources;
    std::vector<DropSource> dropSources;
    double rain = 0;
    std::vector<Manifestation> manifestations;
    std::vector<Firefly> fireflies;
    std::vector<Flame> flames;
    std::vector<HotSpot> hotSpots;
    uint32_t nextManifest = 1;
    bool dense = false;
    dvec3 denseLo, denseHi;

    // Per aspect, cached from the kinetics.
    double mass[kAspects] = {}, sigma[kAspects] = {}, tau[kAspects] = {};

    // Tick scratch, sized for the largest level (one level ticks at a time).
    std::vector<float> der;     // [kDer][cells]
    std::vector<float> flux;    // [3][kA][cells] through each cell's + face (kg m^-2 s^-1)
    std::vector<float> fluxLo;  // [3][kA][cells] through the - face where the - side is not stepped
    std::vector<float> keep;    // [kA][cells] the share of its outflow a cell can give (no negatives)
    std::vector<float> chunkMax;
    std::vector<float> gDer;    // [kDer][ghosts] of the level ticking

    Impl(const Terrain* t, const WindField* w, const MagicOptions& o);

    // magic_field.cpp
    void place(int l);
    void fillColumns(int l, const std::vector<int>& slots);
    void buildNeighbours(int l);
    void refreshGhosts(int l);
    void restrictInto(int coarse);
    void airCorners(int l);
    // .level = -1 if the level does not hold p. fromBase: the level's baseline densities instead of its state.
    MagicSample sampleLevel(int l, const dvec3& p, bool fromBase = false) const;
    dvec3 airAt(const dvec3& p, double aboveGround) const;

    // magic_step.cpp
    void derive(const float* rho, double theta, double pBase, float* out) const;  // out: kDer values
    void tick(int l);
    void applyHeat(int l);
    void drops(int l, double dtTick, int64_t tick);  // magic_drops.cpp
    void raindrops(int l, double dtTick, int64_t tick);
    void manifest(int l, int64_t tick);              // magic_manifest.cpp
};

}  // namespace taelia
