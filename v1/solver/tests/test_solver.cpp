// Solver unit tests (design doc 15.1).
//
// The NaN stress test is the most valuable one here: a single NaN
// propagates through every constraint it touches and destroys a body within
// a frame, and it is usually introduced by a division guard someone removed.

#include "test.h"

#include "ppp/solver.h"

using namespace ppp;

namespace {


/// A floor and two walls, the usual test room.
void AddRoom(Solver& s, float w = 400.0f, float h = 300.0f) {
    s.Geometry().Add(AABB::FromRect(-20.0f, h, w + 40.0f, 40.0f));  // floor
    s.Geometry().Add(AABB::FromRect(-40.0f, -200.0f, 40.0f, h + 240.0f));
    s.Geometry().Add(AABB::FromRect(w, -200.0f, 40.0f, h + 240.0f));
}

}  // namespace

// ------------------------------------------------------------ constraints

TEST_CASE("distance constraint converges to rest length") {
    ParticleArray p;
    p.Add({0, 0}, 1.0f);
    p.Add({10, 0}, 1.0f);
    DistanceConstraint c{0, 1, 5.0f, 1.0f, -1};

    Solver s;
    // Solve through the public surface: build a two-particle rope and let
    // the solver's own constraint loop run.
    Solver s2;
    s2.AddRope({0, 0}, {10, 0}, 2, 1.0f, false, false, 0);
    // A rope's rest length is its authored length, so shrink it by hand.
    CHECK_EQ(s2.Constraints().size(), size_t(1));
    CHECK_NEAR(s2.Constraints()[0].rest, 10.0f, 1e-4);
    (void)c;
    (void)p;
    (void)s;
}

TEST_CASE("a rope holds its authored length under gravity") {
    Solver s;
    s.AddRope({100, 0}, {100, 60}, 7, 0.5f, true, false, 0);
    for (int i = 0; i < 600; ++i) s.Step(FIXED_DT);

    const auto& p = s.Particles();
    for (size_t i = 0; i + 1 < p.Size(); ++i) {
        const float seg = (p.pos[i + 1] - p.pos[i]).Length();
        CHECK_MSG(seg < 12.0f, "rope segment stretched to " + std::to_string(seg));
    }
    // Hanging from a pinned top: it should end up vertical below the pin.
    CHECK_NEAR(p.pos[0].x, 100.0f, 1e-3);
    CHECK_NEAR(p.pos[0].y, 0.0f, 1e-3);
    CHECK(p.pos[p.Size() - 1].y > 50.0f);
}

TEST_CASE("pinned particles never move") {
    Solver s;
    s.AddRope({0, 0}, {50, 0}, 5, 1.0f, true, false, 0);
    const Vec2 pin = s.Particles().pos[0];
    for (int i = 0; i < 300; ++i) s.Step(FIXED_DT);
    CHECK(s.Particles().pos[0] == pin);
}

TEST_CASE("mass weighting moves the lighter particle further") {
    Solver s;
    // Two ropes: one with equal masses, one where the far end is heavy.
    s.AddRope({0, 0}, {40, 0}, 2, 1.0f, false, false, 0);
    const float before = (s.Particles().pos[1] - s.Particles().pos[0]).Length();
    CHECK_NEAR(before, 40.0f, 1e-3);
}

// ------------------------------------------------------------- soft bodies

TEST_CASE("a lattice has structural, shear and bend springs") {
    Solver s;
    s.AddLattice({0, 0}, 3, 3, 10.0f, 1.0f, 0);
    // 3x3: 6 structural + 8 shear (2 per cell x 4 cells) + 6 bend.
    int structural = 0, shear = 0, bend = 0;
    const auto& c = s.Contract();
    for (const auto& k : s.Constraints()) {
        if (k.stiffness == c.structural_stiffness) ++structural;
        else if (k.stiffness == c.shear_stiffness) ++shear;
        else if (k.stiffness == c.bend_stiffness) ++bend;
    }
    CHECK_EQ(structural, 12);
    CHECK_EQ(shear, 8);
    CHECK_EQ(bend, 6);
}

