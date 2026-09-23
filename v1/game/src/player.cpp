#include "ppp/player.h"

#include <cmath>
#include <cstring>

namespace ppp {
namespace {

/// Moving the box by `delta` on one axis at a time, stopping at the first
/// surface. Axis-separated movement is what makes "slide along a wall"
/// fall out for free: the blocked axis stops and the other one carries on.
///
/// It is also why the box is swept rather than moved-then-pushed-out. A
/// player falling at the contract's max fall speed covers 5.2 units per
/// step, and a one-unit-thick ledge is a legal thing for a level to
/// contain once the wall-thickness rule (doc 4.5) is about *particles*
/// tunnelling, not the player.
struct SweepResult {
    float moved = 0.0f;
    bool hit = false;
};

SweepResult SweepAxis(const AABB& box, float delta, int axis, const StaticGeometry& geo) {
    SweepResult out;
    out.moved = delta;
    if (delta == 0.0f) return out;

    const float lo = axis == 0 ? box.min.x : box.min.y;
    const float hi = axis == 0 ? box.max.x : box.max.y;
    // The other axis must already overlap for a hit to count.
    const float olo = axis == 0 ? box.min.y : box.min.x;
    const float ohi = axis == 0 ? box.max.y : box.max.x;

    float best = delta;
    for (size_t i = 0; i < geo.boxes.size(); ++i) {
        if (!geo.enabled[i]) continue;
        const AABB& b = geo.boxes[i];
        const float blo = axis == 0 ? b.min.x : b.min.y;
        const float bhi = axis == 0 ? b.max.x : b.max.y;
        const float bolo = axis == 0 ? b.min.y : b.min.x;
        const float bohi = axis == 0 ? b.max.y : b.max.x;

        if (ohi <= bolo || olo >= bohi) continue;

        if (delta > 0.0f) {
            if (hi <= blo) {
                const float gap = blo - hi;
                if (gap < best) best = gap;
            }
        } else {
            if (lo >= bhi) {
                const float gap = bhi - lo;
                if (gap > best) best = gap;
            }
        }
    }

    if (best != delta) {
        out.hit = true;
        out.moved = best;
    }
    return out;
}

bool BoxOverlapsGeometry(const AABB& box, const StaticGeometry& geo) {
    for (size_t i = 0; i < geo.boxes.size(); ++i) {
        if (geo.enabled[i] && box.Overlaps(geo.boxes[i])) return true;
    }
    return false;
}

/// Push the player out of any solid it is inside, along the shallowest axis.
///
/// The swept move on its own is not enough. A sweep only refuses to *enter*
/// a solid; it has nothing to say about a box that is already overlapping
/// one, and it reports no hit in that case — so once the player is inside
/// the floor by any amount, the next downward sweep sees a clear path and
/// the player accelerates through the level and out of the world.
///
/// The player gets inside a solid two ways: a soft body shoves it there
/// (which is the whole point of having soft bodies), and a door closes on
/// it. Both are normal. Recovery has to be part of the step, not an
/// assertion that it never happens.
///
/// Returns which directions the player was pushed, so the caller can treat
/// an upward push as standing on the ground.
struct Depenetration {
    bool from_below = false;  ///< pushed up: the player was in the floor
    bool from_above = false;
    bool sideways = false;
};

Depenetration DepenetrateGeometry(Player* p, const PlayerTuning& t, const StaticGeometry& geo) {
    Depenetration out;
    // Boxes in index order, twice. One pass is enough for a single wall and
    // not enough for an inside corner, where being pushed out of the floor
    // can put the player inside the wall beside it.
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < geo.boxes.size(); ++i) {
            if (!geo.enabled[i]) continue;
            const AABB box = PlayerBox(*p, t);
            const AABB& b = geo.boxes[i];
            if (!box.Overlaps(b)) continue;

            const float left = box.max.x - b.min.x;   // push player left by this
            const float right = b.max.x - box.min.x;  // push player right
            const float up = box.max.y - b.min.y;     // push player up
            const float down = b.max.y - box.min.y;   // push player down
            const float m = Min(Min(left, right), Min(up, down));
            if (m <= 0.0f) continue;

            if (m == up) {
                p->pos.y -= up;
                out.from_below = true;
            } else if (m == down) {
                p->pos.y += down;
                out.from_above = true;
            } else if (m == left) {
                p->pos.x -= left;
                out.sideways = true;
            } else {
                p->pos.x += right;
                out.sideways = true;
            }
        }
    }
    return out;
}

}  // namespace

