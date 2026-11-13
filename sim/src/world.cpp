#include "taelia/sim/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "taelia/shared_constants.h"
#include "taelia/sim/hash.h"
#include "taelia/sim/sky.h"

namespace taelia {

namespace {

MagicOptions magicOptions(const WorldDesc& d) {
    MagicOptions o = d.magic;
    o.seed = d.seed;
    return o;
}

}  // namespace

World::World(const WorldDesc& d)
    : seed_(d.seed), clock_(d.startHour, d.startDay), settings_(d.settings), terrain_(d.seed), clouds_(d.seed),
      wind_(hashCombine(d.seed, 0x77196)), physics_(std::make_unique<Physics>(&terrain_, &wind_, d.seed)),
      magic_(std::make_unique<MagicField>(&terrain_, &wind_, magicOptions(d))) {
    wind_.setMean(wind());
    physics_->setMagicSampler([this](const dvec3& p, double& rho, dvec3& u, double& air) {
        const MagicSample s = magic_->sample(p);
        rho = s.rho.total();
        u = s.u;
        air = s.level >= 0 && rho > 1e-6 ? s.rho[Aspect::Air] / rho : 0.0;  // only where the field runs
    });
    // The human stands near the spawn point, facing it; a ball bounces off its capsule.
    const dvec3 spawn{k::CAMERA_SPAWN[0], 0.0, k::CAMERA_SPAWN[2]};
    humanPos_ = {spawn.x + k::HUMAN_SPOT[0], 0.0, spawn.z + k::HUMAN_SPOT[2]};
    humanPos_.y = terrain_.height(humanPos_.x, humanPos_.z);
    humanYaw_ = std::atan2(spawn.x - humanPos_.x, spawn.z - humanPos_.z);
    physics_->addCapsule(1, humanPos_ + dvec3{0, 0.5 * k::HUMAN_HEIGHT, 0}, k::HUMAN_RADIUS, 0.5 * k::HUMAN_HEIGHT - k::HUMAN_RADIUS);
}

World::~World() = default;

int World::advance(double realSeconds) {
    const int n = clock_.advance(realSeconds);
    for (int i = 0; i < n; ++i) step();
    return n;
}

void World::step() {
    clock_.onStep();
    clouds_.setOverride(settings_.cloudCover);
    clouds_.setRain(settings_.rain);
    clouds_.step(WorldClock::stepSeconds(), wind(), clock_.seconds());
    wind_.setMean(wind());
    wind_.step(WorldClock::stepSeconds());
    physics_->step(WorldClock::stepSeconds(), clock_.seconds());
    // The campfire: the wind feeds it while its fire magic outweighs the air magic at its bed; else it dies down.
    const dvec3 core = campfireCore();
    if (magic_->started()) {
        const MagicSample bed = magic_->sample(core);
        const double dt = WorldClock::stepSeconds();
        if (bed.rho[Aspect::Fire] > bed.rho[Aspect::Air]) {
            const double breeze = length(wind_.at(core, 0.4));
            fire_ = std::min(double(k::CAMPFIRE_MAX), fire_ + dt * k::CAMPFIRE_FEED * (0.3 + breeze / 3.0));
        } else {
            fire_ -= fire_ * dt / double(k::CAMPFIRE_DIE_TAU);
        }
    }
    // Lightning: each strike leaves fire magic where it hits (whatever it sets alight, it does through that fire).
    for (const dvec3& s : clouds_.takeStrikes()) {
        const dvec3 at{s.x, terrain_.height(s.x, s.z), s.z};
        magic_->addMass(at + dvec3{0, 1.0, 0}, Aspect::Fire, k::WEATHER_STRIKE_FIRE * double(k::MAGIC_L0_CELL) * k::MAGIC_L0_CELL * k::MAGIC_L0_CELL_Y);
        strikes_.push_back({at, clock_.seconds()});
    }
    std::erase_if(strikes_, [&](const Strike& s) { return clock_.seconds() - s.seconds > 5.0; });
    magic_->setRain(clouds_.rainAt(focus_.x, focus_.z));
    // Hot things: the campfire, and the trees burning (each its own fire, fading as it burns out).
    std::vector<HeatSource> heat{HeatSource{core, fire_}};
    for (const auto& [key, b] : burning_) {
        const double left = std::clamp((b.until - clock_.seconds()) / 20.0, 0.0, 1.0);
        heat.push_back({b.tree.position + dvec3{0, 0.6, 0}, k::PLANT_BURN_HEAT * left});
    }
    magic_->setHeat(heat);
    // Light drops: each sun in the sky shoots them (fewer through cloud, none from a hidden disc); the planet sends a
    // little of what hits it back down.
    {
        const SkyState sky = Sky::at(clock_.seconds());
        const double clear = 1.0 - clouds_.cover(clock_.seconds());
        std::vector<DropSource> drops;
        double lit = 0;
        for (int i = 0; i < 2; ++i) {
            if (sky.sun[i].up) drops.push_back({sky.sun[i].dir, k::MAGIC_DROPS * (1.0 - sky.sun[i].coverage) * clear});
            lit += 0.25 * (1.0 - dot(sky.planetDir, sky.sun[i].dir));
        }
        if (sky.planetUp) drops.push_back({sky.planetDir, k::MAGIC_DROPS * k::MAGIC_PLANET_REFLECT * lit * clear});
        magic_->setDrops(drops);
    }
    magic_->step(clock_.steps());
    burnPlants();
    // Runes: what they draw in, what leaks over their seals; the burst ones are gone.
    for (Rune& r : runes_) stepRune(r, *magic_, WorldClock::stepSeconds());
    std::erase_if(runes_, [](const Rune& r) { return r.broken; });
    // What each ball holds evens out with the magic around it; in a void it falls apart.
    {
        std::vector<uint32_t> gone;
        std::map<uint32_t, double> kept;
        for (const Ball& b : physics_->balls()) {
            const auto it = held_.find(b.id);
            double held = it == held_.end() ? double(k::MAGIC_HELD) : it->second;
            held = magic::heldAfter(held, magic_->sample(b.position).rho.total(), WorldClock::stepSeconds());
            if (magic::fallsApart(held)) gone.push_back(b.id);
            else kept[b.id] = held;
        }
        held_.swap(kept);
        for (const uint32_t id : gone) physics_->removeBall(id);
    }
    // The human's nature magic evens out with the magic round its chest (in ordinary places it stays near its share).
    humanHeld_ = magic::heldAfter(humanHeld_, magic_->sample(humanPos_ + dvec3{0, 1.3, 0}).rho.total(), WorldClock::stepSeconds());
    // What the magic left (minerals, sprouts) holds its magic like any object; minerals are rock to a ball.
    {
        std::vector<Manifestation>& ms = magic_->manifestations();
        for (Manifestation& m : ms) {
            m.held = float(magic::heldAfter(m.held, magic_->sample(m.position + dvec3{0, 0.3, 0}).rho.total(), WorldClock::stepSeconds()));
            if (m.kind == ManifestKind::Mineral && !rocks_.count(m.id)) {
                physics_->addRock(m.id, m.position + dvec3{0, 0.25, 0}, std::cbrt(3.0 * m.volume / (4.0 * kPi)));
                rocks_.insert(m.id);
            }
        }
        for (auto it = ms.begin(); it != ms.end();) {
            if (magic::fallsApart(it->held)) {
                physics_->removeRock(it->id);
                rocks_.erase(it->id);
                it = ms.erase(it);
            } else {
                ++it;
            }
        }
    }
}


// Fire magic past the dew point: steam where it meets a pond, trees near it catch (and burn as hot things for a while,
// then stand charred), the grass under it burns away.
void World::burnPlants() {
    // Steam keeps rising a while after the water was heated: it thins over STEAM_SECONDS.
    const double thin = std::exp(-WorldClock::stepSeconds() / k::STEAM_SECONDS);
    for (Glow& g : steam_) g.strength *= thin;
    std::erase_if(steam_, [](const Glow& g) { return g.strength < 1.0; });
    const double now = clock_.seconds();
    for (auto it = burning_.begin(); it != burning_.end();) {
        if (now >= it->second.until) {
            burnt_.insert(it->first);
            it = burning_.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<TreeInstance> near;
    for (const HotSpot& h : magic_->hotSpots()) {
        if (h.water) {
            // One column per couple of metres of water surface, as strong as the fire is past the dew point.
            const double strength = h.fire - k::MAGIC_FIRE_STEAM;
            Glow* column = nullptr;
            for (Glow& g : steam_)
                if (std::hypot(g.position.x - h.position.x, g.position.z - h.position.z) < 2.0) column = &g;
            if (column) column->strength = std::max(column->strength, strength);
            else if (steam_.size() < 64) steam_.push_back({h.position, strength});
            continue;
        }
        const double ground = terrain_.height(h.position.x, h.position.z);
        if (h.position.y - ground < 3.0) {
            bool known = false;
            for (const Glow& g : scorches_) known = known || std::hypot(g.position.x - h.position.x, g.position.z - h.position.z) < 1.0;
            if (!known && scorches_.size() < 64) scorches_.push_back({{h.position.x, ground, h.position.z}, double(k::GRASS_SCORCH_RADIUS)});
        }
        near.clear();
        const double reach = k::PLANT_BURN_REACH + 6.0;
        treesInArea(terrain_, h.position.x - reach, h.position.z - reach, h.position.x + reach, h.position.z + reach, near);
        for (const TreeInstance& t : near) {
            const std::pair<int64_t, int64_t> key{int64_t(std::floor(t.position.x / kTreeCell)), int64_t(std::floor(t.position.z / kTreeCell))};
            if (burnt_.count(key) || burning_.count(key)) continue;
            // Near its trunk or under its crown (a crown spreads about 2.5 m per unit of scale), up to its top.
            const double d = std::hypot(t.position.x - h.position.x, t.position.z - h.position.z);
            const double above = h.position.y - t.position.y;
            if (d < k::PLANT_BURN_REACH + 2.5 * t.scale && above > -1.0 && above < 14.0 * t.scale)
                burning_[key] = {t, now + k::PLANT_BURN_SECONDS};
        }
    }
}

std::vector<World::BurningTree> World::burningTrees() const {
    std::vector<BurningTree> out;
    for (const auto& [key, b] : burning_) {
        const double left = b.until - clock_.seconds();
        out.push_back({b.tree.position, std::clamp(left / 20.0, 0.0, 1.0) * 2.2 * b.tree.scale,
                       std::clamp(1.0 - left / k::PLANT_BURN_SECONDS, 0.0, 1.0)});
    }
    return out;
}

uint32_t World::addRune(Rune r) {
    r.id = nextRune_++;
    r.broken = false;
    runes_.push_back(r);
    return r.id;
}

void World::damageRune(uint32_t id) {
    for (Rune& r : runes_)
        if (r.id == id) breakRune(r, *magic_);
    std::erase_if(runes_, [](const Rune& r) { return r.broken; });
}

dvec3 World::windAt(const dvec3& p) const {
    const TerrainSample s = terrain_.sample(p.x, p.z);
    const double base = s.underwater() ? s.waterLevel : s.height;
    const dvec3 air = wind_.at(p, p.y > base ? p.y - base : 0.0);
    // Moving air magic makes the wind: where it moves on its own (a whirlwind), it takes the air along by its share.
    const MagicSample m = magic_->sample(p);
    const double rho = m.rho.total();
    if (m.level < 0 || rho <= 1e-6) return air;
    return air + (m.u - air) * (double(k::MAGIC_WIND_SHARE) * m.rho[Aspect::Air] / rho);
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
    h = hashCombine(h, physics_->stateHash());
    h = hashCombine(h, wind_.stateHash());
    h = hashCombine(h, magic_->stateHash());
    h = hashCombine(h, clouds_.stateHash());
    for (const auto& [key, b] : burning_) h = hashCombine(hashCombine(h, uint64_t(key.first)), bits(b.until));
    for (const auto& key : burnt_) h = hashCombine(hashCombine(h, uint64_t(key.first)), uint64_t(key.second));
    for (const Glow& g : scorches_) h = hashCombine(hashCombine(h, bits(g.position.x)), bits(g.position.z));
    h = hashCombine(h, bits(fire_));
    for (const auto& [id, held] : held_) h = hashCombine(hashCombine(h, id), bits(held));
    h = hashCombine(h, bits(humanHeld_));
    for (const Rune& r : runes_) {
        h = hashCombine(h, r.id);
        for (const double c : r.contents) h = hashCombine(h, bits(c));
    }
    for (const Manifestation& m : magic_->manifestations()) h = hashCombine(hashCombine(h, m.id), bits(m.held));
    return h;
}

}  // namespace taelia
