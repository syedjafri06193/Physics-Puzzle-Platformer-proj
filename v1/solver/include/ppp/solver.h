// The verlet solver (design doc 4, 5, 14.1).
//
// Standalone: no Godot, no engine, no dependencies beyond the standard
// library. That is the structural decision that makes determinism testing,
// unit testing and the headless replay harness all possible (doc 12).

#pragma once

#include <cstdint>
#include <vector>

#include "ppp/contract.h"
#include "ppp/vec2.h"

namespace ppp {

/// Particles are stored structure-of-arrays: the constraint solve touches
/// positions repeatedly and nothing else (doc 10.3).
struct ParticleArray {
    std::vector<Vec2> pos;
    std::vector<Vec2> prev;
    std::vector<Vec2> accel;
    std::vector<float> inv_mass;  // 0 = pinned

    size_t Size() const { return pos.size(); }

    int Add(const Vec2& p, float inv_m) {
        pos.push_back(p);
        prev.push_back(p);
        accel.push_back(Vec2::Zero());
        inv_mass.push_back(inv_m);
        return static_cast<int>(pos.size()) - 1;
    }

    void Clear() {
        pos.clear();
        prev.clear();
        accel.clear();
        inv_mass.clear();
    }
};

/// A distance constraint between two particles. Constraints live in one flat
/// array in authored order and are never sorted: Gauss-Seidel is
/// order-dependent, so a "cache efficiency" sort would silently invalidate
/// every recorded replay (doc 6.2).
struct DistanceConstraint {
    int a = 0;
    int b = 0;
    float rest = 0.0f;
    float stiffness = 1.0f;
    int body = -1;
};

enum class BodyType : uint8_t {
    Lattice = 0,   ///< mass-spring grid: jelly, deformable platforms
    Pressure = 1,  ///< gas model: blobs, balloons
    Rope = 2,      ///< a chain of particles
};

struct SoftBody {
    BodyType type = BodyType::Pressure;
    /// Particle indices owned by this body, in creation order.
    std::vector<int> particles;
    /// For pressure bodies: the boundary ring, in winding order.
    std::vector<int> ring;
    /// Target area as a multiple of the authored area: 1.0 holds its shape,
    /// 1.3 is a taut balloon, 0.8 is under-inflated. The single knob for how
    /// firm a blob is, and a good one to expose to level design.
    float pressure = 1.0f;
    /// Signed shoelace area at build time; the area constraint is relative
    /// to this, so the same body scales to any size.
    float rest_area = 0.0f;
    float sleep_timer = 0.0f;
    bool sleeping = false;
    bool grabbed = false;  ///< held by the player; never sleeps while true
    /// Stable identifier from the level file, so replays survive reordering.
    int id = 0;
};

/// Static level geometry: axis-aligned boxes, some of which can be disabled
/// at runtime (doors).
struct StaticGeometry {
    std::vector<AABB> boxes;
    std::vector<uint8_t> enabled;  // uint8_t, not bool: vector<bool> is a bitfield

    int Add(const AABB& box) {
        boxes.push_back(box);
        enabled.push_back(1);
        return static_cast<int>(boxes.size()) - 1;
    }

    void Clear() {
        boxes.clear();
        enabled.clear();
    }

    bool PointInside(const Vec2& p) const;

    struct Hit {
        Vec2 point;
        Vec2 normal;
        float t = 0.0f;
        int box = -1;
    };

    /// Segment cast from `from` to `to`, against boxes expanded by
    /// `radius`. Returns the first hit, which is what makes fast movement
    /// through thin walls survivable (doc 4.5).
    bool Raycast(const Vec2& from, const Vec2& to, float radius, Hit* out) const;

    /// Push a point out of any box it is inside, along the shallowest axis.
    bool Resolve(const Vec2& p, float radius, Vec2* corrected, Vec2* normal) const;
};

class Solver {
public:
    explicit Solver(const PhysicsContract& contract = DefaultContract());

    void SetContract(const PhysicsContract& c) { contract_ = c; }
    const PhysicsContract& Contract() const { return contract_; }

    void Clear();

    // --- construction -------------------------------------------------

    /// A mass-spring lattice with structural, shear and bend springs. The
    /// shear springs are the ones people leave out, and the symptom is a
    /// "solid" object that quietly folds flat (doc 5.2).
    int AddLattice(const Vec2& origin, int w, int h, float spacing, float mass_per_particle, int id);

    /// A pressure body: a ring of particles plus the gas model (doc 5.3).
    int AddPressureBody(const Vec2& center, float radius, int count, float mass_per_particle,
                        float pressure, int id);

