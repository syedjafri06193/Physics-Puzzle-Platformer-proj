// Game-layer tests: input packing, player feel, levels, the validator.
//
// The solver tests (test_solver.cpp) assert that the physics is stable. These
// assert that the things built on top of it behave — and in particular that
// the forgiveness mechanisms in doc 7.1 are actually present, because every
// one of them is invisible when it works and only noticeable when it is
// missing.

#include <cmath>
#include <string>

#include "ppp/input.h"
#include "ppp/json.h"
#include "ppp/level.h"
#include "ppp/player.h"
#include "ppp/replay.h"
#include "ppp/script.h"
#include "ppp/sim.h"
#include "test.h"

using namespace ppp;

namespace {

/// A flat room with a floor at y = 320 and walls, for player tests.
Level FlatRoom() {
    Level level;
    level.id = "test-room";
    level.spawn = {100.0f, 300.0f};
    level.goal = AABB::FromRect(560.0f, 276.0f, 40.0f, 44.0f);
    level.camera_bounds = AABB::FromRect(0.0f, 0.0f, 640.0f, 360.0f);
    level.boxes.push_back({AABB::FromRect(0.0f, 0.0f, 16.0f, 360.0f), -1});
    level.boxes.push_back({AABB::FromRect(624.0f, 0.0f, 16.0f, 360.0f), -1});
    level.boxes.push_back({AABB::FromRect(0.0f, 320.0f, 640.0f, 40.0f), -1});
    return level;
}

InputState Move(float x) {
    InputState in;
    in.move_x = x;
    return in;
}

InputState Jump(float x = 0.0f) {
    InputState in;
    in.move_x = x;
    in.jump = true;
    return in;
}

/// Runs the player alone against a geometry, with no solver.
struct Harness {
    Level level;
    StaticGeometry geometry;
    Player player;
    PlayerTuning tuning;

    explicit Harness(const Level& l) : level(l) {
        for (const auto& b : level.boxes) geometry.Add(b.rect);
        player.pos = level.spawn;
        player.prev_pos = level.spawn;
    }

    void Step(const InputState& in, int times = 1) {
        for (int i = 0; i < times; ++i)
            StepPlayer(&player, tuning, in.Quantized(), FIXED_DT, geometry, nullptr);
    }

    /// Steps until the player is standing, so tests start from a known state.
    void Settle() {
        for (int i = 0; i < 60 && !player.on_floor; ++i) Step(InputState::Empty());
    }
};

}  // namespace

// ------------------------------------------------------------------ input

TEST_CASE("packed input round-trips through the replay format") {
    for (int q = -AXIS_STEPS; q <= AXIS_STEPS; ++q) {
        InputState in;
        in.move_x = static_cast<float>(q) / static_cast<float>(AXIS_STEPS);
        in.jump = (q % 3) == 0;
        in.grab = (q % 5) == 0;
        const InputState back = InputState::Unpack(in.Pack());
        CHECK_NEAR(back.move_x, in.move_x, 1e-6);
        CHECK_EQ(back.jump, in.jump);
        CHECK_EQ(back.grab, in.grab);
    }
}

TEST_CASE("quantizing is idempotent, so live play and playback agree") {
    // If quantizing twice differed from quantizing once, a live run would
    // consume one value and its replay another, and the two would drift
    // apart minutes later (doc 14.3).
    for (int i = -100; i <= 100; ++i) {
        InputState in;
        in.move_x = static_cast<float>(i) / 100.0f;
        const InputState once = in.Quantized();
        const InputState twice = once.Quantized();
        CHECK_EQ(once.Pack(), twice.Pack());
    }
}

TEST_CASE("the stick axis survives the extremes without wrapping") {
    InputState left;
    left.move_x = -1.0f;
    InputState right;
    right.move_x = 1.0f;
    CHECK_NEAR(InputState::Unpack(left.Pack()).move_x, -1.0, 1e-6);
    CHECK_NEAR(InputState::Unpack(right.Pack()).move_x, 1.0, 1e-6);

    // Past the extremes, because a stick corner reads over 1.0.
    InputState over;
    over.move_x = 1.9f;
    CHECK_NEAR(InputState::Unpack(over.Pack()).move_x, 1.0, 1e-6);
}

