// The magic field's rings: placing them around the focus, filling new columns, ghosts, nesting, sampling.
#include "taelia/sim/magic.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "magic_internal.h"
#include "taelia/sim/hash.h"
#include "taelia/sim/terrain.h"
#include "taelia/sim/wind.h"

namespace taelia {

using namespace mf;

namespace {

const double kCell[3] = {k::MAGIC_L0_CELL, k::MAGIC_L1_CELL, k::MAGIC_L2_CELL};
const double kCellY[3] = {k::MAGIC_L0_CELL_Y, k::MAGIC_L1_CELL_Y, k::MAGIC_L2_CELL_Y};
const int kEvery[3] = {k::MAGIC_L0_EVERY, k::MAGIC_L1_EVERY, k::MAGIC_L2_EVERY};
const int kOffset[3] = {0, k::MAGIC_L1_OFFSET, k::MAGIC_L2_OFFSET};

double basePressure(const MagicDensities& d) {
    double n = 0;
    for (int a = 0; a < kA; ++a) n += double(d.rho[a]) / magic::moteMass(Aspect(a));
    return n * magic::theta0();
}

}  // namespace

MagicField::Impl::Impl(const Terrain* t, const WindField* w, const MagicOptions& o)
    : opt(o), terrain(o.flatGround ? nullptr : t), wind(w), baseline(o.flatGround ? nullptr : t, o.flatHeight) {
    levels = std::clamp(o.levels, 1, 3);
    pool = std::make_unique<JobPool>(o.threads > 0 ? o.threads : JobPool::defaultThreads());
    for (int a = 0; a < kA; ++a) {
        mass[a] = magic::moteMass(Aspect(a));
        sigma[a] = magic::crossSection(Aspect(a));
        tau[a] = magic::inertiaTau(Aspect(a));
    }
    size_t most = 0;
    for (int l = 0; l < levels; ++l) {
        Level& L = lv[l];
        L.id = l;
        L.dx = kCell[l];
        L.dy = kCellY[l];
        L.nc = o.columns;
        L.ny = o.layers[l];
        L.cols = L.nc * L.nc;
        L.cells = L.cols * L.ny;
        L.nbx = L.nc / kB;
        L.nby = (L.ny + kB - 1) / kB;
        L.bricks = L.nbx * L.nbx * L.nby;
        L.every = kEvery[l];
        L.offset = kOffset[l];
        L.kb.assign(size_t(L.cols), 0);
        L.column.assign(size_t(L.cols), MagicColumn{});
        L.st.assign(size_t(kV) * L.cells, 0.0f);
        L.nx = L.st;
        L.base.assign(size_t(kA) * L.cells, 0.0f);
        L.pBase.assign(size_t(L.cells), 0.0f);
        L.thetaBase.assign(size_t(L.cells), float(magic::theta0()));
        L.emit.assign(size_t(L.cells), 0.0f);
        L.heat.assign(size_t(L.cells), 0.0f);
        L.nb.assign(size_t(6) * L.cells, kWall);
        L.covered.assign(size_t(L.cells), 0);
        L.active.assign(size_t(L.bricks), 1);
        L.wake.assign(size_t(L.bricks), 0);
        L.deviation.assign(size_t(L.bricks), 1e9f);
        L.air.assign(size_t(3) * (L.nbx + 1) * (L.nbx + 1) * (L.nby + 1), 0.0f);
        most = std::max(most, size_t(L.cells));
    }
    lv[0].cond.assign(size_t(6) * lv[0].cells, 0.0f);
    der.assign(size_t(kDer) * most, 0.0f);
    flux.assign(size_t(3) * kA * most, 0.0f);
    fluxLo.assign(size_t(3) * kA * most, 0.0f);
    keep.assign(size_t(kA) * most, 1.0f);
}

dvec3 MagicField::Impl::airAt(const dvec3& p, double aboveGround) const {
    return wind ? wind->at(p, aboveGround) : dvec3{};
}

// Centres level l's ring on the focus (with a brick of slack), filling the columns that come into it.
void MagicField::Impl::place(int l) {
    Level& L = lv[l];
    const int64_t cx = int64_t(std::floor(focus.x / L.dx)), cz = int64_t(std::floor(focus.z / L.dx));
    const int64_t wantX = floorDiv(cx - L.nc / 2, kB) * kB, wantZ = floorDiv(cz - L.nc / 2, kB) * kB;
    std::vector<int> slots;
    if (!L.placed) {
        L.ox = wantX;
        L.oz = wantZ;
        L.placed = true;
        for (int s = 0; s < L.cols; ++s) slots.push_back(s);
    } else {
        if (std::abs(cx - (L.ox + L.nc / 2)) <= kB && std::abs(cz - (L.oz + L.nc / 2)) <= kB) return;
        const int64_t oldX = L.ox, oldZ = L.oz;
        L.ox = wantX;
        L.oz = wantZ;
        for (int sz = 0; sz < L.nc; ++sz)
            for (int sx = 0; sx < L.nc; ++sx)
                if (oldX + posMod(sx - oldX, L.nc) != L.worldX(sx) || oldZ + posMod(sz - oldZ, L.nc) != L.worldZ(sz))
                    slots.push_back(sz * L.nc + sx);
        ++stats.recentres;
    }
    fillColumns(l, slots);
    buildNeighbours(l);
    ++L.version;
}

// New columns: the ground, the baseline per cell, and the state from the next coarser level (or the baseline at rest).
void MagicField::Impl::fillColumns(int l, const std::vector<int>& slots) {
    if (slots.empty()) return;
    Level& L = lv[l];
    double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
    for (int s : slots) {
        const double x = double(L.worldX(s % L.nc)) * L.dx, z = double(L.worldZ(s / L.nc)) * L.dx;
        x0 = std::min(x0, x);
        x1 = std::max(x1, x + L.dx);
        z0 = std::min(z0, z);
        z1 = std::max(z1, z + L.dx);
    }
    std::vector<Pond> ponds;
    baseline.pondsNear(0.5 * (x0 + x1), 0.5 * (z0 + z1), 0.5 * std::hypot(x1 - x0, z1 - z0) + 100.0, ponds);
    std::vector<MagicCrown> crowns;
    const bool withCrowns = l < 2;  // a crown is lost in L2's 64 m cells
    if (withCrowns) baseline.crownsInArea(x0 - 12.0, z0 - 12.0, x1 + 12.0, z1 + 12.0, crowns);
    const bool coarser = l + 1 < levels && lv[l + 1].placed;
    pool->run(int(slots.size()), [&](int i) {
        const int s = slots[size_t(i)];
        const double x = (double(L.worldX(s % L.nc)) + 0.5) * L.dx, z = (double(L.worldZ(s / L.nc)) + 0.5) * L.dx;
        const MagicColumn col = baseline.column(x, z, ponds);
        L.column[size_t(s)] = col;
        const int32_t kb = int32_t(std::floor(col.ground / L.dy));
        L.kb[size_t(s)] = kb;
        for (int k = 0; k < L.ny; ++k) {
            const int c = k * L.cols + s;
            const double y0 = double(kb + k) * L.dy, y1 = y0 + L.dy;
            const MagicDensities b = baseline.average(col, x, z, y0, y1, withCrowns ? &crowns : nullptr);
            for (int a = 0; a < kA; ++a) L.base[L.at(a, c)] = b.rho[a];
            L.pBase[size_t(c)] = float(basePressure(b));
            if (!L.cond.empty())
                for (int m = 0; m < 6; ++m) L.cond[size_t(m) * L.cells + size_t(c)] = 0.0f;
            const dvec3 p{x, 0.5 * (y0 + y1), z};
            MagicSample from;
            if (coarser) from = sampleLevel(l + 1, p);
            if (from.level >= 0) {
                // The fine baseline plus the coarser level's departure from its own: fine structure, coarse stirring.
                const MagicSample fromBase = sampleLevel(l + 1, p, true);
                for (int a = 0; a < kA; ++a) L.st[L.at(a, c)] = std::max(0.0f, b.rho[a] + from.rho.rho[a] - fromBase.rho.rho[a]);
                L.st[L.at(kU, c)] = float(from.u.x);
                L.st[L.at(kU + 1, c)] = float(from.u.y);
                L.st[L.at(kU + 2, c)] = float(from.u.z);
                L.st[L.at(kTheta, c)] = float(from.theta);
                L.st[L.at(kAirT, c)] = float(from.airT);
            } else {
                const dvec3 u = airAt(p, std::max(p.y - col.surface(), 0.0));
                for (int a = 0; a < kA; ++a) L.st[L.at(a, c)] = b.rho[a];
                L.st[L.at(kU, c)] = float(u.x);
                L.st[L.at(kU + 1, c)] = float(u.y);
                L.st[L.at(kU + 2, c)] = float(u.z);
                L.st[L.at(kTheta, c)] = float(magic::theta0());
                L.st[L.at(kAirT, c)] = 0.0f;
            }
        }
    });
}

// Who is next to each cell: a cell, the ground (a wall), or a ghost outside the ring or above a column's top.
void MagicField::Impl::buildNeighbours(int l) {
    Level& L = lv[l];
    L.gX.clear();
    L.gZ.clear();
    L.gK.clear();
    L.gOwner.clear();
    std::map<std::tuple<int64_t, int64_t, int64_t>, int> index;
    const auto ghost = [&](int64_t X, int64_t Z, int64_t K, int owner) -> int32_t {
        if (opt.closed) return kWall;
        const auto [it, fresh] = index.try_emplace(std::tuple<int64_t, int64_t, int64_t>{X, Z, K}, int(L.gX.size()));
        if (fresh) {
            L.gX.push_back(X);
            L.gZ.push_back(Z);
            L.gK.push_back(K);
            L.gOwner.push_back(owner);
        }
        return ghostRef(it->second);
    };
    const size_t C = size_t(L.cells);
    for (int c = 0; c < L.cells; ++c) {
        const int s = c % L.cols, k = c / L.cols;
        const int64_t X = L.worldX(s % L.nc), Z = L.worldZ(s / L.nc), K = int64_t(L.kb[size_t(s)]) + k;
        const auto side = [&](int64_t X2, int64_t Z2) -> int32_t {
            const int f = L.find(X2, Z2, K);
            if (f >= 0) return f;
            if (f == -2) return kWall;
            return ghost(X2, Z2, K, c);  // outside the ring, or above that column's top
        };
        L.nb[0 * C + size_t(c)] = side(X - 1, Z);
        L.nb[1 * C + size_t(c)] = side(X + 1, Z);
        L.nb[2 * C + size_t(c)] = k == 0 ? kWall : c - L.cols;
        L.nb[3 * C + size_t(c)] = k + 1 < L.ny ? c + L.cols : ghost(X, Z, K + 1, c);
        L.nb[4 * C + size_t(c)] = side(X, Z - 1);
        L.nb[5 * C + size_t(c)] = side(X, Z + 1);
    }

    // The ghosts' baseline: columns outside the ring are sampled once each.
    const int g = L.ghosts();
    L.gv.assign(size_t(kV) * g, 0.0f);
    L.gBase.assign(size_t(kA) * g, 0.0f);
    L.gpBase.assign(size_t(g), 0.0f);
    L.gGround.assign(size_t(g), 0.0);
    if (g == 0) return;
    const double x0 = double(L.ox - 1) * L.dx, x1 = double(L.ox + L.nc + 1) * L.dx;
    const double z0 = double(L.oz - 1) * L.dx, z1 = double(L.oz + L.nc + 1) * L.dx;
    std::vector<Pond> ponds;
    baseline.pondsNear(0.5 * (x0 + x1), 0.5 * (z0 + z1), 0.5 * std::hypot(x1 - x0, z1 - z0) + 100.0, ponds);
    std::vector<MagicCrown> crowns;
    if (l < 2) baseline.crownsInArea(x0 - 12.0, z0 - 12.0, x1 + 12.0, z1 + 12.0, crowns);
    std::map<std::pair<int64_t, int64_t>, int> outside;
    std::vector<std::pair<int64_t, int64_t>> outsideList;
    for (int i = 0; i < g; ++i)
        if (!L.inRing(L.gX[size_t(i)], L.gZ[size_t(i)]) &&
            outside.try_emplace({L.gX[size_t(i)], L.gZ[size_t(i)]}, int(outsideList.size())).second)
            outsideList.push_back({L.gX[size_t(i)], L.gZ[size_t(i)]});
    std::vector<MagicColumn> outsideCols(outsideList.size());
    pool->run(int(outsideList.size()), [&](int i) {
        const double x = (double(outsideList[size_t(i)].first) + 0.5) * L.dx, z = (double(outsideList[size_t(i)].second) + 0.5) * L.dx;
        outsideCols[size_t(i)] = baseline.column(x, z, ponds);
    });
    pool->run(g, [&](int i) {
        const int64_t X = L.gX[size_t(i)], Z = L.gZ[size_t(i)];
        const MagicColumn& col = L.inRing(X, Z) ? L.column[size_t(L.slotColumn(X, Z))] : outsideCols[size_t(outside.at({X, Z}))];
        const double x = (double(X) + 0.5) * L.dx, z = (double(Z) + 0.5) * L.dx, y0 = double(L.gK[size_t(i)]) * L.dy;
        const MagicDensities b = baseline.average(col, x, z, y0, y0 + L.dy, l < 2 ? &crowns : nullptr);
        for (int a = 0; a < kA; ++a) L.gBase[L.gat(a, i)] = b.rho[a];
        L.gpBase[size_t(i)] = float(basePressure(b));
        L.gGround[size_t(i)] = col.surface();
    });
    L.ghostsDirty = true;
}

// Ghost values: the next coarser level where it holds the place, else the baseline at rest with the mean air.
void MagicField::Impl::refreshGhosts(int l) {
    Level& L = lv[l];
    const int g = L.ghosts();
    const bool coarser = l + 1 < levels && lv[l + 1].placed;
    pool->run((g + 255) / 256, [&](int chunk) {
        for (int i = chunk * 256; i < std::min(g, chunk * 256 + 256); ++i) {
            const dvec3 p{(double(L.gX[size_t(i)]) + 0.5) * L.dx, (double(L.gK[size_t(i)]) + 0.5) * L.dy,
                          (double(L.gZ[size_t(i)]) + 0.5) * L.dx};
            MagicSample s;
            if (coarser) s = sampleLevel(l + 1, p);
            if (s.level >= 0) {
                const MagicSample sb = sampleLevel(l + 1, p, true);
                for (int a = 0; a < kA; ++a) L.gv[L.gat(a, i)] = std::max(0.0f, L.gBase[L.gat(a, i)] + s.rho.rho[a] - sb.rho.rho[a]);
                L.gv[L.gat(kU, i)] = float(s.u.x);
                L.gv[L.gat(kU + 1, i)] = float(s.u.y);
                L.gv[L.gat(kU + 2, i)] = float(s.u.z);
                L.gv[L.gat(kTheta, i)] = float(s.theta);
                L.gv[L.gat(kAirT, i)] = float(s.airT);
            } else {
                const dvec3 u = wind ? wind->meanAt(std::max(p.y - L.gGround[size_t(i)], 0.0)) : dvec3{};
                for (int a = 0; a < kA; ++a) L.gv[L.gat(a, i)] = L.gBase[L.gat(a, i)];
                L.gv[L.gat(kU, i)] = float(u.x);
                L.gv[L.gat(kU + 1, i)] = float(u.y);
                L.gv[L.gat(kU + 2, i)] = float(u.z);
                L.gv[L.gat(kTheta, i)] = float(magic::theta0());
                L.gv[L.gat(kAirT, i)] = 0.0f;
            }
        }
    });
    // A ghost off the baseline wakes the brick next to it.
    std::fill(L.wake.begin(), L.wake.end(), uint8_t(0));
    for (int i = 0; i < g; ++i)
        for (int a = 0; a < kA; ++a)
            if (std::fabs(L.gv[L.gat(a, i)] - L.gBase[L.gat(a, i)]) > float(k::MAGIC_QUIET_RHO)) {
                L.wake[size_t(L.brickOf(L.gOwner[size_t(i)]))] = 1;
                break;
            }
}

// Coarse cells under the finer level take its average departure from the baseline there: the finer level is the truth.
void MagicField::Impl::restrictInto(int coarse) {
    const Level& F = lv[coarse - 1];
    Level& C = lv[coarse];
    std::fill(C.covered.begin(), C.covered.end(), uint8_t(0));
    if (!F.placed || !C.placed) return;
    const int r = int(std::lround(C.dx / F.dx));
    std::vector<std::pair<int64_t, int64_t>> cols;
    for (int64_t Z = floorDiv(F.oz + r - 1, r); Z < floorDiv(F.oz + F.nc, r); ++Z)
        for (int64_t X = floorDiv(F.ox + r - 1, r); X < floorDiv(F.ox + F.nc, r); ++X)
            if (C.inRing(X, Z)) cols.push_back({X, Z});
    pool->run(int(cols.size()), [&](int i) {
        const auto [X, Z] = cols[size_t(i)];
        const int cs = C.slotColumn(X, Z);
        double top = 1e300;  // the lowest top among the fine columns: coarse cells above it are not covered
        for (int fz = 0; fz < r; ++fz)
            for (int fx = 0; fx < r; ++fx)
                top = std::min(top, double(F.kb[size_t(F.slotColumn(X * r + fx, Z * r + fz))] + F.ny) * F.dy);
        for (int kc = 0; kc < C.ny; ++kc) {
            const double y0 = double(C.kb[size_t(cs)] + kc) * C.dy, y1 = y0 + C.dy;
            if (y1 > top) break;
            double sum[kV] = {}, dsum[kA] = {};
            int n = 0;
            for (int fz = 0; fz < r; ++fz)
                for (int fx = 0; fx < r; ++fx) {
                    const int fs = F.slotColumn(X * r + fx, Z * r + fz);
                    const int kb = F.kb[size_t(fs)];
                    const int lo = std::max(0, int(std::ceil(y0 / F.dy - 0.5)) - kb);
                    const int hi = std::min(F.ny, int(std::ceil(y1 / F.dy - 0.5)) - kb);
                    for (int k = lo; k < hi; ++k) {
                        const int fc = k * F.cols + fs;
                        for (int v = 0; v < kV; ++v) sum[v] += F.st[F.at(v, fc)];
                        for (int a = 0; a < kA; ++a) dsum[a] += F.base[F.at(a, fc)];
                        ++n;
                    }
                }
            if (n == 0) continue;
            const int cc = kc * C.cols + cs;
            // Densities: the coarse baseline plus the fine level's average departure; the rest as an average.
            for (int a = 0; a < kA; ++a) C.st[C.at(a, cc)] = std::max(0.0f, C.base[C.at(a, cc)] + float((sum[a] - dsum[a]) / n));
            for (int v = kA; v < kV; ++v) C.st[C.at(v, cc)] = float(sum[v] / n);
            C.covered[size_t(cc)] = 1;
        }
    });
}

// The air at the level's brick corners (gusts and eddies included); cells interpolate it.
void MagicField::Impl::airCorners(int l) {
    Level& L = lv[l];
    const int nx = L.nbx + 1, ny = L.nby + 1, n = nx * nx * ny;
    pool->run((n + 31) / 32, [&](int chunk) {
        for (int i = chunk * 32; i < std::min(n, chunk * 32 + 32); ++i) {
            const int bx = i % nx, bz = (i / nx) % nx, by = i / (nx * nx);
            const int64_t X = L.ox + int64_t(bx) * kB, Z = L.oz + int64_t(bz) * kB;
            const int s = L.slotColumn(std::min(X, L.ox + L.nc - 1), std::min(Z, L.oz + L.nc - 1));
            const double y = double(L.kb[size_t(s)] + std::min(by * kB, L.ny)) * L.dy;
            const dvec3 a = airAt({double(X) * L.dx, y, double(Z) * L.dx}, std::max(y - L.column[size_t(s)].surface(), 0.0));
            L.air[size_t(3 * i)] = float(a.x);
            L.air[size_t(3 * i + 1)] = float(a.y);
            L.air[size_t(3 * i + 2)] = float(a.z);
        }
    });
}

MagicSample MagicField::Impl::sampleLevel(int l, const dvec3& p, bool fromBase) const {
    MagicSample out;
    const Level& L = lv[l];
    const int64_t Xp = int64_t(std::floor(p.x / L.dx)), Zp = int64_t(std::floor(p.z / L.dx));
    if (!L.inRing(Xp, Zp)) return out;
    if (p.y >= (double(L.kb[size_t(L.slotColumn(Xp, Zp))]) + L.ny) * L.dy) return out;  // above this level
    const double fx = p.x / L.dx - 0.5, fz = p.z / L.dx - 0.5;
    const int64_t X0 = int64_t(std::floor(fx)), Z0 = int64_t(std::floor(fz));
    const double tx = fx - double(X0), tz = fz - double(Z0);
    double acc[kV] = {};
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const int64_t X = std::clamp(X0 + i, L.ox, L.ox + L.nc - 1), Z = std::clamp(Z0 + j, L.oz, L.oz + L.nc - 1);
            const int s = L.slotColumn(X, Z);
            const double fk = std::clamp(p.y / L.dy - 0.5 - double(L.kb[size_t(s)]), 0.0, double(L.ny - 1));
            const int k0 = std::min(int(fk), std::max(L.ny - 2, 0)), k1 = std::min(k0 + 1, L.ny - 1);
            const double tk = fk - double(k0), w = (i ? tx : 1.0 - tx) * (j ? tz : 1.0 - tz);
            const std::vector<float>& src = fromBase ? L.base : L.st;
            for (int v = 0; v < (fromBase ? kA : kV); ++v)
                acc[v] += w * ((1.0 - tk) * src[L.at(v, k0 * L.cols + s)] + tk * src[L.at(v, k1 * L.cols + s)]);
        }
    for (int a = 0; a < kA; ++a) out.rho.rho[a] = float(acc[a]);
    out.u = {acc[kU], acc[kU + 1], acc[kU + 2]};
    out.theta = acc[kTheta];
    out.airT = acc[kAirT];
    out.level = l;
    return out;
}

