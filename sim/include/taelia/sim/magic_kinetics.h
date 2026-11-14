// Magic as a gas of motes: the aspects and their kinetic coefficients (SPEC.md, "As a gas: molecular-kinetic theory").
// All in SI units from shared/constants.txt (MAGIC_*). Pure functions: the field, the HUD and the tests share them.
#pragma once
#include <cstdint>

namespace taelia {

// Life is the aspect of living things (once nature: the user made them one). Necrotic (the user) comes last.
enum class Aspect : uint8_t { Earth, Water, Life, Fire, Air, Lightning, Light, Neutral, Necrotic, Count };
inline constexpr int kAspects = int(Aspect::Count);
const char* aspectName(Aspect a);  // "earth", ...

// Nine densities in kg/m3, in Aspect order.
struct MagicDensities {
    float rho[kAspects] = {};
    float operator[](Aspect a) const { return rho[int(a)]; }
    float& operator[](Aspect a) { return rho[int(a)]; }
    double total() const;
};

namespace magic {

double moteMass(Aspect a);         // kg
double theta0();                   // J: the ambient magic temperature
double showThreshold(Aspect a);    // kg/m3 where the aspect manifests (n*b = 1/3); 0 for neutral (never condenses)
double moteDiameter(Aspect a);     // m, van der Waals: n*b = 1/3 at the threshold; neutral uses air's
double crossSection(Aspect a);     // m2, pi d^2
double excludedVolume(Aspect a);   // m3 per mote, b = 2/3 pi d^3
double moteSpeed(Aspect a, double theta);                   // m/s, sqrt(3 Theta/m)
double soundSpeed(Aspect a, double theta);                  // m/s, sqrt(gamma Theta/m)
double numberDensity(Aspect a, double rho);                 // motes per m3
double partialPressure(Aspect a, double rho, double theta);  // Pa, rho Theta/m = rho v^2/3
// Mean free path in a gas of nTot motes per m3: 1/lambda = sqrt(2) sigma nTot + v/(3 D_MAX) (the second term is
// collisions with the air's molecules, which cap D at D_MAX in thin magic).
double meanFreePath(Aspect a, double nTot, double theta);
double diffusivity(Aspect a, double nTot, double theta);  // m2/s, v lambda/3
double dynamicViscosity(const MagicDensities& d, double theta);    // Pa s, sum of rho_a v_a lambda_a / 3
double kinematicViscosity(const MagicDensities& d, double theta);  // m2/s
double totalNumberDensity(const MagicDensities& d);
double totalPressure(const MagicDensities& d, double theta);  // Dalton
double mixtureSoundSpeed(const MagicDensities& d, double theta);
double inertiaTau(Aspect a);  // s: the time the aspect takes to follow the air
// Effusion through a seal (runes): motes per m2 per second, p / sqrt(2 pi m Theta) (Graham: faster for light motes).
double effusionFlux(Aspect a, double pressure, double theta);

// Changes under pressure, on the aspect's own partial pressure (answers_so_far): fire past the pressure of 675 kg/m3
// at THETA0 turns to lightning, past 1100 to light; lightning past the same light pressure turns to light.
enum class Conversion : uint8_t { None, ToLightning, ToLight };
Conversion fireConversion(double rhoFire, double theta);
Conversion lightningConversion(double rhoLightning, double theta);
double fireToLightningPressure();  // Pa
double toLightPressure();          // Pa

// Direct force on things: proportional to (rho_tot - 50)+, exactly 0 at or below the threshold.
double effectFactor(double rhoTotal);
// Visible past the dew point.
bool visible(double rhoTotal);
// What an object holds (kg/m3 of its own aspect) evens out with the total magic around it, toward MAGIC_HELD at most;
// below MAGIC_FALL_APART of MAGIC_HELD it falls apart (so in ordinary places nothing does).
double heldAfter(double held, double around, double dt);
// The same evening out (MAGIC_HELD_TAU) toward any target: water a living thing holds, necrotic magic it takes up.
double heldToward(double held, double target, double dt);
bool fallsApart(double held);
// Necrotic magic absorbs life magic at MAGIC_NECROTIC_ABSORB * necrotic * life: what a living thing holds of life
// magic (kg/m3) after dt among necrotic magic this dense.
double lifeLeft(double held, double necrotic, double dt);
// Fireflies from light magic of this density (SPEC: 3 at 40, one more for every 3 past it; ten merge into a big one).
int fireflyCount(double light);
void fireflyGroups(double light, int& big, int& small);

}  // namespace magic

}  // namespace taelia