TEST_CASE("the deadzone is radial and rescales from its edge") {
    float x = 0.0f;
    float y = 0.0f;

    // Inside: dead.
    ApplyRadialDeadzone(0.1f, 0.1f, STICK_DEADZONE, &x, &y);
    CHECK_EQ(x, 0.0f);
    CHECK_EQ(y, 0.0f);

    // Just outside: near zero, not a jump to the deadzone magnitude. This is
    // the rescale, and it is the whole reason the stick feels continuous
    // (doc 7.2).
    ApplyRadialDeadzone(STICK_DEADZONE + 0.001f, 0.0f, STICK_DEADZONE, &x, &y);
    CHECK_MSG(std::fabs(x) < 0.01f,
              "output jumped to " + std::to_string(x) + " on crossing the deadzone");

    // Fully deflected: full output.
    ApplyRadialDeadzone(1.0f, 0.0f, STICK_DEADZONE, &x, &y);
    CHECK_NEAR(x, 1.0, 1e-5);

    // A diagonal past the deadzone on both axes but inside it radially must
    // read as zero — a per-axis deadzone would pass this through.
    const float d = STICK_DEADZONE * 0.7f;
    ApplyRadialDeadzone(d, d, STICK_DEADZONE, &x, &y);
    const float magnitude = std::sqrt(d * d + d * d);
    if (magnitude < STICK_DEADZONE) {
        CHECK_EQ(x, 0.0f);
        CHECK_EQ(y, 0.0f);
    }

    // Beyond the unit circle the magnitude is clamped, so a stick corner
    // cannot outrun the run speed.
    ApplyRadialDeadzone(1.0f, 1.0f, STICK_DEADZONE, &x, &y);
    CHECK_MSG(std::sqrt(x * x + y * y) <= 1.0001f, "deadzone let the stick exceed 1.0");
}

// ----------------------------------------------------------------- player

TEST_CASE("the player lands on the floor and stays on it") {
    Harness h(FlatRoom());
    h.Step(InputState::Empty(), 200);
    CHECK(h.player.on_floor);
    CHECK_NEAR(h.player.pos.y, 308.0, 0.01);
    CHECK_NEAR(h.player.vel.y, 0.0, 0.01);
}

TEST_CASE("a jump rises and returns to the ground") {
    Harness h(FlatRoom());
    h.Settle();
    const float ground = h.player.pos.y;

    float highest = ground;
    h.Step(Jump(), 36);  // hold for the full rise
    for (int i = 0; i < 200; ++i) {
        h.Step(InputState::Empty());
        highest = Min(highest, h.player.pos.y);
        if (h.player.on_floor && h.player.vel.y == 0.0f && i > 40) break;
    }
    const float height = ground - highest;
    CHECK_MSG(height > 40.0f && height < 70.0f,
              "jump height was " + std::to_string(height) + ", expected roughly 50");
    CHECK_MSG(h.player.on_floor, "the player never came back down");
}

TEST_CASE("releasing the jump early cuts the jump short") {
    // Variable jump height (doc 7.1).
    auto peak = [](int hold_ticks) {
        Harness h(FlatRoom());
        h.Settle();
        const float ground = h.player.pos.y;
        float highest = ground;
        for (int i = 0; i < 200; ++i) {
            h.Step(i < hold_ticks ? Jump() : InputState::Empty());
            highest = Min(highest, h.player.pos.y);
        }
        return ground - highest;
    };
    const float full = peak(40);
    const float tapped = peak(4);
    CHECK_MSG(tapped < full * 0.8f, "a tap reached " + std::to_string(tapped) +
                                        " against a full jump's " + std::to_string(full));
    CHECK_MSG(tapped > 8.0f, "a tap barely left the ground: " + std::to_string(tapped));
}

