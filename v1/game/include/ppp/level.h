// The level data model, its JSON loader, and the validator (doc 9.3).
//
// Levels are plain data. Nothing here knows about Godot, and nothing here
// runs physics — `Sim` builds a `Solver` from a `Level` and the same `Level`
// drives the headless replay harness and the in-engine game. That is what
// makes the regression suite in doc 15.2 possible at all: the thing CI
// simulates has to be the thing the player plays.
//
// Doc 9.1 says Godot's scene editor is the level editor, and it should be:
// this format is what a `@tool` node exports to, not something anyone types
// by hand for a real level. It stays human-readable so that a level's diff
// is reviewable and a broken level is fixable without opening the editor.

#pragma once

#include <string>
#include <vector>

#include "ppp/solver.h"
#include "ppp/vec2.h"

namespace ppp {

/// Solid geometry. `door_id >= 0` means this box is part of a door and is
/// disabled while that door is open.
struct LevelBox {
    AABB rect;
    int door_id = -1;
};

struct LevelBody {
    BodyType type = BodyType::Pressure;
    int id = 0;
    Vec2 position;

    // Pressure
    float radius = 24.0f;
    int count = 20;
    float pressure = 1.0f;

    // Lattice
    int width = 4;
    int height = 4;
    float spacing = 12.0f;

    // Rope
    Vec2 to;
    bool pin_start = true;
    bool pin_end = false;

    float mass_per_particle = 1.0f;

    /// How many particles this body will create. The validator needs this
    /// before anything is built, to check the level's budget (doc 10.1).
    int ParticleCount() const;
};

/// A pressure plate: opens a door while enough mass rests in its region.
struct LevelPlate {
    int id = 0;
    AABB region;
    /// Mass of soft-body particles inside `region` needed to press it. The
    /// player's own weight counts separately via `player_counts`.
    float threshold = 3.0f;
    bool player_counts = true;
    /// Doors this plate opens. A door with several plates opens when all of
    /// them are pressed, which is the whole of the two-plate puzzle.
    std::vector<int> opens;
};

struct LevelDoor {
    int id = 0;
    /// Plates that must all be pressed. Empty means the door never opens,
    /// which the validator treats as an authoring error.
    std::vector<int> plates;
    /// Once opened, stays open. Doors that re-close on release make a
    /// puzzle a timing exercise; that is a different game and the level
    /// format says so rather than leaving it to a forgotten default.
    bool latching = true;
};

struct LevelCheckpoint {
    Vec2 position;
    float radius = 20.0f;
};

struct Level {
    std::string id;
    std::string name;
    int format_version = 1;
    uint32_t seed = 0;

    Vec2 spawn;
    AABB goal;
    AABB camera_bounds;

    std::vector<LevelBox> boxes;
    std::vector<LevelBody> bodies;
    std::vector<LevelPlate> plates;
    std::vector<LevelDoor> doors;
    std::vector<AABB> hazards;
    std::vector<LevelCheckpoint> checkpoints;

    int particle_budget = 1200;  ///< doc 10.1

    /// Minimum thickness any solid box may have, in world units. Below this
    /// a particle moving at the contract's maximum speed can cross it
    /// between two sweeps (doc 4.5). The validator enforces it; it is stored
    /// per level so a level can raise it, never lower it.
    float min_wall_thickness = 8.0f;

    int TotalParticles() const;
    const LevelDoor* FindDoor(int id) const;
    const LevelPlate* FindPlate(int id) const;
};

/// Load from JSON text. Returns false with a message on a parse error or a
/// structurally impossible level (a body of an unknown type, say). Content
/// problems — an unreachable goal, a thin wall — are the validator's job,
/// because they should be reported all at once rather than one per run.
bool LoadLevel(const std::string& json_text, Level* out, std::string* error);
bool LoadLevelFile(const std::string& path, Level* out, std::string* error);

/// Round-trip: what the Godot `@tool` exporter writes and the tests compare
/// against, so a level that loads and is written back is unchanged.
std::string WriteLevel(const Level& level);

// --------------------------------------------------------------- validation

struct ValidationIssue {
    enum class Severity { Warning, Error };
    Severity severity = Severity::Error;
    std::string check;    ///< which rule, for grouping in CI output
    std::string message;  ///< what is wrong and where
};

struct ValidationReport {
    std::vector<ValidationIssue> issues;
    bool Ok() const {
        for (const auto& i : issues)
            if (i.severity == ValidationIssue::Severity::Error) return false;
        return true;
    }
    int ErrorCount() const;
    int WarningCount() const;
};

/// The checks from doc 9.3, all of them, run on every level in CI:
///
///   - every solid box is at least `min_wall_thickness` thick
///   - the player spawn is not inside geometry
///   - the goal is reachable by a naive flood fill
///   - no soft body starts overlapping static geometry
///   - the level fits inside its camera bounds
///   - checkpoints exist and are reachable
///   - the total particle count is within budget
///
/// Plus three the document does not list but that the format makes cheap:
/// duplicate ids, doors with no plates, and plates that open nothing.
ValidationReport ValidateLevel(const Level& level, const PhysicsContract& contract);

/// The flood fill the reachability checks use, exposed so the tools can
/// render it. Cell size is `cell`; returns true if `to` is reachable from
/// `from` through space not occupied by *enabled-at-start* geometry.
///
/// Naive, as the document says: it ignores jump height and every soft body,
/// so it catches walled-off exits and nothing subtler. A level that passes
/// is not necessarily solvable — that is what the recorded solution replay
/// is for (doc 3.3).
bool FloodReachable(const Level& level, const Vec2& from, const AABB& to, float cell,
                    float agent_radius);

}  // namespace ppp
