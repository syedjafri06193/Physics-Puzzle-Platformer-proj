#include "ppp/solver.h"

#include <algorithm>
#include <cstring>

namespace ppp {

// ---------------------------------------------------------------- hashing

uint64_t HashBytes(const void* data, size_t len, uint64_t seed) {
    const auto* p = static_cast<const unsigned char*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t PhysicsContract::Hash() const {
    // Hashing the object's bytes rather than field by field, so a field
    // added later is covered without anyone remembering to add it here.
    return HashBytes(this, sizeof(PhysicsContract));
}

bool PhysicsContract::operator==(const PhysicsContract& o) const {
    return std::memcmp(this, &o, sizeof(PhysicsContract)) == 0;
}

const PhysicsContract& DefaultContract() {
    static const PhysicsContract c{};
    return c;
}

// ------------------------------------------------------------- geometry

bool StaticGeometry::PointInside(const Vec2& p) const {
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (enabled[i] && boxes[i].Contains(p)) return true;
    }
    return false;
}

namespace {

/// Slab test against one box, for a segment. Returns the entry time in
/// [0, 1] and the face normal.
bool RaycastBox(const AABB& box, const Vec2& from, const Vec2& to, float* t_out, Vec2* n_out) {
    const Vec2 d = to - from;
    float t_min = 0.0f;
    float t_max = 1.0f;
    Vec2 normal = Vec2::Zero();

    // X slab, then Y. The axis that produced the latest entry is the face
    // that was hit.
    const float lo[2] = {box.min.x, box.min.y};
    const float hi[2] = {box.max.x, box.max.y};
    const float o[2] = {from.x, from.y};
    const float dir[2] = {d.x, d.y};

    for (int axis = 0; axis < 2; ++axis) {
        if (dir[axis] > -1e-9f && dir[axis] < 1e-9f) {
            // Parallel to this slab: either always inside it or never.
            if (o[axis] < lo[axis] || o[axis] > hi[axis]) return false;
            continue;
        }
        const float inv = 1.0f / dir[axis];
        float t1 = (lo[axis] - o[axis]) * inv;
        float t2 = (hi[axis] - o[axis]) * inv;
        float sign = -1.0f;
        if (t1 > t2) {
            const float tmp = t1;
            t1 = t2;
            t2 = tmp;
            sign = 1.0f;
        }
        if (t1 > t_min) {
            t_min = t1;
            normal = axis == 0 ? Vec2{sign, 0.0f} : Vec2{0.0f, sign};
        }
        if (t2 < t_max) t_max = t2;
        if (t_min > t_max) return false;
    }

    // A segment that starts inside the box has no entry face to report; the
    // caller's overlap resolution handles that case.
    if (t_min <= 0.0f) return false;

    *t_out = t_min;
    *n_out = normal;
    return true;
}

}  // namespace

bool StaticGeometry::Raycast(const Vec2& from, const Vec2& to, float radius, Hit* out) const {
    bool found = false;
    float best_t = 2.0f;
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (!enabled[i]) continue;
        float t;
        Vec2 n;
        // Expanding the box by the particle radius turns a point sweep into
        // a circle sweep, which is what the caller actually needs.
        if (RaycastBox(boxes[i].Expanded(radius), from, to, &t, &n) && t < best_t) {
            best_t = t;
            found = true;
            out->t = t;
            out->normal = n;
            out->point = from + (to - from) * t;
            out->box = static_cast<int>(i);
        }
    }
    return found;
}

bool StaticGeometry::Resolve(const Vec2& p, float radius, Vec2* corrected, Vec2* normal) const {
    bool any = false;
    Vec2 q = p;
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (!enabled[i]) continue;
        const AABB box = boxes[i].Expanded(radius);
        if (!box.Contains(q)) continue;

        // Push out along the shallowest axis: a particle a hair inside a
        // floor should be lifted, not teleported through the wall.
        const float dl = q.x - box.min.x;
        const float dr = box.max.x - q.x;
        const float db = q.y - box.min.y;
        const float dt = box.max.y - q.y;
        const float m = Min(Min(dl, dr), Min(db, dt));
        if (m == dl) {
            q.x = box.min.x;
            *normal = {-1.0f, 0.0f};
        } else if (m == dr) {
            q.x = box.max.x;
            *normal = {1.0f, 0.0f};
        } else if (m == db) {
            q.y = box.min.y;
            *normal = {0.0f, -1.0f};
        } else {
            q.y = box.max.y;
            *normal = {0.0f, 1.0f};
        }
        any = true;
    }
    if (any) *corrected = q;
    return any;
}