    /// A rope: a chain of particles, optionally pinned at the first end.
    int AddRope(const Vec2& from, const Vec2& to, int count, float mass_per_particle, bool pin_start,
                bool pin_end, int id);

    // --- simulation ---------------------------------------------------

    /// One fixed step. `dt` must always be FIXED_DT; it is a parameter only
    /// so tests can probe sub-contract behaviour.
    void Step(float dt);

    /// External per-step force on a single particle (the player's grab).
    void AddForce(int particle, const Vec2& force);

    /// Moving colliders the solver does not own — the player, and any
    /// platform the game layer moves. Refreshed by the game layer before
    /// each step; `motion` is how far the box moved since the last step, so
    /// friction can drag particles along with it.
    void ClearKinematic();
    void AddKinematic(const AABB& box, const Vec2& motion);

    // --- queries ------------------------------------------------------

    const ParticleArray& Particles() const { return particles_; }
    ParticleArray& MutableParticles() { return particles_; }
    const std::vector<SoftBody>& Bodies() const { return bodies_; }
    std::vector<SoftBody>& MutableBodies() { return bodies_; }
    const std::vector<DistanceConstraint>& Constraints() const { return constraints_; }
    StaticGeometry& Geometry() { return geometry_; }
    const StaticGeometry& Geometry() const { return geometry_; }

    /// Index of the body nearest to a point, or -1. Used by the grab.
    int BodyNear(const Vec2& p, float max_distance) const;
    /// The body's area-weighted centre (its particle centroid).
    Vec2 BodyCenter(int body) const;
    /// Mass of a body's particles inside a region — what a pressure plate
    /// weighs (game/level.h).
    float MassInside(const AABB& region) const;
    /// Nearest particle of a body to a point.
    int NearestParticle(int body, const Vec2& p) const;

    /// Order-independent? No: deliberately order-dependent, over positions
    /// and previous positions, so any change in simulation shows up.
    uint64_t StateHash() const;

    /// Wakes a body (a collision, or the player touching it).
    void Wake(int body);

    int AwakeBodyCount() const;

private:
    /// The gas model as a *positional* constraint (see the .cpp for why it
    /// is not the design document's force).
    void SolveAreaConstraints();
    void Integrate(float dt);
    void SolveConstraints(int iteration);
    /// Swept pass: clamp each particle to the first surface it crossed.
    void SweepCollisions();
    /// Positional pass: push particles out of geometry and each other.
    void ResolveOverlaps();
    void ResolveParticlePairs();
    void BuildGrid();
    /// One velocity pass per substep: restitution and friction, applied
    /// once, from the contacts the position passes recorded.
    void ApplyContactVelocities();
    void NoteContact(int particle, const Vec2& normal, const Vec2& surface_motion);
    void UpdateSleeping(float dt);

    PhysicsContract contract_;
    ParticleArray particles_;
    std::vector<DistanceConstraint> constraints_;
    std::vector<SoftBody> bodies_;
    StaticGeometry geometry_;
    /// Body index per particle, so a collision can wake the right body.
    std::vector<int> particle_body_;

    struct Kinematic {
        AABB box;
        Vec2 motion;
    };
    std::vector<Kinematic> kinematic_;

    // Contacts recorded during the position passes and consumed once by the
    // velocity pass. Applying friction inside the iteration loop instead
    // feeds Gauss-Seidel's directional bias straight into the velocity, and
    // a ring-shaped body then slides across a flat floor on its own.
    std::vector<Vec2> contact_normal_;
    std::vector<Vec2> contact_motion_;
    std::vector<uint8_t> has_contact_;
    /// Velocity right after integration, before any position pass. The
    /// bounce has to be computed from the speed a particle was *approaching*
    /// at; the post-solve velocity already contains the push-out, and using
    /// it hands every resting particle its penetration back as upward speed.
    std::vector<Vec2> pre_velocity_;

    // Uniform grid for particle-particle broadphase (doc 10.3). Arrays
    // only: iterating a hash map would make the results depend on its
    // internal order, which is exactly the kind of thing that silently
    // invalidates replays.
    std::vector<int32_t> grid_start_;
    std::vector<int32_t> grid_entries_;
    std::vector<int32_t> grid_cell_;
    int grid_mask_ = 0;
};

/// FNV-1a over raw float bits. Used for the contract hash and state hashes.
uint64_t HashBytes(const void* data, size_t len, uint64_t seed = 1469598103934665603ull);

}  // namespace ppp