TEST_CASE("coyote time lets a jump work just after leaving a ledge") {
    // A floor that ends at x = 200, so the player runs off it.
    Level level = FlatRoom();
    level.boxes[2].rect = AABB::FromRect(0.0f, 320.0f, 200.0f, 40.0f);

    auto jump_after = [&](int delay_ticks) {
        Harness h(level);
        h.player.pos = {150.0f, 308.0f};
        h.player.prev_pos = h.player.pos;
        h.Settle();
        // Run off the edge.
        int airborne = 0;
        for (int i = 0; i < 200; ++i) {
            h.Step(Move(1.0f));
            if (!h.player.on_floor) {
                ++airborne;
                if (airborne >= delay_ticks) break;
            }
        }
        const float before = h.player.pos.y;
        h.Step(Jump(1.0f), 20);
        // A jump fired: the player is higher than when it was pressed.
        return h.player.pos.y < before - 5.0f;
    };

    CHECK_MSG(jump_after(2), "a jump 2 ticks after the ledge did not fire");
    // The window is 100 ms = 12 ticks. Well past it, there should be no jump.
    CHECK_MSG(!jump_after(40), "a jump 40 ticks after the ledge still fired");
}

TEST_CASE("a jump pressed just before landing still fires") {
    // Jump buffering (doc 7.1).
    Harness h(FlatRoom());
    h.player.pos = {100.0f, 200.0f};  // falling
    h.player.prev_pos = h.player.pos;

    // Fall until just above the floor, then press jump while still airborne.
    while (!h.player.on_floor && h.player.pos.y < 295.0f) h.Step(InputState::Empty());
    CHECK_MSG(!h.player.on_floor, "the test never got the player airborne near the floor");

    h.Step(Jump());  // pressed before touching down
    for (int i = 0; i < 6; ++i) h.Step(Jump());
    CHECK_MSG(h.player.vel.y < 0.0f,
              "the buffered jump never fired; vel.y = " + std::to_string(h.player.vel.y));
}

TEST_CASE("a jump consumes both timers, so one press cannot jump twice") {
    // Leaving the coyote timer set is the bug the design document calls out
    // by name (doc 7.1): a buffered press fires, the timer survives, and the
    // next tick it fires again as a double jump.
    Harness h(FlatRoom());
    h.Settle();
    h.Step(Jump());
    CHECK_EQ(h.player.jump_buffer_timer, 0.0f);
    CHECK_EQ(h.player.coyote_timer, 0.0f);

    const float after_first = h.player.vel.y;
    h.Step(Jump());
    CHECK_MSG(h.player.vel.y > after_first,
              "the second tick added more upward speed: a double jump");
}

TEST_CASE("falling is heavier than rising") {
    // The asymmetry is the design document's fast-fall gravity (doc 7.1).
    Harness h(FlatRoom());
    h.Settle();
    h.Step(Jump());
    const float rise_start = h.player.vel.y;
    h.Step(Jump());
    const float rise_accel = h.player.vel.y - rise_start;

    // Get well into the fall, past the apex-hang window.
    while (h.player.vel.y < 200.0f && !h.player.on_floor) h.Step(InputState::Empty());
    const float fall_start = h.player.vel.y;
    h.Step(InputState::Empty());
    const float fall_accel = h.player.vel.y - fall_start;

    CHECK_MSG(fall_accel > rise_accel * 1.4f,
              "fall gravity " + std::to_string(fall_accel) + " vs rise " +
                  std::to_string(rise_accel));
}

TEST_CASE("the player slides along a wall instead of stopping dead") {
    Level level = FlatRoom();
    level.boxes.push_back({AABB::FromRect(200.0f, 200.0f, 16.0f, 120.0f), -1});
    Harness h(level);
    h.Settle();

    // Run into the wall while falling: the horizontal axis stops, the
    // vertical one carries on. Axis-separated movement is what gives this.
    h.player.pos = {180.0f, 260.0f};
    h.player.prev_pos = h.player.pos;
    const float before_y = h.player.pos.y;
    h.Step(Move(1.0f), 20);
    CHECK_MSG(h.player.pos.y > before_y, "the player stopped falling when it hit a wall");
    CHECK_MSG(h.player.pos.x < 200.0f, "the player went through the wall");
}

