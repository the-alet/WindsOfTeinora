// The magic field's level tick: which bricks step, sub-steps by the fastest wave and the strongest diffusion, then per
// sub-step: derived values, face fluxes (MUSCL/minmod advection by the face velocity and Fick diffusion, both of the
// departure from the baseline: the land, the water and the plants hold the rest), a positivity limit, and the update (densities, velocity, Theta, the air's temperature, the
// taking rule). Jacobi throughout: every pass reads the old state and writes only its own outputs, so the result
// never depends on the thread count. Each face's flux is applied with opposite signs to its two cells.
#include <algorithm>
#include <cmath>

#include "magic_internal.h"
#include "taelia/sim/fmath.h"

namespace taelia {

using namespace mf;

namespace {

inline float minmod(float a, float b) { return a * b <= 0.0f ? 0.0f : (std::fabs(a) < std::fabs(b) ? a : b); }

constexpr int kTakes = 6;
constexpr int kTakeAspect[kTakes] = {int(Aspect::Earth), int(Aspect::Water), int(Aspect::Nature), int(Aspect::Air), int(Aspect::Fire),
                                     int(Aspect::Light)};
const double kTakeTau[kTakes] = {k::MAGIC_TAKE_TAU_EARTH, k::MAGIC_TAKE_TAU_WATER, k::MAGIC_TAKE_TAU_NATURE, k::MAGIC_TAKE_TAU_AIR,
                                 k::MAGIC_TAKE_TAU_FIRE, k::MAGIC_TAKE_TAU_LIGHT};
constexpr int kFire = int(Aspect::Fire);
constexpr int kCondAspect[6] = {int(Aspect::Earth), int(Aspect::Water), int(Aspect::Nature), int(Aspect::Fire), int(Aspect::Air),
                                int(Aspect::Light)};
constexpr int kNeutral = int(Aspect::Neutral);

}  // namespace

// Total density, pressure off the baseline, speed of sound, kinematic viscosity, and D per aspect (the mean free path
// from all the motes, plus the air's molecules capping it in thin magic).
void MagicField::Impl::derive(const float* rho, double theta, double pBase, float* out) const {
    double rhoTot = 0, n = 0;
    for (int a = 0; a < kA; ++a) {
        rhoTot += rho[a];
        n += rho[a] / mass[a];
    }
    const double p = n * theta;
    out[dRho] = float(rhoTot);
    out[dP] = float(p - pBase);
    out[dC] = float(rhoTot > 1e-9 ? std::sqrt(double(k::MAGIC_GAMMA) * p / rhoTot) : 0.0);
    double nu = 0;
    for (int a = 0; a < kA; ++a) {
        const double v = std::sqrt(3.0 * theta / mass[a]);
        const double lambda = 1.0 / (1.4142135623730951 * sigma[a] * n + v / (3.0 * double(k::MAGIC_D_MAX)));
        const double D = v * lambda / 3.0;
        out[dD + a] = float(D);
        nu += rho[a] * D;
    }
    out[dNu] = float(rhoTot > 1e-9 ? nu / rhoTot : 0.0);
}

void MagicField::Impl::tick(int l) {
    Level& L = lv[l];
    // Only what changed: the finer level's departures, the ghosts when the coarser level (or the ring) changed.
    if (l > 0 && lv[l - 1].placed && lv[l - 1].version != L.restrictedFrom) {
        restrictInto(l);
        L.restrictedFrom = lv[l - 1].version;
        ++L.version;
    }
    applyHeat(l);
    if (l == 0) drops(l, double(L.every) / double(k::SIM_STEP_HZ), stats.ticks[l]);
    if (l == 0) raindrops(l, double(L.every) / double(k::SIM_STEP_HZ), stats.ticks[l]);
    const bool outermost = l + 1 >= levels;
    if (L.ghostsDirty || outermost || lv[l + 1].version != L.ghostsFrom) {
        refreshGhosts(l);
        L.ghostsDirty = false;
        if (!outermost) L.ghostsFrom = lv[l + 1].version;
    }
    const size_t C = size_t(L.cells);
    const int g = L.ghosts(), nbx = L.nbx, chunks = L.ny * nbx;
    const float h[3] = {float(L.dx), float(L.dy), float(L.dx)};
    const auto brickIndex = [&](int sx, int sz, int k) { return ((k / kB) * nbx + sz / kB) * nbx + sx / kB; };

    // Which bricks step: off the baseline, woken by a ghost, or next to such a brick (neighbours in the world).
    std::vector<uint8_t> hot(size_t(L.bricks));
    for (int b = 0; b < L.bricks; ++b) hot[size_t(b)] = !opt.skipQuiet || L.deviation[size_t(b)] > float(k::MAGIC_QUIET_RHO) || L.wake[size_t(b)];
    const int64_t obx = floorDiv(L.ox, kB), obz = floorDiv(L.oz, kB);
    const auto brickNeighbour = [&](int bx, int by, int bz, int dx, int dy, int dz) -> int {
        const int64_t wx = obx + posMod(bx - obx, nbx) + dx, wz = obz + posMod(bz - obz, nbx) + dz;
        const int y = by + dy;
        if (wx < obx || wx >= obx + nbx || wz < obz || wz >= obz + nbx || y < 0 || y >= L.nby) return -1;
        return (y * nbx + posMod(wz, nbx)) * nbx + posMod(wx, nbx);
    };
    int activeBricks = 0;
    for (int b = 0; b < L.bricks; ++b) {
        const int bx = b % nbx, bz = (b / nbx) % nbx, by = b / (nbx * nbx);
        bool on = hot[size_t(b)];
        const int d6[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
        for (int i = 0; i < 6 && !on; ++i) {
            const int n = brickNeighbour(bx, by, bz, d6[i][0], d6[i][1], d6[i][2]);
            on = n >= 0 && hot[size_t(n)];
        }
        L.active[size_t(b)] = on;
        activeBricks += on;
    }
    stats.bricksSkipped += L.bricks - activeBricks;
    if (activeBricks == 0) return;  // an undisturbed level costs nothing
    airCorners(l);
    // Derived values are needed on the stepped cells and their neighbours: one more brick around.
    std::vector<uint8_t> near(size_t(L.bricks));
    for (int b = 0; b < L.bricks; ++b) {
        const int bx = b % nbx, bz = (b / nbx) % nbx, by = b / (nbx * nbx);
        bool on = L.active[size_t(b)];
        const int d6[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
        for (int i = 0; i < 6 && !on; ++i) {
            const int n = brickNeighbour(bx, by, bz, d6[i][0], d6[i][1], d6[i][2]);
            on = n >= 0 && L.active[size_t(n)];
        }
        near[size_t(b)] = on;
    }

    // Values on either side of a face: a cell of the level or a ghost.
    gDer.assign(size_t(kDer) * g, 0.0f);
    const auto rhoOf = [&](int32_t r, int a) { return r >= 0 ? L.st[L.at(a, r)] : L.gv[L.gat(a, ghostIndex(r))]; };
    const auto baseOf = [&](int32_t r, int a) { return r >= 0 ? L.base[L.at(a, r)] : L.gBase[L.gat(a, ghostIndex(r))]; };
    const auto derOf = [&](int32_t r, int d) {
        return r >= 0 ? der[size_t(d) * C + size_t(r)] : gDer[size_t(d) * g + size_t(ghostIndex(r))];
    };
    const auto deriveCells = [&]() {
        pool->run(chunks, [&](int ch) {
            const int k = ch / nbx, zr = ch % nbx;
            float rho[kA], out[kDer];
            for (int sz = zr * kB; sz < zr * kB + kB; ++sz)
                for (int sx = 0; sx < L.nc; ++sx) {
                    if (!near[size_t(brickIndex(sx, sz, k))]) continue;
                    const int c = L.cell(sx, sz, k);
                    for (int a = 0; a < kA; ++a) rho[a] = L.st[L.at(a, c)];
                    derive(rho, L.st[L.at(kTheta, c)], L.pBase[size_t(c)], out);
                    for (int d = 0; d < kDer; ++d) der[size_t(d) * C + size_t(c)] = out[d];
                }
        });
    };
    pool->run((g + 255) / 256, [&](int chunk) {
        float rho[kA], out[kDer];
        for (int i = chunk * 256; i < std::min(g, chunk * 256 + 256); ++i) {
            for (int a = 0; a < kA; ++a) rho[a] = L.gv[L.gat(a, i)];
            derive(rho, L.gv[L.gat(kTheta, i)], L.gpBase[size_t(i)], out);
            for (int d = 0; d < kDer; ++d) gDer[size_t(d) * g + size_t(i)] = out[d];
        }
    });
    deriveCells();

    // Sub-steps: the fastest wave (|u| + sound) and the strongest diffusion among the stepped cells.
    chunkMax.assign(size_t(2 * chunks), 0.0f);
    pool->run(chunks, [&](int ch) {
        const int k = ch / nbx, zr = ch % nbx;
        float wave = 0, diff = 0;
        for (int sz = zr * kB; sz < zr * kB + kB; ++sz)
            for (int sx = 0; sx < L.nc; ++sx) {
                if (!L.active[size_t(brickIndex(sx, sz, k))]) continue;
                const int c = L.cell(sx, sz, k);
                const float ux = L.st[L.at(kU, c)], uy = L.st[L.at(kU + 1, c)], uz = L.st[L.at(kU + 2, c)];
                wave = std::max(wave, std::sqrt(ux * ux + uy * uy + uz * uz) + der[size_t(dC) * C + size_t(c)]);
                for (int a = 0; a < kA; ++a) diff = std::max(diff, der[size_t(dD + a) * C + size_t(c)]);
                diff = std::max(diff, der[size_t(dNu) * C + size_t(c)]);
            }
        chunkMax[size_t(2 * ch)] = wave;
        chunkMax[size_t(2 * ch + 1)] = diff;
    });
    double wave = 0, diff = 0;
    for (int ch = 0; ch < chunks; ++ch) {
        wave = std::max(wave, double(chunkMax[size_t(2 * ch)]));
        diff = std::max(diff, double(chunkMax[size_t(2 * ch + 1)]));
    }
    const double dtTick = double(L.every) / double(k::SIM_STEP_HZ), hmin = std::min(L.dx, L.dy);
    const double need = std::max(wave * dtTick / (double(k::MAGIC_CFL) * hmin),
                                 6.0 * diff * dtTick / (double(k::MAGIC_DIFFUSION_LIMIT) * hmin * hmin));
    const int n = std::clamp(int(std::ceil(need - 1e-9)), 1, int(k::MAGIC_MAX_SUBSTEPS));
    const double dt = dtTick / n;

    // The air at a cell, from the brick corners.
    const int ax = nbx + 1;
    const auto cellAir = [&](int sx, int sz, int k, float out[3]) {
        const double rx = (posMod(sx - L.ox, L.nc) + 0.5) / kB, rz = (posMod(sz - L.oz, L.nc) + 0.5) / kB, ry = (k + 0.5) / kB;
        const int ix = std::min(int(rx), nbx - 1), iz = std::min(int(rz), nbx - 1), iy = std::min(int(ry), L.nby - 1);
        const double tx = rx - ix, tz = rz - iz, ty = std::min(ry - iy, 1.0);
        for (int i = 0; i < 3; ++i) {
            double v = 0;
            for (int cy = 0; cy < 2; ++cy)
                for (int cz = 0; cz < 2; ++cz)
                    for (int cx = 0; cx < 2; ++cx) {
                        const double w = (cx ? tx : 1 - tx) * (cz ? tz : 1 - tz) * (cy ? ty : 1 - ty);
                        v += w * L.air[size_t(3 * (((iy + cy) * ax + iz + cz) * ax + ix + cx) + i)];
                    }
            out[i] = float(v);
        }
    };
    const auto steps = [&](int32_t r) { return r >= 0 && L.active[size_t(L.brickOf(r))]; };
    // The flux through the face between A (on the - side) and B (on the + side), per aspect, kg m^-2 s^-1.
    const auto faceFlux = [&](int32_t A, int32_t B, int d, float* out) {
        const float uf = 0.5f * ((A >= 0 ? L.st[L.at(kU + d, A)] : L.gv[L.gat(kU + d, ghostIndex(A))]) +
                                 (B >= 0 ? L.st[L.at(kU + d, B)] : L.gv[L.gat(kU + d, ghostIndex(B))]));
        const int32_t AA = A >= 0 ? L.nb[size_t(2 * d) * C + size_t(A)] : kWall;
        const int32_t BB = B >= 0 ? L.nb[size_t(2 * d + 1) * C + size_t(B)] : kWall;
        const auto dev = [&](int32_t r, int a) { return rhoOf(r, a) - baseOf(r, a); };
        for (int a = 0; a < kA; ++a) {
            const float ea = dev(A, a), eb = dev(B, a);
            float face;
            if (uf >= 0.0f) face = ea + (AA != kWall ? 0.5f * minmod(ea - dev(AA, a), eb - ea) : 0.0f);
            else face = eb + (BB != kWall ? 0.5f * minmod(eb - dev(BB, a), ea - eb) : 0.0f);
            const float da = derOf(A, dD + a), db = derOf(B, dD + a);
            const float df = da + db > 0.0f ? 2.0f * da * db / (da + db) : 0.0f;
            out[a] = uf * face - df * (eb - ea) / h[d];
        }
    };
    const auto lowFlux = [&](int c, int d, int a) -> float {  // through c's - face
        const int32_t nm = L.nb[size_t(2 * d) * C + size_t(c)];
        if (nm == kWall) return 0.0f;
        return steps(nm) ? flux[(size_t(d) * kA + a) * C + size_t(nm)] : fluxLo[(size_t(d) * kA + a) * C + size_t(c)];
    };
    double take[kTakes];
    for (int j = 0; j < kTakes; ++j) take[j] = 1.0 - fmath::exp(-dt / kTakeTau[j]);
    const double hold = 1.0 - fmath::exp(-dt / double(k::MAGIC_FIRE_HOLD_TAU)), updraft = k::MAGIC_UPDRAFT;
    const double condense = 1.0 - fmath::exp(-dt / double(k::MAGIC_CONDENSE_TAU)), evaporate = 1.0 - fmath::exp(-dt / double(k::MAGIC_EVAPORATE_TAU));
    float threshold[6];
    for (int m = 0; m < 6; ++m) threshold[m] = float(magic::showThreshold(Aspect(kCondAspect[m])));
    const double thetaKeep = fmath::exp(-dt / double(k::MAGIC_THETA_TAU)), airTKeep = fmath::exp(-dt / double(k::MAGIC_AIR_T_TAU));
    const double theta0 = magic::theta0(), buoyancy = 9.81 / double(k::MAGIC_AIR_T_REF);

    const auto forActive = [&](auto&& fn) {
        pool->run(chunks, [&](int ch) {
            const int k = ch / nbx, zr = ch % nbx;
            for (int sz = zr * kB; sz < zr * kB + kB; ++sz)
                for (int sx = 0; sx < L.nc; ++sx)
                    if (L.active[size_t(brickIndex(sx, sz, k))]) fn(sx, sz, k, L.cell(sx, sz, k));
        });
    };

    for (int sub = 0; sub < n; ++sub) {
        if (sub > 0) deriveCells();
        // Fluxes through each stepped cell's + faces, and its - faces where the other side does not step.
        forActive([&](int, int, int, int c) {
            float f[kA];
            for (int d = 0; d < 3; ++d) {
                const int32_t np = L.nb[size_t(2 * d + 1) * C + size_t(c)];
                if (np == kWall) std::fill(f, f + kA, 0.0f);
                else faceFlux(c, np, d, f);
                for (int a = 0; a < kA; ++a) flux[(size_t(d) * kA + a) * C + size_t(c)] = f[a];
                const int32_t nm = L.nb[size_t(2 * d) * C + size_t(c)];
                if (nm != kWall && !steps(nm)) {
                    faceFlux(nm, c, d, f);
                    for (int a = 0; a < kA; ++a) fluxLo[(size_t(d) * kA + a) * C + size_t(c)] = f[a];
                }
            }
        });
        // No negatives: a cell gives at most what it holds; its outflows shrink together.
        forActive([&](int, int, int, int c) {
            for (int a = 0; a < kA; ++a) {
                double out = 0;
                for (int d = 0; d < 3; ++d) {
                    const float fp = flux[(size_t(d) * kA + a) * C + size_t(c)], fm = lowFlux(c, d, a);
                    if (fp > 0.0f) out += fp / h[d];
                    if (fm < 0.0f) out -= fm / h[d];
                }
                out *= dt;
                const double rho = L.st[L.at(a, c)];
                keep[size_t(a) * C + size_t(c)] = out > rho && out > 0.0 ? float(rho / out) : 1.0f;
            }
        });
        // The update.
        pool->run(chunks, [&](int ch) {
            const int k = ch / nbx, zr = ch % nbx;
            for (int sz = zr * kB; sz < zr * kB + kB; ++sz)
                for (int sx = 0; sx < L.nc; ++sx) {
                    const int c = L.cell(sx, sz, k);
                    if (!L.active[size_t(brickIndex(sx, sz, k))]) continue;
                    float rho[kA];
                    double nTot = 0;
                    for (int a = 0; a < kA; ++a) {
                        double change = 0;
                        for (int d = 0; d < 3; ++d) {
                            const int32_t np = L.nb[size_t(2 * d + 1) * C + size_t(c)], nm = L.nb[size_t(2 * d) * C + size_t(c)];
                            const float fp = flux[(size_t(d) * kA + a) * C + size_t(c)], fm = lowFlux(c, d, a);
                            const float kp = fp > 0.0f ? keep[size_t(a) * C + size_t(c)] : (steps(np) ? keep[size_t(a) * C + size_t(np)] : 1.0f);
                            const float km = fm > 0.0f ? (steps(nm) ? keep[size_t(a) * C + size_t(nm)] : 1.0f) : keep[size_t(a) * C + size_t(c)];
                            change += (double(fm) * km - double(fp) * kp) / h[d];
                        }
                        rho[a] = std::max(0.0f, float(L.st[L.at(a, c)] + dt * change));
                    }
                    // The taking rule: neutral becomes an aspect below its baseline, and the aspect neutral above it.
                    // A hot thing makes the fire magic it holds (it comes from the heat, not from the neutral).
                    const float heldFire = L.base[L.at(kFire, c)];
                    if (rho[kFire] < heldFire) rho[kFire] += float((heldFire - rho[kFire]) * hold);
                    if (opt.takingRule) {
                        for (int j = 0; j < kTakes; ++j) {
                            const int a = kTakeAspect[j];
                            const float target = L.base[L.at(a, c)];
                            if (rho[a] < target) {
                                const float m = std::min(rho[kNeutral], float((target - rho[a]) * take[j]));
                                rho[a] += m;
                                rho[kNeutral] -= m;
                            } else {
                                const float m = float((rho[a] - target) * take[j]);
                                rho[a] -= m;
                                rho[kNeutral] += m;
                            }
                        }
                    }
                    // Nature drinks water magic: half of it grows nature, half goes back to neutral.
                    {
                        const int w = int(Aspect::Water), nat = int(Aspect::Nature);
                        const float drink = std::min(rho[w], float(double(k::MAGIC_NATURE_DRINK) * rho[nat] * rho[w] * dt));
                        rho[w] -= drink;
                        rho[nat] += 0.5f * drink;
                        rho[kNeutral] += 0.5f * drink;
                    }
                    // Free fire magic rising from a fire, born hot.
                    double thetaIn = L.st[L.at(kTheta, c)];
                    if (L.emit[size_t(c)] > 0.0f) {
                        double nOld = 0;
                        for (int a = 0; a < kA; ++a) nOld += rho[a] / mass[a];
                        const double add = double(L.emit[size_t(c)]) * dt, nAdd = add / mass[kFire];
                        rho[kFire] += float(add);
                        thetaIn = (nOld * thetaIn + nAdd * theta0 * double(k::MAGIC_FIRE_BIRTH_THETA)) / std::max(nOld + nAdd, 1e-30);
                    }
                    // Condensation: past its threshold an aspect condenses into its manifestation, which evaporates back
                    // when the surroundings thin (the finest level only).
                    if (!L.cond.empty()) {
                        for (int m = 0; m < 6; ++m) {
                            const int a = kCondAspect[m];
                            float& cond = L.cond[size_t(m) * C + size_t(c)];
                            if (rho[a] > threshold[m]) {
                                const float q = float((rho[a] - threshold[m]) * condense);
                                rho[a] -= q;
                                cond += q;
                            } else if (cond > 0.0f) {
                                const float q = std::min(cond, float((threshold[m] - rho[a]) * evaporate));
                                cond -= q;
                                rho[a] += q;
                            }
                        }
                    }
                    for (int a = 0; a < kA; ++a) {
                        L.nx[L.at(a, c)] = rho[a];
                        nTot += rho[a] / mass[a];
                    }
                    // Velocity: the pressure off the baseline pushes, viscosity smooths, the air drags it along (implicit),
                    // warm air lifts it.
                    const float rhoT = std::max(der[size_t(dRho) * C + size_t(c)], 1e-3f), pc = der[size_t(dP) * C + size_t(c)];
                    const float nu = der[size_t(dNu) * C + size_t(c)];
                    float u[3], air[3], accel[3] = {0, 0, 0};
                    for (int i = 0; i < 3; ++i) u[i] = L.st[L.at(kU + i, c)];
                    cellAir(sx, sz, k, air);
                    const float th = float(thetaIn), airT = L.st[L.at(kAirT, c)];
                    air[1] += float(std::min(updraft * std::max(airT, 0.0f), double(k::MAGIC_UPDRAFT_MAX)));  // warm air rises
                    double thAdv = 0, tAdv = 0;
                    for (int d = 0; d < 3; ++d) {
                        const int32_t np = L.nb[size_t(2 * d + 1) * C + size_t(c)], nm = L.nb[size_t(2 * d) * C + size_t(c)];
                        const float pp = np == kWall ? pc : derOf(np, dP), pm = nm == kWall ? pc : derOf(nm, dP);
                        accel[d] -= (pp - pm) / (2.0f * h[d]) / rhoT;
                        for (int i = 0; i < 3; ++i) {
                            const float up = np == kWall ? u[i] : (np >= 0 ? L.st[L.at(kU + i, np)] : L.gv[L.gat(kU + i, ghostIndex(np))]);
                            const float um = nm == kWall ? u[i] : (nm >= 0 ? L.st[L.at(kU + i, nm)] : L.gv[L.gat(kU + i, ghostIndex(nm))]);
                            accel[i] += nu * (up - 2.0f * u[i] + um) / (h[d] * h[d]);
                        }
                        // Theta rides the magic, the air's temperature the air (first-order upwind).
                        const auto thOf = [&](int32_t r, int v) {
                            if (r == kWall) return v == kTheta ? float(theta0) : 0.0f;  // the ground: ambient
                            return r >= 0 ? L.st[L.at(v, r)] : L.gv[L.gat(v, ghostIndex(r))];
                        };
                        thAdv += u[d] > 0.0f ? u[d] * (th - thOf(nm, kTheta)) / h[d] : u[d] * (thOf(np, kTheta) - th) / h[d];
                        tAdv += air[d] > 0.0f ? air[d] * (airT - thOf(nm, kAirT)) / h[d] : air[d] * (thOf(np, kAirT) - airT) / h[d];
                    }
                    accel[1] += float(buoyancy * airT);
                    double tauMix = 0;
                    for (int a = 0; a < kA; ++a) tauMix += rho[a] * tau[a];
                    tauMix = tauMix > 0.0 ? tauMix / std::max(double(rhoT), 1e-6) : tau[int(Aspect::Air)];
                    const double r = dt / tauMix;
                    for (int i = 0; i < 3; ++i) L.nx[L.at(kU + i, c)] = float((u[i] + dt * accel[i] + r * air[i]) / (1.0 + r));
                    // Theta relaxes to the ambient; the energy it gives up warms the air, which cools in turn.
                    const double thHeld = L.thetaBase[size_t(c)];
                    const double thA = th - dt * thAdv, thR = thHeld + (thA - thHeld) * thetaKeep;
                    const double heat = 1.5 * nTot * (thA - thR) / double(k::MAGIC_AIR_HEAT_CAPACITY);
                    L.nx[L.at(kTheta, c)] = float(thR);
                    L.nx[L.at(kAirT, c)] = float((airT - dt * tAdv + heat + dt * L.heat[size_t(c)]) * airTKeep);
                }
        });
        forActive([&](int, int, int, int c) {
            for (int v = 0; v < kV; ++v) L.st[L.at(v, c)] = L.nx[L.at(v, c)];
        });
    }

    // How far each brick is from its baseline, for the next tick's choice.
    pool->run(L.bricks, [&](int b) {
        if (!L.active[size_t(b)]) return;
        const int bx = b % nbx, bz = (b / nbx) % nbx, by = b / (nbx * nbx);
        float dev = 0;
        for (int k = by * kB; k < std::min(by * kB + kB, L.ny); ++k)
            for (int sz = bz * kB; sz < bz * kB + kB; ++sz)
                for (int sx = bx * kB; sx < bx * kB + kB; ++sx) {
                    const int c = L.cell(sx, sz, k);
                    for (int a = 0; a < kA; ++a) dev = std::max(dev, std::fabs(L.st[L.at(a, c)] - L.base[L.at(a, c)]));
                    if (!L.cond.empty())
                        for (int m = 0; m < 6; ++m) dev = std::max(dev, L.cond[size_t(m) * C + size_t(c)]);  // condensate waits to evaporate
                }
        L.deviation[size_t(b)] = dev;
        // Going quiet: what the taking rule would still do, done at once (each aspect to its baseline, the
        // difference to or from neutral; no mass lost), so nothing lingers frozen below the threshold.
        if (dev >= float(k::MAGIC_QUIET_RHO) || !opt.takingRule) return;
        for (int k = by * kB; k < std::min(by * kB + kB, L.ny); ++k)
            for (int sz = bz * kB; sz < bz * kB + kB; ++sz)
                for (int sx = bx * kB; sx < bx * kB + kB; ++sx) {
                    const int c = L.cell(sx, sz, k);
                    float& neutral = L.st[L.at(kNeutral, c)];
                    for (int j = 0; j < kTakes; ++j) {
                        const int a = kTakeAspect[j];
                        float& rho = L.st[L.at(a, c)];
                        const float move = std::max(rho - L.base[L.at(a, c)], -neutral);
                        rho -= move;
                        neutral += move;
                    }
                }
    });
    if (l == 0) manifest(l, stats.ticks[l]);
    stats.cellUpdates += int64_t(activeBricks) * kB * kB * kB * n;
    stats.lastSubsteps[l] = n;
    ++L.version;
}

}  // namespace taelia