// --------------------------------------------------------------- solver

Solver::Solver(const PhysicsContract& contract) : contract_(contract) {}

void Solver::Clear() {
    particles_.Clear();
    constraints_.clear();
    bodies_.clear();
    geometry_.Clear();
    particle_body_.clear();
}

namespace {

/// Jakobsen's distance constraint (doc 4.4).
inline void SolveDistance(ParticleArray& p, const DistanceConstraint& c) {
    const Vec2 delta = p.pos[c.b] - p.pos[c.a];
    const float dist = delta.Length();
    if (dist < 1e-6f) return;  // coincident: no meaningful direction

    const float wa = p.inv_mass[c.a];
    const float wb = p.inv_mass[c.b];
    const float wsum = wa + wb;
    if (wsum == 0.0f) return;  // both pinned

    const float diff = (dist - c.rest) / dist;
    const Vec2 corr = delta * (diff * c.stiffness);

    // Mass weighting is what makes a heavy particle move less than a light
    // one; without it every constraint behaves as if both ends were equal.
    p.pos[c.a] += corr * (wa / wsum);
    p.pos[c.b] -= corr * (wb / wsum);
}

/// Shoelace area. Signed, so the sign reports the winding order — and
/// getting the winding backwards makes a pressure body implode instead of
/// inflate (doc 5.3).
float PolygonArea(const ParticleArray& p, const std::vector<int>& ring) {
    float a = 0.0f;
    const size_t n = ring.size();
    for (size_t i = 0; i < n; ++i) {
        const Vec2& p0 = p.pos[ring[i]];
        const Vec2& p1 = p.pos[ring[(i + 1) % n]];
        a += p0.x * p1.y - p1.x * p0.y;
    }
    return a * 0.5f;
}

}  // namespace

int Solver::AddLattice(const Vec2& origin, int w, int h, float spacing, float mass_per_particle,
                       int id) {
    SoftBody body;
    body.type = BodyType::Lattice;
    body.id = id;
    const int body_index = static_cast<int>(bodies_.size());
    const float inv_m = mass_per_particle > 0.0f ? 1.0f / mass_per_particle : 0.0f;

    const int base = static_cast<int>(particles_.Size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int idx = particles_.Add(origin + Vec2{x * spacing, y * spacing}, inv_m);
            particle_body_.push_back(body_index);
            body.particles.push_back(idx);
        }
    }

    auto at = [&](int x, int y) { return base + y * w + x; };
    const float diag = spacing * 1.41421356f;

    // Structural, shear, bend — in that order, and always in this order,
    // because Gauss-Seidel results depend on it (doc 6.2).
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (x + 1 < w)
                constraints_.push_back({at(x, y), at(x + 1, y), spacing,
                                        contract_.structural_stiffness, body_index});
            if (y + 1 < h)
                constraints_.push_back({at(x, y), at(x, y + 1), spacing,
                                        contract_.structural_stiffness, body_index});
            if (x + 1 < w && y + 1 < h) {
                constraints_.push_back(
                    {at(x, y), at(x + 1, y + 1), diag, contract_.shear_stiffness, body_index});
                constraints_.push_back(
                    {at(x + 1, y), at(x, y + 1), diag, contract_.shear_stiffness, body_index});
            }
            if (x + 2 < w)
                constraints_.push_back(
                    {at(x, y), at(x + 2, y), spacing * 2.0f, contract_.bend_stiffness, body_index});
            if (y + 2 < h)
                constraints_.push_back(
                    {at(x, y), at(x, y + 2), spacing * 2.0f, contract_.bend_stiffness, body_index});
        }
    }

    bodies_.push_back(std::move(body));
    return body_index;
}