TEST_CASE("a soft body cannot push the player through the floor") {
    // The player gets shoved into geometry by blobs all the time; what must
    // never happen is that it stays there and then sinks, because the swept
    // move reports no hit from inside a solid.
    Level level = FlatRoom();
    Sim sim(level);
    // Put the player inside the floor and step.
    Solver& solver = sim.MutableSolver();
    (void)solver;
    Sim fresh(level);
    for (int i = 0; i < 400; ++i) fresh.Step(Move(1.0f));
    CHECK_MSG(fresh.GetPlayer().pos.y < 330.0f,
              "the player sank to y=" + std::to_string(fresh.GetPlayer().pos.y));
    CHECK(fresh.GetPlayer().on_floor);
}

// ------------------------------------------------------------------- json

TEST_CASE("the json reader handles what a level file contains") {
    const std::string text = R"({
        // a comment, which is not JSON but is in every hand-edited file
        "id": "x", "n": -12.5, "e": 2e3, "t": true, "z": null,
        "a": [1, 2, 3,],
        "o": {"k": "v\n\"q\""},
    })";
    Json root;
    std::string error;
    CHECK_MSG(Json::Parse(text, &root, &error), "parse failed: " + error);
    CHECK_EQ(root["id"].AsString(), std::string("x"));
    CHECK_NEAR(root["n"].AsNumber(), -12.5, 1e-9);
    CHECK_NEAR(root["e"].AsNumber(), 2000.0, 1e-9);
    CHECK_EQ(root["t"].AsBool(), true);
    CHECK(root["z"].IsNull());
    CHECK_EQ(root["a"].Size(), static_cast<size_t>(3));
    CHECK_EQ(root["a"][1].AsInt(), 2);
    CHECK_EQ(root["o"]["k"].AsString(), std::string("v\n\"q\""));
}

TEST_CASE("a missing json key yields the fallback rather than crashing") {
    Json root;
    std::string error;
    CHECK(Json::Parse("{\"a\": 1}", &root, &error));
    CHECK_EQ(root["nope"].AsInt(7), 7);
    CHECK_EQ(root["nope"]["deeper"].AsInt(7), 7);
    CHECK_EQ(root["nope"][3].AsInt(7), 7);
    CHECK_EQ(root["a"].AsString("fallback"), std::string("fallback"));
}

TEST_CASE("a malformed level file reports where it broke") {
    Json root;
    std::string error;
    CHECK(!Json::Parse("{\"a\": }", &root, &error));
    CHECK_MSG(error.find("byte") != std::string::npos,
              "the error should name a byte offset, got: " + error);
}

// ------------------------------------------------------------------ level

TEST_CASE("a level round-trips through the writer and back") {
    Level original = FlatRoom();
    original.bodies.push_back(LevelBody{});
    original.bodies[0].id = 4;
    original.bodies[0].position = {123.5f, 67.25f};
    original.plates.push_back(LevelPlate{1, AABB::FromRect(10, 20, 30, 40), 5.5f, false, {2}});
    original.doors.push_back(LevelDoor{2, {1}, false});
    original.hazards.push_back(AABB::FromRect(1, 2, 3, 4));
    original.checkpoints.push_back({{11.0f, 22.0f}, 33.0f});

    Level back;
    std::string error;
    CHECK_MSG(LoadLevel(WriteLevel(original), &back, &error), "reload failed: " + error);

    CHECK_EQ(back.id, original.id);
    CHECK_EQ(back.boxes.size(), original.boxes.size());
    CHECK_EQ(back.bodies[0].id, 4);
    // Exact, not approximate. A level that shifts by one float bit on a
    // save/load cycle invalidates every replay recorded against it.
    CHECK_EQ(back.bodies[0].position.x, original.bodies[0].position.x);
    CHECK_EQ(back.bodies[0].position.y, original.bodies[0].position.y);
    CHECK_EQ(back.plates[0].threshold, 5.5f);
    CHECK_EQ(back.plates[0].player_counts, false);
    CHECK_EQ(back.doors[0].latching, false);
    CHECK_EQ(back.checkpoints[0].radius, 33.0f);
}