// Hot things: the cells around each core hold its fire magic (a Gaussian of MAGIC_FIRE_CORE at full strength, averaged
// over the cell, so every level holds the same mass), hot; the cell holding the core heats its air and the one above
// it sends free fire magic up. Mass and heat rates are the same whatever the cell size.
void MagicField::Impl::applyHeat(int l) {
    Level& L = lv[l];
    const double th0 = magic::theta0();
    const auto resetPressure = [&](int c) {
        double n = 0;
        for (int a = 0; a < kA; ++a) n += L.base[L.at(a, c)] / mass[a];
        L.pBase[size_t(c)] = float(n * L.thetaBase[size_t(c)]);
    };
    for (const int32_t c : L.heated) {
        L.base[L.at(int(Aspect::Fire), c)] = 0.0f;
        L.thetaBase[size_t(c)] = float(th0);
        L.emit[size_t(c)] = 0.0f;
        L.heat[size_t(c)] = 0.0f;
        resetPressure(c);
    }
    L.heated.clear();
    const double kTwoPi15 = 15.749609945722419;  // (2 pi)^1.5
    const double sg = k::MAGIC_FIRE_CORE_SIGMA, volume = L.dx * L.dx * L.dy, l0Volume = double(k::MAGIC_L0_CELL) * k::MAGIC_L0_CELL * k::MAGIC_L0_CELL_Y;
    const double sh2 = sg * sg + L.dx * L.dx / 12.0, sv2 = sg * sg + L.dy * L.dy / 12.0;
    for (const HeatSource& h : heatSources) {
        if (h.strength <= 0.0) continue;
        const double held = k::MAGIC_FIRE_CORE * kTwoPi15 * sg * sg * sg * h.strength;  // kg
        const double peak = held / (kTwoPi15 * sh2 * std::sqrt(sv2));
        const int64_t X = int64_t(std::floor(h.position.x / L.dx)), Z = int64_t(std::floor(h.position.z / L.dx));
        const int64_t K = int64_t(std::floor(h.position.y / L.dy));
        if (!L.inRing(X, Z)) continue;
        const int rh = int(std::ceil(3.0 * std::sqrt(sh2) / L.dx)), rv = int(std::ceil(3.0 * std::sqrt(sv2) / L.dy));
        for (int64_t z = Z - rh; z <= Z + rh; ++z)
            for (int64_t x = X - rh; x <= X + rh; ++x)
                for (int64_t y = K - rv; y <= K + rv; ++y) {
                    const int c = L.find(x, z, y);
                    if (c < 0) continue;
                    const double dx = (double(x) + 0.5) * L.dx - h.position.x, dz = (double(z) + 0.5) * L.dx - h.position.z;
                    const double dy = (double(y) + 0.5) * L.dy - h.position.y;
                    const double fire = peak * std::exp(-0.5 * ((dx * dx + dz * dz) / sh2 + dy * dy / sv2));
                    if (fire < 0.01) continue;
                    L.base[L.at(int(Aspect::Fire), c)] += float(fire);
                    L.heated.push_back(c);
                }
        const int core = L.find(X, Z, K);
        if (core >= 0) {
            const double perCell = l0Volume / volume;  // the same mass and heat whatever the cell
            L.heat[size_t(core)] += float(k::MAGIC_FIRE_HEAT * h.strength * perCell);
            L.heated.push_back(core);
            const int above = core + L.cols < L.cells ? core + L.cols : -1;
            if (above >= 0) {
                L.emit[size_t(above)] += float(k::MAGIC_FIRE_EMIT * h.strength * perCell);
                L.heat[size_t(above)] += float(0.5 * k::MAGIC_FIRE_HEAT * h.strength * perCell);
                L.heated.push_back(above);
            }
        }
    }
    // Held fire is born hot: its cells are held at a higher temperature, and their baseline pressure with it.
    for (const int32_t c : L.heated) {
        double total = 0;
        for (int a = 0; a < kA; ++a) total += L.base[L.at(a, c)];
        const double share = total > 1e-6 ? L.base[L.at(int(Aspect::Fire), c)] / total : 0.0;
        L.thetaBase[size_t(c)] = float(th0 * (1.0 + (double(k::MAGIC_FIRE_BIRTH_THETA) - 1.0) * share));
        resetPressure(c);
        L.deviation[size_t(L.brickOf(c))] = 1e9f;  // a fire's bricks always step
    }
}

