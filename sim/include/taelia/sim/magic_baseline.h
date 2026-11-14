// The analytic baseline of magic: what the land, the water and the plants hold the gas at when nothing stirs it
// (.local/step5_design.json, "sources_sinks"). The field's taking rule pulls toward it and new bricks are filled from
// it; it is never a hidden sink. Earth near the ground, water near and in ponds, life by meadows and in tree crowns,
// free magic (half air, half neutral) everywhere up to about 2 km.
#pragma once
#include <vector>

#include "taelia/sim/magic_kinetics.h"
#include "taelia/sim/math.h"

namespace taelia {

class Terrain;
struct Pond;

// What the baseline needs to know about one column of ground.
struct MagicColumn {
    double ground = 0;          // the ground (a pond's bed under water)
    double water = -1e30;       // the water surface over this column, if any
    double rockSand = 0;        // 0..1: bare rock and sand hold a little more earth
    double meadow = 0;          // 0..1
    double shoreDistance = 1e30;  // horizontal metres to the nearest pond's water (dry columns)
    double shoreLevel = -1e30;    // that pond's level
    bool underwater() const { return water > ground; }
    double surface() const { return underwater() ? water : ground; }
};

// A tree crown as an ellipsoid.
struct MagicCrown {
    dvec3 center;
    double radius = 0;     // horizontal
    double halfHeight = 0;
};

class MagicBaseline {
public:
    // terrain may be null: flat ground at `flatHeight`, no water, no plants (tests).
    explicit MagicBaseline(const Terrain* terrain, double flatHeight = 0) : terrain_(terrain), flat_(flatHeight) {}

    const Terrain* terrain() const { return terrain_; }
    // Ponds that may matter around an area (pass them to column()).
    void pondsNear(double x, double z, double radius, std::vector<Pond>& out) const;
    MagicColumn column(double x, double z, const std::vector<Pond>& ponds) const;
    void crownsInArea(double x0, double z0, double x1, double z1, std::vector<MagicCrown>& out) const;

    // The density at height y over a column, and its average over the part of [y0, y1] above the ground.
    MagicDensities point(const MagicColumn& c, double x, double y, double z, const std::vector<MagicCrown>* crowns) const;
    MagicDensities average(const MagicColumn& c, double x, double z, double y0, double y1,
                           const std::vector<MagicCrown>* crowns) const;
    // Self-contained point value (samples the terrain: for the HUD and tests, not for inner loops).
    MagicDensities at(const dvec3& p) const;

private:
    const Terrain* terrain_;
    double flat_;
};

}  // namespace taelia