TEST_CASE("a plate that names its door and a door that names its plate agree") {
    const std::string a = R"({"id":"a","boxes":[],
        "plates":[{"id":1,"region":[0,0,10,10],"opens":[2]}],
        "doors":[{"id":2}]})";
    Level level;
    std::string error;
    CHECK(LoadLevel(a, &level, &error));
    CHECK_EQ(level.doors[0].plates.size(), static_cast<size_t>(1));
    CHECK_EQ(level.doors[0].plates[0], 1);
}

// -------------------------------------------------------------- validator

TEST_CASE("the validator catches a wall too thin to collide reliably") {
    Level level = FlatRoom();
    level.boxes.push_back({AABB::FromRect(300.0f, 200.0f, 2.0f, 120.0f), -1});
    const ValidationReport report = ValidateLevel(level, DefaultContract());
    bool found = false;
    for (const auto& i : report.issues)
        if (i.check == "wall-thickness") found = true;
    CHECK_MSG(found, "a 2-unit wall passed the thickness check");
    CHECK(!report.Ok());
}

TEST_CASE("the validator catches a spawn inside geometry") {
    Level level = FlatRoom();
    level.spawn = {100.0f, 340.0f};  // inside the floor
    const ValidationReport report = ValidateLevel(level, DefaultContract());
    bool found = false;
    for (const auto& i : report.issues)
        if (i.check == "spawn-inside-geometry") found = true;
    CHECK_MSG(found, "a spawn buried in the floor passed");
}

TEST_CASE("the validator catches a goal walled off from the spawn") {
    Level level = FlatRoom();
    // A full-height wall between the spawn and the goal, with no door.
    level.boxes.push_back({AABB::FromRect(300.0f, 0.0f, 16.0f, 320.0f), -1});
    const ValidationReport report = ValidateLevel(level, DefaultContract());
    bool found = false;
    for (const auto& i : report.issues)
        if (i.check == "goal-unreachable") found = true;
    CHECK_MSG(found, "a sealed-off goal passed the flood fill");
}

TEST_CASE("a goal behind a door is still considered reachable") {
    // Doors are open for the flood fill. Whether the door *can* be opened is
    // a question only a recorded solution answers (doc 3.3), and failing it
    // here would reject every real puzzle in the game.
    Level level = FlatRoom();
    level.boxes.push_back({AABB::FromRect(300.0f, 0.0f, 16.0f, 320.0f), 0});
    level.plates.push_back(LevelPlate{0, AABB::FromRect(100, 300, 40, 20), 1.0f, true, {0}});
    level.doors.push_back(LevelDoor{0, {0}, true});
    const ValidationReport report = ValidateLevel(level, DefaultContract());
    for (const auto& i : report.issues)
        CHECK_MSG(i.check != "goal-unreachable", "a door was treated as a wall");
}

TEST_CASE("the validator catches a body starting inside a wall") {
    Level level = FlatRoom();
    LevelBody body;
    body.type = BodyType::Pressure;
    body.position = {100.0f, 330.0f};  // buried in the floor
    body.radius = 20.0f;
    level.bodies.push_back(body);
    const ValidationReport report = ValidateLevel(level, DefaultContract());
    bool found = false;
    for (const auto& i : report.issues)
        if (i.check == "body-overlaps-geometry") found = true;
    CHECK_MSG(found, "a blob inside the floor passed; it explodes on the first step");
}