int Solver::AddPressureBody(const Vec2& center, float radius, int count, float mass_per_particle,
                            float pressure, int id) {
    SoftBody body;
    body.type = BodyType::Pressure;
    body.id = id;
    body.pressure = pressure;
    const int body_index = static_cast<int>(bodies_.size());
    const float inv_m = mass_per_particle > 0.0f ? 1.0f / mass_per_particle : 0.0f;

    // The ring is built at the *inflated* radius, not the authored one.
    //
    // `pressure` is a target-area multiplier, and the area constraint and
    // the ring springs are both hard positional constraints. If the ring's
    // rest lengths describe one shape and the area target describes a
    // larger one, the two never agree: each solver iteration the springs
    // pull in, the area constraint pushes out, and Gauss-Seidel's
    // directional sweep turns that standoff into net motion. Measured: a
    // blob at pressure 1.05 drifted 182 units across a flat floor and never
    // slept; at 1.20 it gained enough energy to escape through the floor.
    //
    // Area scales as length squared, so a ring built at radius·√pressure has
    // exactly `pressure` times the authored area, and both constraints are
    // satisfied by the same shape. `radius` is then the rest radius and
    // `pressure` says how inflated the body is — which is what the two words
    // mean anyway. An author who wants a firmer blob of the same size raises
    // `pressure_stiffness` or `ring_stiffness`, which is the honest knob for
    // firmness (doc 5.3).
    const float inflated_radius = radius * std::sqrt(pressure > 0.0f ? pressure : 1.0f);

    // Clockwise winding, so PerpCW gives the outward normal. This is the
    // handedness that decides inflate vs implode.
    const float two_pi = 6.28318530718f;
    for (int i = 0; i < count; ++i) {
        const float a = -two_pi * static_cast<float>(i) / static_cast<float>(count);
        const Vec2 p = center + Vec2{std::cos(a) * inflated_radius, std::sin(a) * inflated_radius};
        const int idx = particles_.Add(p, inv_m);
        particle_body_.push_back(body_index);
        body.particles.push_back(idx);
        body.ring.push_back(idx);
    }

    // A ring of distance constraints so the boundary cannot stretch without
    // limit under pressure (doc 5.3).
    const float rest = (body.ring.size() >= 2)
                           ? (particles_.pos[body.ring[1]] - particles_.pos[body.ring[0]]).Length()
                           : 0.0f;
    for (int i = 0; i < count; ++i) {
        constraints_.push_back(
            {body.ring[i], body.ring[(i + 1) % count], rest, contract_.ring_stiffness, body_index});
    }
    // Skip-one springs: without them the ring folds in on itself when
    // squeezed hard, and the pressure force cannot push it back out.
    for (int i = 0; i < count; ++i) {
        const float r2 = (particles_.pos[body.ring[(i + 2) % count]] -
                          particles_.pos[body.ring[i]])
                             .Length();
        constraints_.push_back(
            {body.ring[i], body.ring[(i + 2) % count], r2, contract_.bend_stiffness, body_index});
    }

    // The area constraint targets `pressure * rest_area`, so `rest_area` is
    // the *uninflated* area — the ring was built inflated, so divide it back
    // out. Storing it this way keeps `pressure` live: lowering it at runtime
    // deflates the body, which is a mechanic worth having, and raising it
    // inflates one without rebuilding the ring.
    const float built_area = PolygonArea(particles_, body.ring);
    body.rest_area = pressure > 0.0f ? built_area / pressure : built_area;
    bodies_.push_back(std::move(body));
    return body_index;
}

int Solver::AddRope(const Vec2& from, const Vec2& to, int count, float mass_per_particle,
                    bool pin_start, bool pin_end, int id) {
    SoftBody body;
    body.type = BodyType::Rope;
    body.id = id;
    const int body_index = static_cast<int>(bodies_.size());
    const float inv_m = mass_per_particle > 0.0f ? 1.0f / mass_per_particle : 0.0f;

    for (int i = 0; i < count; ++i) {
        const float t = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f;
        const bool pinned = (i == 0 && pin_start) || (i == count - 1 && pin_end);
        const int idx = particles_.Add(from + (to - from) * t, pinned ? 0.0f : inv_m);
        particle_body_.push_back(body_index);
        body.particles.push_back(idx);
    }
    const float seg = count > 1 ? (to - from).Length() / static_cast<float>(count - 1) : 0.0f;
    for (int i = 0; i + 1 < count; ++i) {
        constraints_.push_back({body.particles[i], body.particles[i + 1], seg,
                                contract_.structural_stiffness, body_index});
    }

    bodies_.push_back(std::move(body));
    return body_index;
}