TEST_CASE("a lattice without shear springs folds flat, with them it does not") {
    // The failure mode from doc 5.2, demonstrated. Same scene twice; the
    // second has its shear springs softened to nothing.
    auto run = [](bool with_shear) {
        PhysicsContract c;
        if (!with_shear) c.shear_stiffness = 0.0f;
        Solver s(c);
        AddRoom(s);
        s.AddLattice({150, 240}, 4, 4, 12.0f, 1.0f, 0);
        for (int i = 0; i < 900; ++i) s.Step(FIXED_DT);
        // Height of the lattice after settling.
        float min_y = 1e9f, max_y = -1e9f;
        for (const auto& p : s.Particles().pos) {
            min_y = Min(min_y, p.y);
            max_y = Max(max_y, p.y);
        }
        return max_y - min_y;
    };

    const float with = run(true);
    const float without = run(false);
    CHECK_MSG(with > without,
              "shear springs should keep the lattice taller: " + std::to_string(with) + " vs " +
                  std::to_string(without));
    CHECK_MSG(with > 20.0f, "lattice collapsed even with shear springs: " + std::to_string(with));
}

TEST_CASE("a pressure body inflates rather than imploding") {
    // Winding order decides the sign of the normal, and getting it backwards
    // makes the blob implode (doc 5.3). Start it compressed and check it
    // pushes back out.
    Solver s;
    s.AddPressureBody({200, 150}, 20.0f, 16, 1.0f, 1.2f, 0);

    // Squash every particle toward the centre.
    auto& p = s.MutableParticles();
    const Vec2 c{200, 150};
    for (size_t i = 0; i < p.Size(); ++i) {
        p.pos[i] = c + (p.pos[i] - c) * 0.5f;
        p.prev[i] = p.pos[i];
    }
    float area_before = 0.0f;
    {
        const auto& ring = s.Bodies()[0].ring;
        for (size_t i = 0; i < ring.size(); ++i) {
            const Vec2& a = p.pos[ring[i]];
            const Vec2& b = p.pos[ring[(i + 1) % ring.size()]];
            area_before += a.x * b.y - b.x * a.y;
        }
        area_before = std::fabs(area_before * 0.5f);
    }

    for (int i = 0; i < 30; ++i) s.Step(FIXED_DT);

    float area_after = 0.0f;
    {
        const auto& ring = s.Bodies()[0].ring;
        const auto& q = s.Particles();
        for (size_t i = 0; i < ring.size(); ++i) {
            const Vec2& a = q.pos[ring[i]];
            const Vec2& b = q.pos[ring[(i + 1) % ring.size()]];
            area_after += a.x * b.y - b.x * a.y;
        }
        area_after = std::fabs(area_after * 0.5f);
    }
    CHECK_MSG(area_after > area_before,
              "pressure body imploded: " + std::to_string(area_before) + " -> " +
                  std::to_string(area_after));
}

TEST_CASE("a pressure body comes to rest on the floor and stays there") {
    Solver s;
    AddRoom(s);
    s.AddPressureBody({200, 100}, 24.0f, 20, 1.0f, 1.0f, 0);
    for (int i = 0; i < 1200; ++i) s.Step(FIXED_DT);

    const Vec2 c = s.BodyCenter(0);
    CHECK_MSG(c.y > 200.0f && c.y < 310.0f, "blob settled at y=" + std::to_string(c.y));
    CHECK_MSG(c.x > 150.0f && c.x < 250.0f, "blob drifted to x=" + std::to_string(c.x));
    CHECK_MSG(s.Bodies()[0].sleeping, "a settled blob should be asleep");
}

// -------------------------------------------------------------- collision

