// The player character (doc 7.1, and doc 2.2 for why it is rigid).
//
// A rigid axis-aligned box with an explicit velocity, not a soft body and
// not a solver particle. Doc 2.2 is unambiguous about this: a soft-bodied
// player is a different and much harder game, because every control
// guarantee a platformer needs — jump height, ledge grabs, a predictable
// collision box — stops being a guarantee once the body can deform.
//
// Everything here runs on the fixed timestep. The forgiveness windows in
// doc 7.1 are measured in seconds, and a coyote window that is "6 frames"
// rather than "100 ms" is a different game on a 144 Hz monitor.

#pragma once

#include <cstdint>

#include "ppp/input.h"
#include "ppp/solver.h"
#include "ppp/vec2.h"

namespace ppp {

/// Every number that changes how the player moves. Part of the physics
/// contract: these are hashed into the contract hash and a change to any of
/// them invalidates recorded replays, exactly like a solver parameter.
struct PlayerTuning {
    float half_width = 7.0f;
    float half_height = 12.0f;

    float run_speed = 210.0f;
    /// Reaching top speed in ~0.08 s on the ground and ~0.14 s in the air.
    /// Air control that matches ground control makes the jump feel
    /// weightless; none at all makes a missed jump unrecoverable.
    float ground_accel = 2600.0f;
    float ground_friction = 2400.0f;
    float air_accel = 1500.0f;
    float air_friction = 500.0f;

    float jump_speed = 330.0f;
    float gravity = 1100.0f;
    /// The design document's "fast-fall gravity, 1.5–2× rise gravity"
    /// (doc 7.1). It is an asymmetry between rise and fall, not a button:
    /// rising feels floaty, falling feels weighty.
    float fall_gravity_scale = 1.8f;
    float max_fall_speed = 620.0f;

    /// Reduced gravity near the top of the arc, which is the moment the
    /// player is aiming and therefore the moment they want more time.
    float apex_speed = 55.0f;
    float apex_gravity_scale = 0.45f;

    float coyote_time = 0.100f;   ///< doc 7.1
    float jump_buffer = 0.100f;   ///< doc 7.1
    /// Cutting the rise when the button releases (variable jump height).
    float jump_release_cut = 0.45f;
    /// Nudge past a corner clipped by this much or less (doc 7.1).
    float corner_correction = 2.0f;

    // --- grab (doc 7.3) -----------------------------------------------
    float grab_range = 34.0f;
    /// The grab is a spring to the hand, not a teleport: a rigid attachment
    /// to a soft body lets the player inject unbounded energy by running
    /// into a wall while holding one.
    float grab_stiffness = 900.0f;
    float grab_damping = 26.0f;
    /// Force cap, so a grabbed body wedged in geometry cannot accumulate an
    /// arbitrarily large impulse and fire across the level on release.
    float grab_max_force = 40000.0f;
    float hand_reach = 16.0f;

    uint64_t Hash() const;
    bool operator==(const PlayerTuning& o) const;
};

const PlayerTuning& DefaultPlayerTuning();

struct Player {
    Vec2 pos{0.0f, 0.0f};  ///< centre of the box
    Vec2 vel{0.0f, 0.0f};
    /// Position at the end of the previous step, for render interpolation
    /// (doc 4.2). The renderer never reads `pos` directly.
    Vec2 prev_pos{0.0f, 0.0f};

    bool on_floor = false;
    bool on_ceiling = false;
    bool against_wall = false;
    /// -1 or +1; kept across a stop so the hand stays where it was pointed.
    int facing = 1;

    float coyote_timer = 0.0f;
    float jump_buffer_timer = 0.0f;
    /// True between leaving the ground on a jump and either releasing the
    /// button or starting to fall. Only a rising jump can be cut short.
    bool jump_held = false;
    bool prev_jump = false;
    bool prev_grab = false;

    int grabbed_body = -1;
    int grabbed_particle = -1;
};

/// Where the player's hand is, given facing. The grab reaches from here.
Vec2 HandPosition(const Player& p, const PlayerTuning& t);

/// The player's collision box.
AABB PlayerBox(const Player& p, const PlayerTuning& t);

/// One fixed step of player movement.
///
/// `solver` is used two ways: the player is pushed out of soft bodies (so a
/// blob is something you can stand on), and the grab applies a force to one
/// particle. The solver's own kinematic list is refreshed by the caller
/// (`Sim`), not here, because the order matters and Sim owns it.
void StepPlayer(Player* p, const PlayerTuning& t, const InputState& in, float dt,
                const StaticGeometry& geometry, Solver* solver);

}  // namespace ppp