void Solver::SolveAreaConstraints() {
    // The gas model, as a position constraint rather than the design
    // document's force (doc 5.3).
    //
    // An explicit force `P = nRT / A` applied once per substep, fighting
    // ring constraints that are projected six times per substep, is an
    // undamped driven oscillator: it does work every substep with nothing
    // taking that energy back out. Measured, a blob dropped on a flat floor
    // gained energy indefinitely, rolled 170 units sideways, and never
    // settled — and "an object that never quite settles is maddening in a
    // puzzle" (doc 7.3).
    //
    // The positional form is Müller's PBD pressure constraint:
    //
    //     C = A − k·A_rest,  ∇ᵢC = ½·perp(pᵢ₊₁ − pᵢ₋₁),
    //     Δpᵢ = −C / Σⱼ wⱼ|∇ⱼC|² · wᵢ∇ᵢC
    //
    // It cannot inject energy, because it only ever moves positions toward
    // satisfying the constraint. Signs take care of themselves: the
    // gradient points along increasing *signed* area and the rest area is
    // captured with the same sign, so the winding order no longer decides
    // between inflating and imploding.
    for (auto& body : bodies_) {
        if (body.type != BodyType::Pressure || body.sleeping) continue;
        const size_t n = body.ring.size();
        if (n < 3) continue;

        const float area = PolygonArea(particles_, body.ring);
        const float target = body.rest_area * body.pressure;
        const float c = area - target;

        float denom = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            const int idx = body.ring[i];
            const Vec2& next = particles_.pos[body.ring[(i + 1) % n]];
            const Vec2& prev = particles_.pos[body.ring[(i + n - 1) % n]];
            const Vec2 grad{0.5f * (next.y - prev.y), 0.5f * (prev.x - next.x)};
            denom += particles_.inv_mass[idx] * grad.LengthSquared();
        }
        if (denom < 1e-9f) continue;  // every particle pinned, or degenerate

        const float lambda = -c / denom * contract_.pressure_stiffness;
        for (size_t i = 0; i < n; ++i) {
            const int idx = body.ring[i];
            const Vec2& next = particles_.pos[body.ring[(i + 1) % n]];
            const Vec2& prev = particles_.pos[body.ring[(i + n - 1) % n]];
            const Vec2 grad{0.5f * (next.y - prev.y), 0.5f * (prev.x - next.x)};
            particles_.pos[idx] += grad * (lambda * particles_.inv_mass[idx]);
        }
    }
}

void Solver::AddForce(int particle, const Vec2& force) {
    if (particle < 0 || particle >= static_cast<int>(particles_.Size())) return;
    particles_.accel[particle] += force * particles_.inv_mass[particle];
}

void Solver::Integrate(float dt) {
    const float dt2 = dt * dt;
    for (size_t i = 0; i < particles_.Size(); ++i) {
        if (particles_.inv_mass[i] == 0.0f) {
            particles_.prev[i] = particles_.pos[i];
            particles_.accel[i] = Vec2::Zero();
            continue;
        }
        const int b = particle_body_[i];
        if (b >= 0 && bodies_[b].sleeping) {
            particles_.accel[i] = Vec2::Zero();
            continue;
        }
        const Vec2 velocity = (particles_.pos[i] - particles_.prev[i]) * contract_.damping;
        particles_.prev[i] = particles_.pos[i];
        particles_.pos[i] += velocity + particles_.accel[i] * dt2;
        particles_.accel[i] = Vec2::Zero();
    }
}

void Solver::SolveConstraints(int iteration) {
    // A flat array in authored order. Never sorted, never parallelized
    // naively: both change the results and invalidate every replay.
    //
    // The sweep direction alternates by iteration, which is *not* what the
    // design document's loop does. A one-directional Gauss-Seidel sweep
    // around a closed ring biases every correction the same way around the
    // loop, and the visible result is a pressure body that rolls steadily
    // across a flat floor under its own solver — 170 units in three seconds
    // before this changed. Alternating cancels the bias and stays perfectly
    // deterministic, because the direction is a function of the iteration
    // index and nothing else.
    const size_t n = constraints_.size();
    if ((iteration & 1) == 0) {
        for (size_t i = 0; i < n; ++i) {
            const DistanceConstraint& c = constraints_[i];
            if (c.body >= 0 && bodies_[c.body].sleeping) continue;
            SolveDistance(particles_, c);
        }
    } else {
        for (size_t i = n; i-- > 0;) {
            const DistanceConstraint& c = constraints_[i];
            if (c.body >= 0 && bodies_[c.body].sleeping) continue;
            SolveDistance(particles_, c);
        }
    }
}