// ---------------------------------------------------------------------------------------------- the public face

MagicField::MagicField(const Terrain* terrain, const WindField* wind, const MagicOptions& o)
    : impl_(std::make_unique<Impl>(terrain, wind, o)) {}

MagicField::~MagicField() = default;

void MagicField::setFocus(const dvec3& p) {
    impl_->focus = p;
    impl_->hasFocus = true;
}

bool MagicField::started() const { return impl_->started; }

void MagicField::setHeat(const std::vector<HeatSource>& sources) { impl_->heatSources = sources; }

void MagicField::setDrops(const std::vector<DropSource>& sources) { impl_->dropSources = sources; }

const std::vector<Manifestation>& MagicField::manifestations() const { return impl_->manifestations; }
std::vector<Manifestation>& MagicField::manifestations() { return impl_->manifestations; }
const std::vector<Firefly>& MagicField::fireflies() const { return impl_->fireflies; }
const std::vector<Flame>& MagicField::flames() const { return impl_->flames; }
const std::vector<HotSpot>& MagicField::hotSpots() const { return impl_->hotSpots; }

bool MagicField::denseBox(dvec3& lo, dvec3& hi) const {
    lo = impl_->denseLo;
    hi = impl_->denseHi;
    return impl_->dense;
}

void MagicField::step(int64_t stepIndex) {
    Impl& m = *impl_;
    if (!m.hasFocus) return;
    if (!m.started) {
        for (int l = m.levels - 1; l >= 0; --l) m.place(l);  // coarse first: finer levels fill from them
        m.started = true;
    }
    for (int l = m.levels - 1; l >= 0; --l) {
        Level& L = m.lv[l];
        if (posMod(stepIndex - L.offset, L.every) != 0) continue;
        m.place(l);
        m.tick(l);
        ++m.stats.ticks[l];
    }
}