TEST_CASE("the validator catches a particle budget overrun") {
    Level level = FlatRoom();
    level.particle_budget = 10;
    LevelBody body;
    body.type = BodyType::Lattice;
    body.position = {100.0f, 100.0f};
    body.width = 10;
    body.height = 10;
    level.bodies.push_back(body);
    const ValidationReport report = ValidateLevel(level, DefaultContract());
    bool found = false;
    for (const auto& i : report.issues)
        if (i.check == "particle-budget") found = true;
    CHECK_MSG(found, "100 particles passed a budget of 10");
}

TEST_CASE("the validator catches a door nothing can open and duplicate ids") {
    Level level = FlatRoom();
    level.doors.push_back(LevelDoor{0, {}, true});
    level.boxes.push_back({AABB::FromRect(300.0f, 200.0f, 16.0f, 120.0f), 0});
    LevelBody body;
    body.id = 3;
    level.bodies.push_back(body);
    level.bodies.push_back(body);  // same id

    const ValidationReport report = ValidateLevel(level, DefaultContract());
    bool unopenable = false;
    bool duplicate = false;
    for (const auto& i : report.issues) {
        if (i.check == "unopenable-door") unopenable = true;
        if (i.check == "duplicate-id") duplicate = true;
    }
    CHECK(unopenable);
    CHECK(duplicate);
}

TEST_CASE("a clean level produces no errors") {
    const Level level = FlatRoom();
    const ValidationReport report = ValidateLevel(level, DefaultContract());
    std::string all;
    for (const auto& i : report.issues)
        if (i.severity == ValidationIssue::Severity::Error) all += "[" + i.check + "] " + i.message + "; ";
    CHECK_MSG(report.Ok(), "a clean room reported: " + all);
}

// -------------------------------------------------------------------- sim

TEST_CASE("a plate presses under enough mass and opens its door") {
    Level level = FlatRoom();
    LevelBody body;
    body.type = BodyType::Pressure;
    body.id = 0;
    body.position = {300.0f, 288.0f};
    body.radius = 21.0f;
    body.count = 18;
    level.bodies.push_back(body);
    level.plates.push_back(
        LevelPlate{0, AABB::FromRect(270.0f, 296.0f, 60.0f, 24.0f), 4.0f, false, {0}});
    level.doors.push_back(LevelDoor{0, {0}, false});
    level.boxes.push_back({AABB::FromRect(450.0f, 200.0f, 16.0f, 120.0f), 0});

    Sim sim(level);
    CHECK_MSG(!sim.Doors()[0].open, "the door started open");
    for (int i = 0; i < 200; ++i) sim.Step(InputState::Empty());
    CHECK_MSG(sim.Plates()[0].pressed,
              "a blob sitting on the plate did not press it; mass was " +
                  std::to_string(sim.Plates()[0].mass));
    CHECK(sim.Doors()[0].open);
}

TEST_CASE("a non-latching door closes again when the weight leaves") {
    Level level = FlatRoom();
    level.plates.push_back(
        LevelPlate{0, AABB::FromRect(90.0f, 296.0f, 60.0f, 24.0f), 1.0f, true, {0}});
    level.doors.push_back(LevelDoor{0, {0}, false});
    level.boxes.push_back({AABB::FromRect(450.0f, 200.0f, 16.0f, 120.0f), 0});

    Sim sim(level);
    for (int i = 0; i < 60; ++i) sim.Step(InputState::Empty());
    CHECK_MSG(sim.Doors()[0].open, "the player standing on the plate did not open the door");
    for (int i = 0; i < 200; ++i) sim.Step(Move(1.0f));
    CHECK_MSG(!sim.Doors()[0].open, "the door stayed open after the player walked off");
}

TEST_CASE("a latching door stays open once opened") {
    Level level = FlatRoom();
    level.plates.push_back(
        LevelPlate{0, AABB::FromRect(90.0f, 296.0f, 60.0f, 24.0f), 1.0f, true, {0}});
    level.doors.push_back(LevelDoor{0, {0}, true});
    level.boxes.push_back({AABB::FromRect(450.0f, 200.0f, 16.0f, 120.0f), 0});

    Sim sim(level);
    for (int i = 0; i < 60; ++i) sim.Step(InputState::Empty());
    CHECK(sim.Doors()[0].open);
    for (int i = 0; i < 200; ++i) sim.Step(Move(1.0f));
    CHECK_MSG(sim.Doors()[0].open, "a latching door closed again");
}

