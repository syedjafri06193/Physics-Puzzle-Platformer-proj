// The physics contract (design doc 3.3).
//
// Every parameter here changes the simulation. Changing any of them
// invalidates every recorded replay, so they live in one versioned struct
// with a hash, and `PHYSICS_VERSION` is bumped whenever one moves.
//
// The regression suite compares a replay's stored contract hash against the
// running build's. A mismatch is reported as "needs re-verification", never
// silently trusted — which is the whole point of writing it down.

#pragma once

#include <cstdint>

namespace ppp {

/// Bump on ANY change that can affect simulation: stiffness, iteration
/// count, substeps, gravity, damping, collision epsilon, player tuning.
/// See 1—Documentation/v1/physics-contract.md for the changelog.
inline constexpr int PHYSICS_VERSION = 1;

/// Physics runs at a fixed rate. Not a setting; a contract term (doc 4.2).
inline constexpr float PHYSICS_HZ = 120.0f;
inline constexpr float FIXED_DT = 1.0f / PHYSICS_HZ;

/// Rendering may run at any rate and interpolates between the last two
/// physics states. These two guards keep a suspended process from running
/// hundreds of steps on resume, and stop the spiral of death (doc 4.2).
inline constexpr float MAX_FRAME_TIME = 0.25f;
inline constexpr int MAX_STEPS_PER_FRAME = 5;

struct PhysicsContract {
    // --- integration -------------------------------------------------
    float gravity = 900.0f;
    /// Multiplies the implicit velocity each step; this is where damping
    /// belongs in Verlet, because there is no velocity variable (doc 4.1).
    float damping = 0.995f;

    // --- solver ------------------------------------------------------
    /// Substeps converge better than iterations for stiff constraints
    /// (doc 4.3, XPBD). Both are contract terms, not tuning knobs.
    int substeps = 2;
    int iterations = 6;

    // --- collision ---------------------------------------------------
    float particle_radius = 3.0f;
    /// Separation kept after a collision, so a particle resting on a
    /// surface does not re-collide every step.
    float skin = 0.05f;
    float restitution = 0.15f;
    float friction = 0.35f;

    // --- sleeping (doc 14.2) -----------------------------------------
    float sleep_threshold = 0.0004f;
    float sleep_delay = 0.35f;

    // --- soft bodies -------------------------------------------------
    float structural_stiffness = 0.9f;
    float shear_stiffness = 0.55f;
    float bend_stiffness = 0.25f;
    float ring_stiffness = 0.85f;
    /// How hard the area constraint pushes back toward the target area.
    float pressure_stiffness = 0.35f;

    /// FNV-1a over the raw bytes of every field above. Stored in replays so
    /// a physics change is detected even if someone forgets to bump the
    /// version.
    uint64_t Hash() const;

    bool operator==(const PhysicsContract& o) const;
};

/// The one contract the game ships with.
const PhysicsContract& DefaultContract();

}  // namespace ppp