void Solver::ClearKinematic() { kinematic_.clear(); }

void Solver::AddKinematic(const AABB& box, const Vec2& motion) {
    kinematic_.push_back({box, motion});
}

void Solver::NoteContact(int p, const Vec2& normal, const Vec2& surface_motion) {
    // Normals accumulate: a particle wedged into a corner has two, and the
    // velocity pass should see their sum rather than whichever came last.
    contact_normal_[p] += normal;

    // Motion does NOT accumulate. It is the velocity of the surface the
    // particle is resting against, and a velocity is a property of one
    // surface, not a sum over however many times the position passes
    // happened to notice it.
    //
    // Accumulating it is a spectacular bug rather than a subtle one: this
    // runs inside the iteration loop, so a particle touching the player
    // collected the player's motion six times per substep and came out with
    // six times the player's speed. Walking into a blob launched it the
    // width of the level. With several kinematic surfaces in contact the
    // fastest one wins, which is the right answer for the case that
    // actually occurs — a player standing on a moving platform.
    if (surface_motion.LengthSquared() > contact_motion_[p].LengthSquared()) {
        contact_motion_[p] = surface_motion;
    }
    has_contact_[p] = 1;
}

void Solver::SweepCollisions() {
    const float r = contract_.particle_radius;
    for (size_t i = 0; i < particles_.Size(); ++i) {
        if (particles_.inv_mass[i] == 0.0f) continue;
        const int b = particle_body_[i];
        if (b >= 0 && bodies_[b].sleeping) continue;

        // `pos - prev` is this step's movement. There is no velocity vector
        // in Verlet, but the sweep along that segment is what stops a fast
        // particle crossing a thin wall (doc 4.5).
        StaticGeometry::Hit hit;
        if (geometry_.Raycast(particles_.prev[i], particles_.pos[i], r, &hit)) {
            particles_.pos[i] = hit.point + hit.normal * contract_.skin;
            NoteContact(static_cast<int>(i), hit.normal, Vec2::Zero());
        }
    }
}

void Solver::BuildGrid() {
    const size_t n = particles_.Size();
    int table = 16;
    while (static_cast<size_t>(table) < n * 2 && table < (1 << 20)) table <<= 1;
    grid_mask_ = table - 1;

    grid_start_.assign(static_cast<size_t>(table) + 1, 0);
    grid_entries_.resize(n);
    grid_cell_.resize(n);

    const float cell = contract_.particle_radius * 2.0f;
    auto cell_of = [&](const Vec2& p) {
        // floorf, not a cast: a cast truncates toward zero and puts -0.5
        // and +0.5 in the same cell, which drops contacts across the origin.
        const int ix = static_cast<int>(std::floor(p.x / cell));
        const int iy = static_cast<int>(std::floor(p.y / cell));
        const uint32_t h = static_cast<uint32_t>(ix) * 73856093u ^ static_cast<uint32_t>(iy) * 19349663u;
        return static_cast<int32_t>(h & static_cast<uint32_t>(grid_mask_));
    };

    for (size_t i = 0; i < n; ++i) {
        grid_cell_[i] = cell_of(particles_.pos[i]);
        ++grid_start_[static_cast<size_t>(grid_cell_[i]) + 1];
    }
    for (size_t i = 1; i < grid_start_.size(); ++i) grid_start_[i] += grid_start_[i - 1];

    // Counting sort, filled in ascending particle index, so each bucket is
    // ordered and the pair loop below is deterministic.
    std::vector<int32_t> cursor(grid_start_.begin(), grid_start_.end() - 1);
    for (size_t i = 0; i < n; ++i) {
        grid_entries_[static_cast<size_t>(cursor[static_cast<size_t>(grid_cell_[i])]++)] =
            static_cast<int32_t>(i);
    }
}