TEST_CASE("a hazard kills and respawns at the last checkpoint") {
    Level level = FlatRoom();
    level.checkpoints.push_back({{200.0f, 308.0f}, 30.0f});
    level.hazards.push_back(AABB::FromRect(300.0f, 300.0f, 40.0f, 20.0f));

    Sim sim(level);
    bool died = false;
    for (int i = 0; i < 400; ++i) {
        if (sim.Step(Move(1.0f)) == SimOutcome::Died) {
            died = true;
            break;
        }
    }
    CHECK_MSG(died, "the player walked through a hazard unharmed");
    CHECK_EQ(sim.Deaths(), 1);
    CHECK_MSG(sim.CheckpointIndex() == 0, "the checkpoint was not taken on the way past");
    CHECK_NEAR(sim.GetPlayer().pos.x, 200.0, 0.01);
}

TEST_CASE("falling out of the world counts as a death rather than a softlock") {
    Level level;
    level.id = "void";
    level.spawn = {100.0f, 100.0f};
    level.goal = AABB::FromRect(560.0f, 276.0f, 40.0f, 44.0f);
    level.camera_bounds = AABB::FromRect(0.0f, 0.0f, 640.0f, 360.0f);
    // No floor at all.
    Sim sim(level);
    bool died = false;
    for (int i = 0; i < 600; ++i)
        if (sim.Step(InputState::Empty()) == SimOutcome::Died) died = true;
    CHECK_MSG(died, "a player falling forever never died");
}

TEST_CASE("a restart is bit-identical to a fresh load") {
    // A restart that leaves anything behind is a replay that diverges a
    // minute in, and it is very hard to find afterwards.
    Level level = FlatRoom();
    LevelBody body;
    body.type = BodyType::Pressure;
    body.position = {300.0f, 288.0f};
    level.bodies.push_back(body);

    Sim a(level);
    for (int i = 0; i < 120; ++i) a.Step(Move(1.0f));
    a.Reset();

    Sim b(level);
    CHECK_EQ(a.StateHash(), b.StateHash());

    for (int i = 0; i < 120; ++i) {
        a.Step(Move(-1.0f));
        b.Step(Move(-1.0f));
    }
    CHECK_EQ(a.StateHash(), b.StateHash());
}

TEST_CASE("the fixed-timestep accumulator clamps a suspended frame") {
    StepClock clock;
    CHECK_EQ(clock.Advance(FIXED_DT), 1);
    CHECK_EQ(clock.Advance(0.0f), 0);
    // A process suspended for ten seconds must not run 1200 steps on resume.
    CHECK_MSG(clock.Advance(10.0f) <= MAX_STEPS_PER_FRAME,
              "a huge frame delta ran more than the step cap");
    // And the leftover is dropped, not carried: carrying it is the spiral of
    // death (doc 4.2).
    CHECK_MSG(clock.Advance(0.0f) == 0, "the clock carried a backlog into the next frame");
}

TEST_CASE("render interpolation stays between the last two states") {
    Level level = FlatRoom();
    Sim sim(level);
    for (int i = 0; i < 60; ++i) sim.Step(Move(1.0f));
    const Vec2 prev = sim.GetPlayer().prev_pos;
    const Vec2 now = sim.GetPlayer().pos;
    CHECK_EQ(sim.InterpolatedPlayerPosition(0.0f).x, prev.x);
    CHECK_EQ(sim.InterpolatedPlayerPosition(1.0f).x, now.x);
    const float mid = sim.InterpolatedPlayerPosition(0.5f).x;
    CHECK_MSG(mid >= Min(prev.x, now.x) && mid <= Max(prev.x, now.x),
              "interpolation left the interval");
    // Out-of-range alpha is clamped, not extrapolated.
    CHECK_EQ(sim.InterpolatedPlayerPosition(4.0f).x, now.x);
}