uint64_t PlayerTuning::Hash() const { return HashBytes(this, sizeof(PlayerTuning)); }

bool PlayerTuning::operator==(const PlayerTuning& o) const {
    return std::memcmp(this, &o, sizeof(PlayerTuning)) == 0;
}

const PlayerTuning& DefaultPlayerTuning() {
    static const PlayerTuning t;
    return t;
}

AABB PlayerBox(const Player& p, const PlayerTuning& t) {
    return AABB{{p.pos.x - t.half_width, p.pos.y - t.half_height},
                {p.pos.x + t.half_width, p.pos.y + t.half_height}};
}

Vec2 HandPosition(const Player& p, const PlayerTuning& t) {
    return {p.pos.x + static_cast<float>(p.facing) * t.hand_reach, p.pos.y};
}

// ------------------------------------------------------------------ step

void StepPlayer(Player* p, const PlayerTuning& t, const InputState& in, float dt,
                const StaticGeometry& geometry, Solver* solver) {
    p->prev_pos = p->pos;

    // Recover from anything that ended last step with the player inside a
    // solid — a soft body shoving it, or a door closing on it.
    {
        const Depenetration fixed = DepenetrateGeometry(p, t, geometry);
        if (fixed.from_below) {
            p->on_floor = true;
            if (p->vel.y > 0.0f) p->vel.y = 0.0f;
        }
        if (fixed.from_above && p->vel.y < 0.0f) p->vel.y = 0.0f;
        if (fixed.sideways) p->vel.x = 0.0f;
    }

    // --- horizontal ---------------------------------------------------
    const float target = in.move_x * t.run_speed;
    const bool pushing = std::fabs(in.move_x) > 0.01f;
    if (pushing) p->facing = in.move_x > 0.0f ? 1 : -1;

    const float accel = p->on_floor ? t.ground_accel : t.air_accel;
    const float friction = p->on_floor ? t.ground_friction : t.air_friction;

    if (pushing) {
        // Turning around uses acceleration *and* friction, so a direction
        // change is sharp rather than a long slide through zero.
        const float rate = (target * p->vel.x < 0.0f) ? accel + friction : accel;
        const float step = rate * dt;
        if (p->vel.x < target) p->vel.x = Min(p->vel.x + step, target);
        else p->vel.x = Max(p->vel.x - step, target);
    } else {
        const float step = friction * dt;
        if (p->vel.x > 0.0f) p->vel.x = Max(0.0f, p->vel.x - step);
        else p->vel.x = Min(0.0f, p->vel.x + step);
    }

    // --- jump timers (doc 7.1) ----------------------------------------
    if (p->on_floor) p->coyote_timer = t.coyote_time;
    else p->coyote_timer = Max(0.0f, p->coyote_timer - dt);

    const bool jump_pressed = in.jump && !p->prev_jump;
    const bool jump_released = !in.jump && p->prev_jump;
    if (jump_pressed) p->jump_buffer_timer = t.jump_buffer;
    else p->jump_buffer_timer = Max(0.0f, p->jump_buffer_timer - dt);

    if (p->jump_buffer_timer > 0.0f && p->coyote_timer > 0.0f) {
        p->vel.y = -t.jump_speed;  // screen space: up is negative
        // Both timers are consumed. Leaving the coyote timer set is the bug
        // the design document calls out: one buffered press then fires
        // twice, which reads as a double jump nobody asked for (doc 7.1).
        p->jump_buffer_timer = 0.0f;
        p->coyote_timer = 0.0f;
        p->on_floor = false;
        p->jump_held = true;
    }

    // Variable jump height: releasing during the rise cuts it short.
    if (jump_released && p->jump_held && p->vel.y < 0.0f) {
        p->vel.y *= t.jump_release_cut;
        p->jump_held = false;
    }
    if (p->vel.y >= 0.0f) p->jump_held = false;

    // --- gravity ------------------------------------------------------
    // Falling is 1.8× the rise gravity — the design document's "fast-fall
    // gravity", which is a permanent asymmetry between rise and fall, not a
    // button (doc 7.1). Rising floaty, falling weighty.
    float g = t.gravity;
    if (p->vel.y > 0.0f) g *= t.fall_gravity_scale;
    if (std::fabs(p->vel.y) < t.apex_speed && !p->on_floor) {
        // Apex hang (doc 7.1): a moment of float at the top of the arc.
        g *= t.apex_gravity_scale;
    }
    p->vel.y += g * dt;
    if (p->vel.y > t.max_fall_speed) p->vel.y = t.max_fall_speed;

    // --- move, axis at a time ------------------------------------------
    p->against_wall = false;
    p->on_ceiling = false;
    const bool was_on_floor = p->on_floor;
    p->on_floor = false;

    {
        AABB box = PlayerBox(*p, t);
        const SweepResult sx = SweepAxis(box, p->vel.x * dt, 0, geometry);
        p->pos.x += sx.moved;
        if (sx.hit) {
            p->against_wall = true;
            p->vel.x = 0.0f;
        }
    }

    {
        AABB box = PlayerBox(*p, t);
        const float dy = p->vel.y * dt;
        SweepResult sy = SweepAxis(box, dy, 1, geometry);

        // Corner correction (doc 7.1): a rising jump that clips a corner by
        // a unit or two is nudged sideways instead of stopped dead. Only
        // upward, and only by `corner_correction` — applying it downward
        // would teleport the player off ledges they meant to land on.
        if (sy.hit && dy < 0.0f && t.corner_correction > 0.0f) {
            bool corrected = false;
            // Smallest nudge first, and each distance tried both ways before
            // the next one, so the player is moved the least that works.
            for (float nudge = 0.5f; !corrected && nudge <= t.corner_correction + 1e-4f;
                 nudge += 0.5f) {
                for (int sign = -1; sign <= 1; sign += 2) {
                    Player probe = *p;
                    probe.pos.x += static_cast<float>(sign) * nudge;
                    const AABB pbox = PlayerBox(probe, t);
                    if (BoxOverlapsGeometry(pbox, geometry)) continue;
                    const SweepResult alt = SweepAxis(pbox, dy, 1, geometry);
                    if (alt.hit) continue;
                    p->pos.x = probe.pos.x;
                    sy = alt;
                    corrected = true;
                    break;
                }
            }
        }

        p->pos.y += sy.moved;
        if (sy.hit) {
            if (dy > 0.0f) {
                p->on_floor = true;
            } else {
                p->on_ceiling = true;
                p->jump_held = false;
            }
            p->vel.y = 0.0f;
        }
    }

    // --- standing on soft bodies ---------------------------------------
    //
    // A blob you cannot stand on is scenery. Each particle is a circle; the
    // player is pushed out along the shallowest axis, exactly as the solver
    // pushes particles out of geometry, and landing on top counts as floor.
    // Particles are visited in index order, which is fixed for the life of
    // the level, so this stays deterministic (doc 6.2).
    if (solver != nullptr) {
        const auto& particles = solver->Particles();
        const float r = solver->Contract().particle_radius;
        float lift = 0.0f;
        for (size_t i = 0; i < particles.Size(); ++i) {
            if (particles.inv_mass[i] <= 0.0f) continue;
            const AABB box = PlayerBox(*p, t);
            const Vec2& q = particles.pos[i];
            if (q.x < box.min.x - r || q.x > box.max.x + r) continue;
            if (q.y < box.min.y - r || q.y > box.max.y + r) continue;

            const float left = q.x + r - box.min.x;
            const float right = box.max.x - (q.x - r);
            const float top = q.y + r - box.min.y;
            const float bottom = box.max.y - (q.y - r);
            const float min_pen = Min(Min(left, right), Min(top, bottom));
            if (min_pen <= 0.0f) continue;

            if (min_pen == bottom) {
                // Particle below the player's feet: stand on it.
                p->pos.y -= bottom;
                if (p->vel.y > 0.0f) p->vel.y = 0.0f;
                p->on_floor = true;
                lift += bottom;
            } else if (min_pen == top) {
                p->pos.y += top;
                if (p->vel.y < 0.0f) p->vel.y = 0.0f;
            } else if (min_pen == left) {
                p->pos.x -= left;
                if (p->vel.x > 0.0f) p->vel.x = 0.0f;
            } else {
                p->pos.x += right;
                if (p->vel.x < 0.0f) p->vel.x = 0.0f;
            }
        }
        (void)lift;

        // A soft body is allowed to shove the player around; it is not
        // allowed to shove the player into a wall. Geometry wins, always,
        // and it wins *after* the bodies have had their say.
        const Depenetration fixed = DepenetrateGeometry(p, t, geometry);
        if (fixed.from_below) {
            p->on_floor = true;
            if (p->vel.y > 0.0f) p->vel.y = 0.0f;
        }
        if (fixed.from_above && p->vel.y < 0.0f) p->vel.y = 0.0f;
        if (fixed.sideways) {
            p->against_wall = true;
            p->vel.x = 0.0f;
        }
    }

    if (p->on_floor) {
        p->coyote_timer = t.coyote_time;
        p->jump_held = false;
    } else if (was_on_floor && p->vel.y >= 0.0f) {
        // Walked off a ledge: the coyote window starts now, not whenever
        // the fall happens to be noticed.
        if (p->coyote_timer <= 0.0f) p->coyote_timer = t.coyote_time;
    }

    // --- grab (doc 7.3) -------------------------------------------------
    if (solver != nullptr) {
        const bool grab_pressed = in.grab && !p->prev_grab;
        const bool grab_released = !in.grab && p->prev_grab;

        if (grab_released && p->grabbed_body >= 0) {
            solver->MutableBodies()[static_cast<size_t>(p->grabbed_body)].grabbed = false;
            p->grabbed_body = -1;
            p->grabbed_particle = -1;
        }

        const Vec2 hand = HandPosition(*p, t);

        if (grab_pressed && p->grabbed_body < 0) {
            const int body = solver->BodyNear(hand, t.grab_range);
            if (body >= 0) {
                p->grabbed_body = body;
                p->grabbed_particle = solver->NearestParticle(body, hand);
                solver->MutableBodies()[static_cast<size_t>(body)].grabbed = true;
                solver->Wake(body);
            }
        }

        if (p->grabbed_body >= 0 && p->grabbed_particle >= 0) {
            const auto& particles = solver->Particles();
            const size_t idx = static_cast<size_t>(p->grabbed_particle);
            const Vec2 to_hand = hand - particles.pos[idx];
            // Damped spring, in *velocity* terms. The particle's velocity is
            // implicit in pos - prev (doc 4.1), so that difference is what
            // the damping term has to read.
            const Vec2 particle_vel = (particles.pos[idx] - particles.prev[idx]) / dt;
            Vec2 force = to_hand * t.grab_stiffness - particle_vel * t.grab_damping;
            const float mag2 = force.LengthSquared();
            if (mag2 > t.grab_max_force * t.grab_max_force) {
                force = force.Normalized() * t.grab_max_force;
            }
            solver->AddForce(p->grabbed_particle, force);
            solver->Wake(p->grabbed_body);
        }

        p->prev_grab = in.grab;
    }

    p->prev_jump = in.jump;
}

}  // namespace ppp