void Solver::ResolveParticlePairs() {
    const size_t n = particles_.Size();
    if (n < 2) return;
    const float r = contract_.particle_radius;
    const float min_dist = r * 2.0f;
    const float cell = min_dist;

    for (size_t i = 0; i < n; ++i) {
        const int bi = particle_body_[i];
        const bool sleeping_i = bi >= 0 && bodies_[bi].sleeping;
        const float wi = sleeping_i ? 0.0f : particles_.inv_mass[i];

        const Vec2 pi = particles_.pos[i];
        const int ix = static_cast<int>(std::floor(pi.x / cell));
        const int iy = static_cast<int>(std::floor(pi.y / cell));

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const uint32_t h = static_cast<uint32_t>(ix + dx) * 73856093u ^
                                   static_cast<uint32_t>(iy + dy) * 19349663u;
                const int32_t c = static_cast<int32_t>(h & static_cast<uint32_t>(grid_mask_));
                for (int32_t e = grid_start_[static_cast<size_t>(c)];
                     e < grid_start_[static_cast<size_t>(c) + 1]; ++e) {
                    const int j = grid_entries_[static_cast<size_t>(e)];
                    if (j <= static_cast<int>(i)) continue;  // each pair once

                    const int bj = particle_body_[j];
                    // Particles in the same body are held by constraints;
                    // repelling them as well fights the lattice.
                    if (bi >= 0 && bi == bj) continue;

                    const bool sleeping_j = bj >= 0 && bodies_[bj].sleeping;
                    if (sleeping_i && sleeping_j) continue;
                    const float wj = sleeping_j ? 0.0f : particles_.inv_mass[j];
                    const float wsum = wi + wj;
                    if (wsum == 0.0f) continue;

                    Vec2 d = particles_.pos[j] - particles_.pos[i];
                    float dist = d.Length();
                    if (dist >= min_dist) continue;
                    if (dist < 1e-6f) {
                        // Exactly coincident: pick a fixed direction rather
                        // than dividing by zero. Deterministic by index.
                        d = Vec2{static_cast<float>((j - static_cast<int>(i)) % 2 == 0 ? 1 : -1), 0.0f};
                        dist = 1.0f;
                    }
                    const Vec2 normal = d / dist;
                    const float overlap = min_dist - dist;
                    particles_.pos[i] -= normal * (overlap * (wi / wsum));
                    particles_.pos[j] += normal * (overlap * (wj / wsum));

                    NoteContact(static_cast<int>(i), -normal, Vec2::Zero());
                    NoteContact(j, normal, Vec2::Zero());

                    // A moving body must wake whatever it lands on, or a
                    // sleeping stack behaves like static geometry forever.
                    if (sleeping_i && !sleeping_j) Wake(bi);
                    if (sleeping_j && !sleeping_i) Wake(bj);
                }
            }
        }
    }
}

void Solver::ResolveOverlaps() {
    const float r = contract_.particle_radius;
    for (size_t i = 0; i < particles_.Size(); ++i) {
        if (particles_.inv_mass[i] == 0.0f) continue;
        const int b = particle_body_[i];

        if (b >= 0 && bodies_[b].sleeping) {
            // A sleeping body still has to notice something moving into it,
            // or the player walks straight through a blob that happens to
            // have settled. Sleeping particles are not *moved* here — waking
            // takes effect next step, when the body is awake and the normal
            // path runs — but the overlap test has to happen before the
            // skip, not after it.
            bool woken = false;
            for (const auto& k : kinematic_) {
                if (!k.box.Expanded(r).Contains(particles_.pos[i])) continue;
                Wake(b);
                woken = true;
                break;
            }
            if (!woken) continue;
        }

        Vec2 corrected;
        Vec2 normal;
        if (geometry_.Resolve(particles_.pos[i], r, &corrected, &normal)) {
            particles_.pos[i] = corrected + normal * contract_.skin;
            NoteContact(static_cast<int>(i), normal, Vec2::Zero());
        }

        // Kinematic boxes: the player, and anything the game moves. Their
        // motion is carried into the contact so friction drags particles
        // along with them.
        for (const auto& k : kinematic_) {
            const AABB box = k.box.Expanded(r);
            if (!box.Contains(particles_.pos[i])) continue;
            const Vec2 p = particles_.pos[i];
            const float dl = p.x - box.min.x;
            const float dr = box.max.x - p.x;
            const float db = p.y - box.min.y;
            const float dt = box.max.y - p.y;
            const float m = Min(Min(dl, dr), Min(db, dt));
            Vec2 n;
            Vec2 q = p;
            if (m == dl) { q.x = box.min.x; n = {-1.0f, 0.0f}; }
            else if (m == dr) { q.x = box.max.x; n = {1.0f, 0.0f}; }
            else if (m == db) { q.y = box.min.y; n = {0.0f, -1.0f}; }
            else { q.y = box.max.y; n = {0.0f, 1.0f}; }
            particles_.pos[i] = q + n * contract_.skin;
            // `motion` is how far the surface moved over a whole step, and
            // the velocity pass works in per-substep displacement, so it is
            // divided here rather than at the call site — the game layer
            // should not have to know how many substeps the contract runs.
            NoteContact(static_cast<int>(i), n,
                        k.motion / static_cast<float>(contract_.substeps));
            if (b >= 0) Wake(b);
        }
    }
}