MagicSample MagicField::sample(const dvec3& p) const {
    for (int l = 0; l < impl_->levels; ++l) {
        const MagicSample s = impl_->sampleLevel(l, p);
        if (s.level >= 0) return s;
    }
    MagicSample s;
    s.rho = impl_->baseline.at(p);
    s.theta = magic::theta0();
    return s;
}

const MagicBaseline& MagicField::baseline() const { return impl_->baseline; }

uint64_t MagicField::stateHash() const {
    uint64_t h = 0x6d61676963ull;
    for (int l = 0; l < impl_->levels; ++l) {
        const Level& L = impl_->lv[l];
        h = hashCombine(h, uint64_t(L.ox));
        h = hashCombine(h, uint64_t(L.oz));
        for (const float v : L.st) h = hashCombine(h, std::bit_cast<uint32_t>(v));
    }
    return h;
}

double MagicField::totalMass(Aspect a) const {
    double sum = 0;
    for (int l = 0; l < impl_->levels; ++l) {
        const Level& L = impl_->lv[l];
        double level = 0;
        for (int c = 0; c < L.cells; ++c)
            if (!L.covered[size_t(c)]) level += L.st[L.at(int(a), c)];
        sum += level * L.dx * L.dx * L.dy;
    }
    return sum;
}