// ----------------------------------------------------------------- replay

TEST_CASE("a replay round-trips through its binary encoding") {
    Replay r;
    r.level_id = "a-level";
    r.note = "walk right, then jump";
    r.contract_hash = 0xDEADBEEFCAFEF00Dull;
    r.seed = 4242;
    r.expected_tick = 1234;
    r.expected_state_hash = 0x0123456789ABCDEFull;
    r.expected_deaths = 2;
    for (int i = 0; i < 500; ++i) {
        InputState in;
        in.move_x = static_cast<float>((i % 65) - 32) / 32.0f;
        in.jump = (i % 7) == 0;
        r.frames.push_back(in.Pack());
    }

    Replay back;
    std::string error;
    CHECK_MSG(DecodeReplay(EncodeReplay(r), &back, &error), "decode failed: " + error);
    CHECK_EQ(back.level_id, r.level_id);
    CHECK_EQ(back.note, r.note);
    CHECK_EQ(back.contract_hash, r.contract_hash);
    CHECK_EQ(back.expected_tick, r.expected_tick);
    CHECK_EQ(back.expected_state_hash, r.expected_state_hash);
    CHECK_EQ(back.expected_deaths, r.expected_deaths);
    CHECK_EQ(back.frames.size(), r.frames.size());
    for (size_t i = 0; i < r.frames.size(); ++i) CHECK_EQ(back.frames[i], r.frames[i]);
}

TEST_CASE("a truncated replay is rejected rather than half-read") {
    Replay r;
    r.level_id = "x";
    r.frames.assign(100, 0);
    const std::string bytes = EncodeReplay(r);

    Replay back;
    std::string error;
    CHECK(!DecodeReplay(bytes.substr(0, bytes.size() - 20), &back, &error));
    CHECK_MSG(error.find("truncated") != std::string::npos, "unhelpful error: " + error);
    CHECK(!DecodeReplay("not a replay at all", &back, &error));
}

TEST_CASE("a script compiles only if it actually completes the level") {
    Level level = FlatRoom();
    level.goal = AABB::FromRect(560.0f, 276.0f, 40.0f, 44.0f);

    Script good;
    std::string error;
    CHECK_MSG(ParseScript("# run right\n5.0 1.0\n", &good, &error), error);
    CHECK_EQ(good.note, std::string("run right"));

    Replay replay;
    const CompileResult ok = CompileScript(level, good, &replay);
    CHECK_MSG(ok.completed, "running right across an empty room did not reach the goal");
    CHECK_MSG(replay.Ticks() > 0, "the replay recorded no input");
    CHECK_EQ(replay.note, good.note);

    Script bad;
    CHECK(ParseScript("1.0 -1.0\n", &bad, &error));
    Replay unused;
    const CompileResult fail = CompileScript(level, bad, &unused);
    CHECK_MSG(!fail.completed, "walking away from the goal was accepted as a solution");
}

TEST_CASE("a malformed script says which line is wrong") {
    Script script;
    std::string error;
    CHECK(!ParseScript("1.0 0.0 fly\n", &script, &error));
    CHECK_MSG(error.find("line 1") != std::string::npos, "unhelpful error: " + error);
    CHECK(!ParseScript("0.5 4.0\n", &script, &error));  // axis out of range
    CHECK(!ParseScript("notanumber 0\n", &script, &error));
}

TEST_CASE("script segment lengths do not accumulate rounding error") {
    // Each segment's tick count is computed from its own duration. Summing a
    // running time instead lets one rounding error shift every later
    // segment, which for a jump timed to a ledge is a solution or a death.
    Script script;
    std::string error;
    std::string text;
    for (int i = 0; i < 100; ++i) text += "0.1 0.0\n";
    CHECK(ParseScript(text, &script, &error));
    CHECK_EQ(script.Expand().size(), static_cast<size_t>(100 * 12));
}