void Solver::ApplyContactVelocities() {
    for (size_t i = 0; i < particles_.Size(); ++i) {
        if (!has_contact_[i]) continue;
        if (particles_.inv_mass[i] == 0.0f) continue;
        const int b = particle_body_[i];
        if (b >= 0 && bodies_[b].sleeping) continue;

        const Vec2 n = contact_normal_[i].Normalized();
        if (n.LengthSquared() == 0.0f) continue;

        // Velocities relative to the surface, so a particle carried by a
        // moving platform is not fought by its own friction.
        const Vec2 v_post = (particles_.pos[i] - particles_.prev[i]) - contact_motion_[i];
        const Vec2 v_pre = pre_velocity_[i] - contact_motion_[i];

        const float vn_post = Vec2::Dot(v_post, n);
        const float vn_pre = Vec2::Dot(v_pre, n);

        // Tangential from the solved velocity, normal from the approach
        // speed — Müller's PBD contact update. A resting particle approaches
        // at gravity·dt², so its bounce is negligible and it settles;
        // deriving the bounce from v_post instead would return the push-out
        // as speed and the body would buzz forever, never sleeping.
        const Vec2 v_tangent = v_post - n * vn_post;
        const float bounce = vn_pre < 0.0f ? -vn_pre * contract_.restitution : 0.0f;

        Vec2 out = v_tangent * (1.0f - contract_.friction) + n * bounce;
        if (vn_post > 0.0f && bounce == 0.0f) {
            // Already separating and not a bounce: keep the separation, or a
            // particle pushed out of a wall would be pinned against it.
            //
            // But only as much separation as the particle *already had*.
            // `vn_post` includes the position passes' depenetration, and
            // depenetration is not velocity: handing it back turns "you were
            // 12 units inside this box" into "you are now travelling at 12
            // units per substep". That is how running into a resting blob
            // launched it across the level — the player's box swallowed its
            // lower half in one step, and the push-out that followed came
            // back as speed. Capping at `vn_pre` keeps genuine motion and
            // discards the correction.
            out += n * Min(vn_post, Max(vn_pre, 0.0f));
        }
        particles_.prev[i] = particles_.pos[i] - (out + contact_motion_[i]);
    }
}

void Solver::UpdateSleeping(float dt) {
    for (size_t bi = 0; bi < bodies_.size(); ++bi) {
        SoftBody& body = bodies_[bi];
        if (body.particles.empty()) continue;
        if (body.grabbed) {
            body.sleeping = false;
            body.sleep_timer = 0.0f;
            continue;
        }

        float energy = 0.0f;
        for (int idx : body.particles) {
            const Vec2 v = particles_.pos[idx] - particles_.prev[idx];
            energy += v.LengthSquared();
        }
        energy /= static_cast<float>(body.particles.size());

        if (energy < contract_.sleep_threshold) {
            body.sleep_timer += dt;
            if (body.sleep_timer > contract_.sleep_delay && !body.sleeping) {
                body.sleeping = true;
                // Zero the implicit velocity, or the body lurches when it
                // wakes with whatever residual motion it had (doc 14.2).
                for (int idx : body.particles) particles_.prev[idx] = particles_.pos[idx];
            }
        } else {
            body.sleep_timer = 0.0f;
            body.sleeping = false;
        }
    }
}

void Solver::Wake(int body) {
    if (body < 0 || body >= static_cast<int>(bodies_.size())) return;
    bodies_[body].sleeping = false;
    bodies_[body].sleep_timer = 0.0f;
}