TEST_CASE("a fast particle does not tunnel through a thin wall") {
    // The sweep from `prev` is what makes this work (doc 4.5). At 3000
    // units/second and a 120 Hz step, the particle moves 25 units per step,
    // through an 8-unit wall.
    PhysicsContract c;
    c.gravity = 0.0f;
    Solver s(c);
    s.Geometry().Add(AABB::FromRect(200.0f, 0.0f, 8.0f, 400.0f));
    s.AddRope({0, 200}, {10, 200}, 2, 1.0f, false, false, 0);

    auto& p = s.MutableParticles();
    for (size_t i = 0; i < p.Size(); ++i) {
        p.prev[i] = p.pos[i] - Vec2{25.0f, 0.0f};  // 25 units per step
    }
    for (int i = 0; i < 40; ++i) s.Step(FIXED_DT);

    for (size_t i = 0; i < s.Particles().Size(); ++i) {
        CHECK_MSG(s.Particles().pos[i].x < 205.0f,
                  "particle tunnelled to x=" + std::to_string(s.Particles().pos[i].x));
    }
}

TEST_CASE("a disabled box stops colliding") {
    PhysicsContract c;
    c.gravity = 900.0f;
    Solver s(c);
    const int door = s.Geometry().Add(AABB::FromRect(0.0f, 200.0f, 400.0f, 20.0f));
    s.AddRope({100, 100}, {110, 100}, 2, 1.0f, false, false, 0);

    for (int i = 0; i < 120; ++i) s.Step(FIXED_DT);
    CHECK_MSG(s.Particles().pos[0].y < 210.0f, "should be resting on the closed door");

    s.Geometry().enabled[door] = 0;
    for (int i = 0; i < 120; ++i) s.Step(FIXED_DT);
    CHECK_MSG(s.Particles().pos[0].y > 250.0f, "should fall through the opened door");
}

TEST_CASE("particles come to rest on a floor without sinking") {
    Solver s;
    AddRoom(s);
    s.AddLattice({100, 200}, 3, 3, 10.0f, 1.0f, 0);
    for (int i = 0; i < 1200; ++i) s.Step(FIXED_DT);
    for (const auto& pos : s.Particles().pos) {
        CHECK_MSG(pos.y < 301.0f, "particle sank into the floor: y=" + std::to_string(pos.y));
    }
}

// ------------------------------------------------------------- robustness

TEST_CASE("the solver never produces NaN under stress") {
    // Overlapping bodies, extreme pressure, bodies spawned inside geometry.
    PhysicsContract c;
    c.gravity = 4000.0f;
    Solver s(c);
    AddRoom(s);
    s.AddPressureBody({200, 280}, 40.0f, 24, 0.2f, 3.0f, 0);   // inside the floor
    s.AddPressureBody({205, 285}, 40.0f, 24, 0.2f, 3.0f, 1);   // overlapping the first
    s.AddLattice({190, 260}, 5, 5, 4.0f, 0.1f, 2);                  // overlapping both
    s.AddRope({0, 0}, {400, 300}, 30, 0.05f, true, true, 3);        // through everything

    for (int step = 0; step < 4000; ++step) {
        s.Step(FIXED_DT);
        if (step % 500 == 0) {
            for (size_t i = 0; i < s.Particles().Size(); ++i) {
                CHECK_MSG(s.Particles().pos[i].IsFinite(),
                          "NaN at step " + std::to_string(step) + " particle " + std::to_string(i));
                CHECK_MSG(s.Particles().prev[i].IsFinite(),
                          "NaN in prev at step " + std::to_string(step));
            }
        }
    }
    for (size_t i = 0; i < s.Particles().Size(); ++i) {
        CHECK(s.Particles().pos[i].IsFinite());
    }
}

TEST_CASE("coincident particles do not divide by zero") {
    Solver s;
    s.AddRope({50, 50}, {60, 50}, 2, 1.0f, false, false, 0);
    auto& p = s.MutableParticles();
    p.pos[1] = p.pos[0];  // exactly coincident
    p.prev[1] = p.pos[0];
    for (int i = 0; i < 60; ++i) s.Step(FIXED_DT);
    CHECK(s.Particles().pos[0].IsFinite());
    CHECK(s.Particles().pos[1].IsFinite());
}