double MagicField::minDensity() const {
    float m = 1e30f;
    for (int l = 0; l < impl_->levels; ++l) {
        const Level& L = impl_->lv[l];
        for (int a = 0; a < kA; ++a)
            for (int c = 0; c < L.cells; ++c) m = std::min(m, L.st[L.at(a, c)]);
    }
    return m;
}

bool MagicField::addDensity(const dvec3& p, Aspect a, double kgPerM3) {
    for (int l = 0; l < impl_->levels; ++l) {
        Level& L = impl_->lv[l];
        const int c = L.find(int64_t(std::floor(p.x / L.dx)), int64_t(std::floor(p.z / L.dx)), int64_t(std::floor(p.y / L.dy)));
        if (c < 0) continue;
        L.st[L.at(int(a), c)] += float(kgPerM3);
        L.deviation[size_t(L.brickOf(c))] = 1e9f;
        ++L.version;
        return true;
    }
    return false;
}

bool MagicField::addMass(const dvec3& p, Aspect a, double kg) {
    for (int l = 0; l < impl_->levels; ++l) {
        const Level& L = impl_->lv[l];
        if (L.find(int64_t(std::floor(p.x / L.dx)), int64_t(std::floor(p.z / L.dx)), int64_t(std::floor(p.y / L.dy))) >= 0)
            return addDensity(p, a, kg / (L.dx * L.dx * L.dy));
    }
    return false;
}

void MagicField::setRain(double rain) { impl_->rain = rain; }

MagicLevelView MagicField::view(int level) const {
    MagicLevelView v;
    if (level < 0 || level >= impl_->levels) return v;
    const Level& L = impl_->lv[level];
    v.dx = L.dx;
    v.dy = L.dy;
    v.columns = L.nc;
    v.layers = L.ny;
    v.originX = L.ox;
    v.originZ = L.oz;
    v.columnBase = L.kb.data();
    v.state = L.st.data();
    v.version = L.version;
    return v;
}

const MagicStats& MagicField::stats() const { return impl_->stats; }

int MagicField::threads() const { return impl_->pool->threads(); }

}  // namespace taelia