void Solver::Step(float dt) {
    const float sub_dt = dt / static_cast<float>(contract_.substeps);
    const size_t n = particles_.Size();
    contact_normal_.assign(n, Vec2::Zero());
    contact_motion_.assign(n, Vec2::Zero());
    has_contact_.assign(n, 0);

    for (int s = 0; s < contract_.substeps; ++s) {
        for (size_t i = 0; i < n; ++i) {
            if (particles_.inv_mass[i] > 0.0f) particles_.accel[i].y += contract_.gravity;
        }
        Integrate(sub_dt);
        pre_velocity_.resize(n);
        for (size_t i = 0; i < n; ++i) pre_velocity_[i] = particles_.pos[i] - particles_.prev[i];

        // Position passes first, one velocity pass afterwards — the
        // Position-Based Dynamics structure. Restitution and friction
        // applied inside the iteration loop instead would feed
        // Gauss-Seidel's directional bias into the velocity once per
        // iteration, and a ring-shaped body then walks sideways across a
        // flat floor.
        std::fill(contact_normal_.begin(), contact_normal_.end(), Vec2::Zero());
        std::fill(contact_motion_.begin(), contact_motion_.end(), Vec2::Zero());
        std::fill(has_contact_.begin(), has_contact_.end(), 0);

        SweepCollisions();

        // Fixed iteration count and fixed order: both are part of the
        // physics contract (docs/physics-contract.md). Changing either
        // changes the simulation and invalidates every recorded replay, so
        // the regression suite has to run before this line is touched.
        BuildGrid();
        for (int i = 0; i < contract_.iterations; ++i) {
            SolveConstraints(i);
            SolveAreaConstraints();
            ResolveOverlaps();
            ResolveParticlePairs();
        }

        ApplyContactVelocities();
    }

    UpdateSleeping(dt);
}

int Solver::BodyNear(const Vec2& p, float max_distance) const {
    int best = -1;
    float best_d2 = max_distance * max_distance;
    for (size_t b = 0; b < bodies_.size(); ++b) {
        for (int idx : bodies_[b].particles) {
            const float d2 = (particles_.pos[idx] - p).LengthSquared();
            if (d2 < best_d2) {
                best_d2 = d2;
                best = static_cast<int>(b);
            }
        }
    }
    return best;
}

Vec2 Solver::BodyCenter(int body) const {
    if (body < 0 || body >= static_cast<int>(bodies_.size())) return Vec2::Zero();
    const auto& ps = bodies_[body].particles;
    if (ps.empty()) return Vec2::Zero();
    Vec2 sum = Vec2::Zero();
    for (int idx : ps) sum += particles_.pos[idx];
    return sum / static_cast<float>(ps.size());
}

int Solver::NearestParticle(int body, const Vec2& p) const {
    if (body < 0 || body >= static_cast<int>(bodies_.size())) return -1;
    int best = -1;
    float best_d2 = 1e30f;
    for (int idx : bodies_[body].particles) {
        const float d2 = (particles_.pos[idx] - p).LengthSquared();
        if (d2 < best_d2) {
            best_d2 = d2;
            best = idx;
        }
    }
    return best;
}

float Solver::MassInside(const AABB& region) const {
    float mass = 0.0f;
    for (size_t i = 0; i < particles_.Size(); ++i) {
        if (particles_.inv_mass[i] <= 0.0f) continue;
        if (region.Contains(particles_.pos[i])) mass += 1.0f / particles_.inv_mass[i];
    }
    return mass;
}

uint64_t Solver::StateHash() const {
    uint64_t h = 1469598103934665603ull;
    // Positions and previous positions: prev carries the implicit velocity,
    // so hashing only pos would miss a body moving differently at the same
    // instant (doc 6.4).
    h = HashBytes(particles_.pos.data(), particles_.pos.size() * sizeof(Vec2), h);
    h = HashBytes(particles_.prev.data(), particles_.prev.size() * sizeof(Vec2), h);
    for (const auto& b : bodies_) {
        const uint8_t flags = static_cast<uint8_t>((b.sleeping ? 1 : 0) | (b.grabbed ? 2 : 0));
        h = HashBytes(&flags, sizeof(flags), h);
    }
    for (size_t i = 0; i < geometry_.enabled.size(); ++i) {
        h = HashBytes(&geometry_.enabled[i], 1, h);
    }
    return h;
}

int Solver::AwakeBodyCount() const {
    int n = 0;
    for (const auto& b : bodies_) {
        if (!b.sleeping) ++n;
    }
    return n;
}

}  // namespace ppp
