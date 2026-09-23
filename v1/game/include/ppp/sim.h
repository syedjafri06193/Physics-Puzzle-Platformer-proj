// The simulation: one level, one player, one solver, one tick counter.
//
// This is the unit the replay harness runs (doc 15.2) and the unit the
// engine drives. Godot owns rendering and input; it does not own a single
// number that affects the outcome. That split is what makes a recorded
// solution replayable in CI on a machine with no GPU.
//
// Everything in `Sim` is a pure function of (level, contract, input stream).
// No wall clock, no RNG that is not seeded from the level, no iteration over
// an unordered container, no floating-point state carried between levels.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ppp/input.h"
#include "ppp/level.h"
#include "ppp/player.h"
#include "ppp/solver.h"

namespace ppp {

/// Deterministic RNG, seeded per level (doc 6.2). xorshift64*, because the
/// standard library's engines are specified but its *distributions* are not
/// — `std::uniform_int_distribution` is allowed to differ between
/// implementations, which is exactly the kind of thing that makes a replay
/// stop reproducing after a toolchain upgrade.
class Rng {
public:
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) : state_(seed ? seed : 1ull) {}

    uint64_t Next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1Dull;
    }

    /// Uniform in [0, 1). 24 bits of mantissa, so the result is exactly
    /// representable and identical on every IEEE-754 machine.
    float NextFloat() { return static_cast<float>(Next() >> 40) * (1.0f / 16777216.0f); }

    uint64_t State() const { return state_; }
    void SetState(uint64_t s) { state_ = s; }

private:
    uint64_t state_;
};

struct PlateState {
    int id = 0;
    bool pressed = false;
    float mass = 0.0f;
};

struct DoorState {
    int id = 0;
    bool open = false;
};

enum class SimOutcome : uint8_t {
    Running = 0,
    Completed = 1,
    /// Died this tick. The sim respawns at the last checkpoint on the next
    /// tick; the outcome is reported so a replay can assert on it.
    Died = 2,
};

class Sim {
public:
    Sim(const Level& level, const PhysicsContract& contract = DefaultContract(),
        const PlayerTuning& tuning = DefaultPlayerTuning());

    /// Rebuild from the level. Called by the constructor, by a restart, and
    /// by the replay harness between runs — the same code path each time,
    /// so a restarted level is bit-identical to a freshly loaded one. A
    /// restart that leaves anything behind is a replay that diverges a
    /// minute in, and it is very hard to find afterwards.
    void Reset();

    /// One fixed step. `in` is quantized on the way in, so live play and
    /// playback consume exactly the same values (see input.h).
    SimOutcome Step(const InputState& in);

    // --- state -----------------------------------------------------------

    int Tick() const { return tick_; }
    SimOutcome Outcome() const { return outcome_; }
    bool Completed() const { return outcome_ == SimOutcome::Completed; }
    int Deaths() const { return deaths_; }
    int CheckpointIndex() const { return checkpoint_; }

    const Player& GetPlayer() const { return player_; }
    const Solver& GetSolver() const { return solver_; }
    Solver& MutableSolver() { return solver_; }
    const Level& GetLevel() const { return level_; }
    const std::vector<PlateState>& Plates() const { return plates_; }
    const std::vector<DoorState>& Doors() const { return doors_; }
    const PlayerTuning& Tuning() const { return tuning_; }

    /// Hash of everything that can differ between two runs: the tick,
    /// outcome, player, plate and door states, and the solver's particles.
    /// The replay suite compares these, so anything left out of it is
    /// something a divergence test cannot see.
    uint64_t StateHash() const;

    /// The contract hash recorded replays are checked against. Covers the
    /// physics contract *and* the player tuning, because a change to jump
    /// height breaks a recorded solution exactly as thoroughly as a change
    /// to gravity, and the design document's contract (doc 3.3) lists
    /// player tuning among the things it covers.
    uint64_t ContractHash() const;

    /// Render interpolation (doc 4.2): `alpha` is the fraction of a step
    /// elapsed since the last one. The renderer calls this, never `pos`.
    Vec2 InterpolatedPlayerPosition(float alpha) const;

private:
    void Build();
    void UpdatePlates();
    void ApplyDoors();
    bool CheckHazards();
    void CheckCheckpoints();
    void Respawn();

    Level level_;
    PhysicsContract contract_;
    PlayerTuning tuning_;

    Solver solver_;
    Player player_;
    Rng rng_;

    std::vector<PlateState> plates_;
    std::vector<DoorState> doors_;
    /// Box index in the solver's geometry for each level box, so a door can
    /// disable exactly the boxes that belong to it.
    std::vector<int> box_to_geometry_;
    /// Solver body index for each level body, in level order.
    std::vector<int> body_index_;

    int tick_ = 0;
    int deaths_ = 0;
    int checkpoint_ = -1;
    SimOutcome outcome_ = SimOutcome::Running;
};

/// The fixed-timestep accumulator from doc 4.2, for the engine side.
///
/// Kept here rather than in GDScript so that the clamp and the step cap are
/// part of the tested C++ rather than a line someone edits in the editor.
class StepClock {
public:
    /// Feeds a frame's wall-clock delta in and reports how many fixed steps
    /// to run. `Alpha()` afterwards is the render interpolation factor.
    int Advance(float frame_delta) {
        // A suspended process resumes with an enormous delta; without this
        // clamp it runs hundreds of steps at once and the level is over
        // before the first frame draws (doc 4.2).
        if (frame_delta > MAX_FRAME_TIME) frame_delta = MAX_FRAME_TIME;
        accumulator_ += frame_delta;
        int steps = 0;
        while (accumulator_ >= FIXED_DT && steps < MAX_STEPS_PER_FRAME) {
            accumulator_ -= FIXED_DT;
            ++steps;
        }
        // If the cap was hit, the leftover is dropped rather than carried.
        // Carrying it is the spiral of death: the next frame is asked for
        // even more steps, takes longer, and asks for more again.
        if (steps == MAX_STEPS_PER_FRAME && accumulator_ >= FIXED_DT) accumulator_ = 0.0f;
        return steps;
    }

    float Alpha() const { return accumulator_ / FIXED_DT; }
    void Reset() { accumulator_ = 0.0f; }

private:
    float accumulator_ = 0.0f;
};

}  // namespace ppp