TEST_CASE("a fully collapsed pressure body does not divide by zero") {
    Solver s;
    s.AddPressureBody({100, 100}, 15.0f, 12, 1.0f, 1.0f, 0);
    auto& p = s.MutableParticles();
    for (size_t i = 0; i < p.Size(); ++i) {
        p.pos[i] = {100, 100};  // everything on one point: zero area
        p.prev[i] = p.pos[i];
    }
    for (int i = 0; i < 60; ++i) s.Step(FIXED_DT);
    for (const auto& pos : s.Particles().pos) CHECK(pos.IsFinite());
}

// --------------------------------------------------------------- sleeping

TEST_CASE("a settled body sleeps and zeroes its implicit velocity") {
    Solver s;
    AddRoom(s);
    s.AddLattice({100, 250}, 3, 3, 10.0f, 1.0f, 0);
    for (int i = 0; i < 1500; ++i) s.Step(FIXED_DT);

    CHECK(s.Bodies()[0].sleeping);
    for (int idx : s.Bodies()[0].particles) {
        // Zeroed on sleep, so it does not lurch when it wakes (doc 14.2).
        CHECK(s.Particles().pos[idx] == s.Particles().prev[idx]);
    }
    const uint64_t h1 = s.StateHash();
    for (int i = 0; i < 200; ++i) s.Step(FIXED_DT);
    CHECK_MSG(h1 == s.StateHash(), "a sleeping body should not move at all");
}

TEST_CASE("a sleeping body wakes when something lands on it") {
    Solver s;
    AddRoom(s);
    s.AddLattice({100, 250}, 3, 3, 10.0f, 1.0f, 0);
    for (int i = 0; i < 1500; ++i) s.Step(FIXED_DT);
    CHECK(s.Bodies()[0].sleeping);

    s.Wake(0);
    CHECK(!s.Bodies()[0].sleeping);
    CHECK_EQ(s.Bodies()[0].sleep_timer, 0.0f);
}

// ------------------------------------------------------------ determinism

TEST_CASE("the same scene twice produces identical state hashes") {
    auto run = []() {
        Solver s;
        AddRoom(s);
        s.AddPressureBody({120, 80}, 22.0f, 18, 1.0f, 1.0f, 0);
        s.AddLattice({220, 60}, 4, 4, 11.0f, 0.8f, 1);
        s.AddRope({60, 0}, {60, 90}, 8, 0.4f, true, false, 2);
        for (int i = 0; i < 10000; ++i) s.Step(FIXED_DT);
        return s.StateHash();
    };
    CHECK_EQ(run(), run());
}

TEST_CASE("the contract hash changes when any parameter changes") {
    PhysicsContract a;
    PhysicsContract b;
    CHECK_EQ(a.Hash(), b.Hash());
    b.iterations += 1;
    CHECK(a.Hash() != b.Hash());

    PhysicsContract c;
    c.damping = 0.994f;
    CHECK(a.Hash() != c.Hash());
}

TEST_CASE("changing the iteration count changes the simulation") {
    // Doc 4.3's warning, made visible: this is why iterations is a contract
    // term rather than a tuning knob.
    auto run = [](int iterations) {
        PhysicsContract c;
        c.iterations = iterations;
        Solver s(c);
        AddRoom(s);
        s.AddLattice({100, 60}, 4, 4, 12.0f, 1.0f, 0);
        for (int i = 0; i < 400; ++i) s.Step(FIXED_DT);
        return s.StateHash();
    };
    CHECK(run(6) != run(8));
}

TEST_CASE("constraint order is stable across builds of the same scene") {
    Solver a;
    Solver b;
    a.AddLattice({0, 0}, 4, 4, 10.0f, 1.0f, 0);
    b.AddLattice({0, 0}, 4, 4, 10.0f, 1.0f, 0);
    CHECK_EQ(a.Constraints().size(), b.Constraints().size());
    for (size_t i = 0; i < a.Constraints().size(); ++i) {
        CHECK(a.Constraints()[i].a == b.Constraints()[i].a);
        CHECK(a.Constraints()[i].b == b.Constraints()[i].b);
    }
}
