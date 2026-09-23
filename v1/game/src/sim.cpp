#include "ppp/sim.h"

#include <cstring>

namespace ppp {

Sim::Sim(const Level& level, const PhysicsContract& contract, const PlayerTuning& tuning)
    : level_(level), contract_(contract), tuning_(tuning), solver_(contract) {
    Reset();
}

void Sim::Reset() {
    solver_.Clear();
    solver_.SetContract(contract_);
    player_ = Player{};
    rng_ = Rng(level_.seed ? level_.seed : 0x9E3779B97F4A7C15ull);
    tick_ = 0;
    deaths_ = 0;
    checkpoint_ = -1;
    outcome_ = SimOutcome::Running;
    Build();
}

void Sim::Build() {
    box_to_geometry_.clear();
    body_index_.clear();
    plates_.clear();
    doors_.clear();

    // Geometry first, in level order. The order fixes the box indices, which
    // decide which surface a particle is pushed out of when it is inside two
    // at once — so this loop must never be reordered or filtered (doc 6.2).
    for (const auto& box : level_.boxes) {
        box_to_geometry_.push_back(solver_.Geometry().Add(box.rect));
    }

    for (const auto& b : level_.bodies) {
        int index = -1;
        switch (b.type) {
            case BodyType::Pressure:
                index = solver_.AddPressureBody(b.position, b.radius, b.count, b.mass_per_particle,
                                                b.pressure, b.id);
                break;
            case BodyType::Lattice:
                index = solver_.AddLattice(b.position, b.width, b.height, b.spacing,
                                           b.mass_per_particle, b.id);
                break;
            case BodyType::Rope:
                index = solver_.AddRope(b.position, b.to, b.count, b.mass_per_particle, b.pin_start,
                                        b.pin_end, b.id);
                break;
        }
        body_index_.push_back(index);
    }

    for (const auto& p : level_.plates) plates_.push_back(PlateState{p.id, false, 0.0f});
    for (const auto& d : level_.doors) doors_.push_back(DoorState{d.id, false});

    player_.pos = level_.spawn;
    player_.prev_pos = level_.spawn;

    ApplyDoors();
}

// --------------------------------------------------------------------- step

SimOutcome Sim::Step(const InputState& raw) {
    if (outcome_ == SimOutcome::Completed) return outcome_;

    // Quantize on the way in, always. A live run and a replay of that run
    // must consume identical floats, and the only way to guarantee that is
    // for live play to go through the same lossy step the recording does
    // (doc 14.3).
    const InputState in = raw.Quantized();

    outcome_ = SimOutcome::Running;

    if (in.restart) {
        Respawn();
        ++tick_;
        return outcome_;
    }

    // The player is a kinematic box as far as the solver is concerned: soft
    // bodies are pushed by it and it is not pushed back by them here (the
    // push-back is in StepPlayer, which resolves against particles
    // directly). `motion` is last step's movement, so friction can drag a
    // body along a moving platform or a walking player.
    const Vec2 player_motion = player_.pos - player_.prev_pos;
    solver_.ClearKinematic();
    solver_.AddKinematic(PlayerBox(player_, tuning_), player_motion);

    StepPlayer(&player_, tuning_, in, FIXED_DT, solver_.Geometry(), &solver_);

    // The player moved; refresh the kinematic box before the solver runs so
    // bodies react to where the player is now, not where it was.
    solver_.ClearKinematic();
    solver_.AddKinematic(PlayerBox(player_, tuning_), player_.pos - player_.prev_pos);

    solver_.Step(FIXED_DT);

    UpdatePlates();
    ApplyDoors();
    CheckCheckpoints();

    if (CheckHazards()) {
        ++deaths_;
        Respawn();
        outcome_ = SimOutcome::Died;
        ++tick_;
        return outcome_;
    }

    if (PlayerBox(player_, tuning_).Overlaps(level_.goal)) {
        outcome_ = SimOutcome::Completed;
    }

    ++tick_;
    return outcome_;
}

void Sim::UpdatePlates() {
    for (size_t i = 0; i < level_.plates.size(); ++i) {
        const LevelPlate& plate = level_.plates[i];
        float mass = solver_.MassInside(plate.region);
        if (plate.player_counts && PlayerBox(player_, tuning_).Overlaps(plate.region)) {
            // The player's weight, in the same units as a particle's mass,
            // so one number in the level file means the same thing whether
            // a blob or a person is standing on the plate.
            mass += 6.0f;
        }
        plates_[i].mass = mass;
        const bool pressed = mass >= plate.threshold;
        // A latching door's plate may release; the door stays open. The
        // plate itself always reports its live state, because the HUD shows
        // it and a plate that lies is a puzzle nobody can read.
        plates_[i].pressed = pressed;
    }
}

void Sim::ApplyDoors() {
    for (size_t i = 0; i < level_.doors.size(); ++i) {
        const LevelDoor& door = level_.doors[i];
        bool all_pressed = !door.plates.empty();
        for (const int plate_id : door.plates) {
            bool found = false;
            for (const auto& p : plates_) {
                if (p.id != plate_id) continue;
                found = true;
                if (!p.pressed) all_pressed = false;
                break;
            }
            if (!found) all_pressed = false;
        }

        if (door.latching) {
            doors_[i].open = doors_[i].open || all_pressed;
        } else {
            doors_[i].open = all_pressed;
        }
    }

    // Push the door states into the solver's geometry. A disabled box stops
    // colliding for particles and, because StepPlayer reads the same
    // `enabled` array, for the player too.
    for (size_t i = 0; i < level_.boxes.size(); ++i) {
        const int door_id = level_.boxes[i].door_id;
        if (door_id < 0) continue;
        bool open = false;
        for (const auto& d : doors_)
            if (d.id == door_id) open = d.open;
        const int geometry_index = box_to_geometry_[i];
        if (geometry_index < 0) continue;
        solver_.Geometry().enabled[static_cast<size_t>(geometry_index)] = open ? 0u : 1u;
    }
}

bool Sim::CheckHazards() {
    const AABB box = PlayerBox(player_, tuning_);
    for (const auto& h : level_.hazards)
        if (box.Overlaps(h)) return true;
    // Falling out of the world counts. Without this a player who clips
    // through the floor falls forever and the level never ends, which is
    // the single most common way a physics platformer softlocks.
    if (player_.pos.y > level_.camera_bounds.max.y + 200.0f) return true;
    return false;
}

void Sim::CheckCheckpoints() {
    for (size_t i = 0; i < level_.checkpoints.size(); ++i) {
        if (static_cast<int>(i) <= checkpoint_) continue;
        const LevelCheckpoint& cp = level_.checkpoints[i];
        const Vec2 delta = player_.pos - cp.position;
        if (delta.LengthSquared() <= cp.radius * cp.radius) {
            // Checkpoints are ordered and only ever advance. Letting them go
            // backwards means a player who walks back past an earlier one
            // loses progress, which reads as a bug every single time.
            checkpoint_ = static_cast<int>(i);
        }
    }
}

void Sim::Respawn() {
    // A death rebuilds the whole level, not just the player.
    //
    // Restoring only the player is tempting and wrong: the soft bodies are
    // the puzzle, and a body that has been pushed somewhere unrecoverable
    // stays unrecoverable, so the player respawns into a level they can no
    // longer solve. Doc 7.3 asks for a fast restart for exactly this
    // reason, and a full rebuild is also the only respawn that is
    // bit-identical to a fresh load, which the replay suite depends on.
    const int saved_checkpoint = checkpoint_;
    const int saved_deaths = deaths_;

    solver_.Clear();
    solver_.SetContract(contract_);
    player_ = Player{};
    rng_ = Rng(level_.seed ? level_.seed : 0x9E3779B97F4A7C15ull);
    Build();

    checkpoint_ = saved_checkpoint;
    deaths_ = saved_deaths;

    if (checkpoint_ >= 0 && checkpoint_ < static_cast<int>(level_.checkpoints.size())) {
        player_.pos = level_.checkpoints[static_cast<size_t>(checkpoint_)].position;
        player_.prev_pos = player_.pos;
    }
}

// --------------------------------------------------------------- hashing

uint64_t Sim::StateHash() const {
    uint64_t h = HashBytes(&tick_, sizeof(tick_));
    h = HashBytes(&outcome_, sizeof(outcome_), h);
    h = HashBytes(&deaths_, sizeof(deaths_), h);
    h = HashBytes(&checkpoint_, sizeof(checkpoint_), h);
    h = HashBytes(&player_.pos, sizeof(Vec2), h);
    h = HashBytes(&player_.vel, sizeof(Vec2), h);
    h = HashBytes(&player_.on_floor, sizeof(bool), h);
    h = HashBytes(&player_.grabbed_body, sizeof(int), h);
    h = HashBytes(&player_.grabbed_particle, sizeof(int), h);
    for (const auto& p : plates_) h = HashBytes(&p.pressed, sizeof(bool), h);
    for (const auto& d : doors_) h = HashBytes(&d.open, sizeof(bool), h);
    const uint64_t solver_hash = solver_.StateHash();
    h = HashBytes(&solver_hash, sizeof(solver_hash), h);
    const uint64_t rng_state = rng_.State();
    h = HashBytes(&rng_state, sizeof(rng_state), h);
    return h;
}

uint64_t Sim::ContractHash() const {
    const uint64_t physics = contract_.Hash();
    const uint64_t player = tuning_.Hash();
    uint64_t h = HashBytes(&physics, sizeof(physics));
    h = HashBytes(&player, sizeof(player), h);
    const int version = PHYSICS_VERSION;
    h = HashBytes(&version, sizeof(version), h);
    const int input_version = INPUT_FORMAT_VERSION;
    h = HashBytes(&input_version, sizeof(input_version), h);
    return h;
}

Vec2 Sim::InterpolatedPlayerPosition(float alpha) const {
    const float a = Clamp(alpha, 0.0f, 1.0f);
    return player_.prev_pos + (player_.pos - player_.prev_pos) * a;
}

}  // namespace ppp
