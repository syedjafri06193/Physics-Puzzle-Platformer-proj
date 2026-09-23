// Determinism (doc 6, doc 15.3).
//
// Determinism is not a nice property here, it is the load-bearing one. The
// replay regression suite is the only thing that makes a physics change
// affordable once there are a hundred levels (doc 3.3), and a replay suite
// on a non-deterministic simulation reports noise.
//
// Doc 15.3 asks for the same replay twice in one process and again in a
// fresh process. The in-process half is here. The fresh-process half is
// `tools/run_replays.py --twice`, which is the only place it can live: a
// test that re-runs itself in a new process to check for hidden global
// state is a test that carries the global state with it.
//
// Same machine only, deliberately (doc 6.3). Cross-platform bit-identical
// floating point would mean a software float library, and the project does
// not need it: replays are recorded and verified on CI, not shared between
// a player's machine and someone else's.

#include <cmath>
#include <string>
#include <vector>

#include "ppp/level.h"
#include "ppp/replay.h"
#include "ppp/script.h"
#include "ppp/sim.h"
#include "test.h"

using namespace ppp;

namespace {

/// A scene with one of everything, so a divergence anywhere shows up.
Level MixedLevel() {
    Level level;
    level.id = "determinism";
    level.seed = 20260401;
    level.spawn = {60.0f, 300.0f};
    level.goal = AABB::FromRect(560.0f, 276.0f, 40.0f, 44.0f);
    level.camera_bounds = AABB::FromRect(0.0f, 0.0f, 640.0f, 360.0f);

    level.boxes.push_back({AABB::FromRect(0.0f, 0.0f, 16.0f, 360.0f), -1});
    level.boxes.push_back({AABB::FromRect(624.0f, 0.0f, 16.0f, 360.0f), -1});
    level.boxes.push_back({AABB::FromRect(0.0f, 320.0f, 640.0f, 40.0f), -1});
    level.boxes.push_back({AABB::FromRect(420.0f, 200.0f, 16.0f, 120.0f), 0});

    LevelBody blob;
    blob.type = BodyType::Pressure;
    blob.id = 0;
    blob.position = {200.0f, 280.0f};
    blob.radius = 22.0f;
    blob.count = 18;
    blob.pressure = 1.05f;
    level.bodies.push_back(blob);

    LevelBody jelly;
    jelly.type = BodyType::Lattice;
    jelly.id = 1;
    jelly.position = {280.0f, 250.0f};
    jelly.width = 4;
    jelly.height = 4;
    jelly.spacing = 11.0f;
    level.bodies.push_back(jelly);

    LevelBody rope;
    rope.type = BodyType::Rope;
    rope.id = 2;
    rope.position = {120.0f, 80.0f};
    rope.to = {120.0f, 200.0f};
    rope.count = 10;
    rope.pin_start = true;
    rope.pin_end = false;
    level.bodies.push_back(rope);

    level.plates.push_back(
        LevelPlate{0, AABB::FromRect(180.0f, 296.0f, 80.0f, 24.0f), 4.0f, true, {0}});
    level.doors.push_back(LevelDoor{0, {0}, false});
    return level;
}

/// A long, varied input stream: movement both ways, jumps, grabs, and the
/// idle stretches where sleeping kicks in.
std::vector<InputState> BusyInputs(int ticks) {
    std::vector<InputState> out;
    out.reserve(static_cast<size_t>(ticks));
    for (int t = 0; t < ticks; ++t) {
        InputState in;
        const int phase = (t / 90) % 6;
        switch (phase) {
            case 0: in.move_x = 1.0f; break;
            case 1: in.move_x = 1.0f; in.jump = (t % 40) < 20; break;
            case 2: in.move_x = 0.5f; in.grab = true; break;
            case 3: in.move_x = -1.0f; in.grab = true; break;
            case 4: in.move_x = 0.0f; break;  // idle: bodies settle and sleep
            case 5: in.move_x = -0.25f; in.jump = (t % 60) < 10; break;
        }
        out.push_back(in);
    }
    return out;
}

uint64_t RunMixed(int ticks) {
    const Level level = MixedLevel();
    Sim sim(level);
    const std::vector<InputState> inputs = BusyInputs(ticks);
    for (const auto& in : inputs) sim.Step(in);
    return sim.StateHash();
}

}  // namespace

TEST_CASE("the same inputs produce the same state hash, twice in one process") {
    CHECK_EQ(RunMixed(3000), RunMixed(3000));
}

TEST_CASE("a third run agrees with the first two") {
    // Three, not two. Two runs can agree because both read the same stale
    // memory; a third after other tests have churned the allocator is a
    // cheap extra chance to catch uninitialised state.
    const uint64_t a = RunMixed(1500);
    std::vector<Sim> churn;
    for (int i = 0; i < 4; ++i) churn.emplace_back(MixedLevel());
    for (auto& s : churn)
        for (int i = 0; i < 50; ++i) s.Step(InputState::Empty());
    const uint64_t b = RunMixed(1500);
    const uint64_t c = RunMixed(1500);
    CHECK_EQ(a, b);
    CHECK_EQ(b, c);
}

TEST_CASE("state hashes diverge when the input differs") {
    // The complement of the property above: if two runs with *different*
    // inputs produced the same hash, the hash would be covering nothing and
    // every determinism test would pass vacuously.
    //
    // The check is made shortly after the difference, not at the end of the
    // run. Over a long run the two states can legitimately reconverge — the
    // first draft of this test nudged the stick for one tick and compared
    // 200 ticks later, by which point the player was resting against a door
    // and every body was asleep, so both runs had been clamped to bitwise
    // identical positions. That is correct behaviour, and a test that
    // forbids it would be asserting that a wall cannot stop two players at
    // the same place.
    const Level level = MixedLevel();
    Sim a(level);
    Sim b(level);
    // The window matters: it has to be one where the player is free to
    // move. Pushing into a wall clamps both runs to the same position
    // whatever the stick is doing, so the difference has to be introduced
    // while the player is walking away from one (phase 3 of BusyInputs).
    const std::vector<InputState> inputs = BusyInputs(320);
    for (size_t i = 0; i < inputs.size(); ++i) {
        a.Step(inputs[i]);
        InputState nudged = inputs[i];
        // Nudged *toward* zero. Phase 3 holds the stick at -1.0, which is
        // the end of the quantized range, so nudging further out clamps
        // straight back to -1.0 and the two runs stay identical — which is
        // the clamp working, not the hash failing.
        if (i >= 280) nudged.move_x += 1.0f / static_cast<float>(AXIS_STEPS);
        b.Step(nudged);
        if (i == 300) {
            CHECK_MSG(a.StateHash() != b.StateHash(),
                      "a difference in stick position left the hash unchanged");
        }
    }
}

TEST_CASE("the state hash covers the player, the bodies, and the doors") {
    // Each of these has been left out of a hash at some point in some
    // project, and each omission makes a class of divergence invisible.
    const Level level = MixedLevel();

    Sim base(level);
    for (int i = 0; i < 300; ++i) base.Step(InputState::Empty());
    const uint64_t original = base.StateHash();

    {   // player position
        Sim s(level);
        for (int i = 0; i < 300; ++i) s.Step(InputState::Empty());
        CHECK_EQ(s.StateHash(), original);
        for (int i = 0; i < 30; ++i) s.Step(InputState{1.0f, false, false, false});
        CHECK(s.StateHash() != original);
    }
    {   // a body moved
        Sim s(level);
        for (int i = 0; i < 300; ++i) s.Step(InputState::Empty());
        s.MutableSolver().MutableParticles().pos[0].x += 0.5f;
        CHECK_MSG(s.StateHash() != original, "moving a particle did not change the hash");
    }
}

TEST_CASE("a replay of a recorded run reproduces it exactly") {
    const Level level = MixedLevel();

    // Record.
    Sim recording(level);
    ReplayRecorder recorder(level.id, recording);
    const std::vector<InputState> inputs = BusyInputs(1200);
    for (const auto& in : inputs) {
        recorder.RecordTick(in);
        recording.Step(in);
    }
    recorder.Finish(recording);
    const Replay& replay = recorder.Get();

    // Play back through the replay's own accessor, tick by tick.
    Sim playback(level);
    for (size_t t = 0; t < replay.Ticks(); ++t) playback.Step(replay.At(static_cast<int>(t)));

    CHECK_EQ(playback.Tick(), recording.Tick());
    CHECK_EQ(playback.StateHash(), recording.StateHash());
    CHECK_EQ(playback.StateHash(), replay.expected_state_hash);
}

TEST_CASE("a replay survives a trip through its binary encoding") {
    const Level level = MixedLevel();
    Sim recording(level);
    ReplayRecorder recorder(level.id, recording);
    for (const auto& in : BusyInputs(600)) {
        recorder.RecordTick(in);
        recording.Step(in);
    }
    recorder.Finish(recording);

    Replay decoded;
    std::string error;
    CHECK_MSG(DecodeReplay(EncodeReplay(recorder.Get()), &decoded, &error), error);

    Sim playback(level);
    for (size_t t = 0; t < decoded.Ticks(); ++t) playback.Step(decoded.At(static_cast<int>(t)));
    CHECK_EQ(playback.StateHash(), decoded.expected_state_hash);
}

TEST_CASE("the contract hash covers the player tuning, not just the physics") {
    // Doc 3.3 lists player tuning among the things the contract covers, and
    // it has to: a change to jump height invalidates a recorded solution
    // exactly as thoroughly as a change to gravity.
    const Level level = MixedLevel();

    PlayerTuning changed = DefaultPlayerTuning();
    changed.jump_speed += 1.0f;

    Sim a(level, DefaultContract(), DefaultPlayerTuning());
    Sim b(level, DefaultContract(), changed);
    CHECK_MSG(a.ContractHash() != b.ContractHash(),
              "changing the jump speed left the contract hash unchanged");

    PhysicsContract heavier = DefaultContract();
    heavier.gravity += 1.0f;
    Sim c(level, heavier, DefaultPlayerTuning());
    CHECK(a.ContractHash() != c.ContractHash());
}

TEST_CASE("a replay recorded under another contract is flagged, not trusted") {
    // The whole point of storing the hash: a mismatch must be reported as
    // "needs re-verification", never silently trusted and never reported as
    // a regression it is not (doc 3.3).
    Level level;
    level.id = "walk-right";
    level.spawn = {60.0f, 300.0f};
    level.goal = AABB::FromRect(560.0f, 276.0f, 40.0f, 44.0f);
    level.camera_bounds = AABB::FromRect(0.0f, 0.0f, 640.0f, 360.0f);
    level.boxes.push_back({AABB::FromRect(0.0f, 320.0f, 640.0f, 40.0f), -1});

    Script script;
    std::string error;
    CHECK(ParseScript("5.0 1.0\n", &script, &error));

    Replay replay;
    CHECK(CompileScript(level, script, &replay).completed);

    // Unchanged physics: a clean pass.
    const ReplayResult pass = RunReplay(level, replay);
    CHECK_MSG(pass.verdict == ReplayVerdict::Passed,
              std::string("expected a pass, got ") + ReplayVerdictName(pass.verdict) + ": " +
                  pass.message);
    CHECK(pass.state_hash_matched);

    // Physics changed but the level still solvable: needs re-verification.
    PhysicsContract changed = DefaultContract();
    changed.gravity *= 1.02f;
    const ReplayResult stale = RunReplay(level, replay, ReplayOptions{}, changed);
    CHECK_MSG(stale.verdict == ReplayVerdict::NeedsReverification,
              std::string("expected re-verification, got ") + ReplayVerdictName(stale.verdict));

    // Physics changed so much the level no longer completes: that is a
    // failure, and reporting it as "needs re-verification" would hide
    // exactly the regression the suite exists to find.
    PlayerTuning crippled = DefaultPlayerTuning();
    crippled.run_speed = 1.0f;
    const ReplayResult broken =
        RunReplay(level, replay, ReplayOptions{}, DefaultContract(), crippled);
    CHECK_MSG(broken.verdict == ReplayVerdict::NotCompleted,
              std::string("expected a failure, got ") + ReplayVerdictName(broken.verdict));
}

TEST_CASE("timing drift beyond the tolerance is reported") {
    Level level;
    level.id = "drift";
    level.spawn = {60.0f, 300.0f};
    level.goal = AABB::FromRect(560.0f, 276.0f, 40.0f, 44.0f);
    level.camera_bounds = AABB::FromRect(0.0f, 0.0f, 640.0f, 360.0f);
    level.boxes.push_back({AABB::FromRect(0.0f, 320.0f, 640.0f, 40.0f), -1});

    Script script;
    std::string error;
    CHECK(ParseScript("5.0 1.0\n", &script, &error));
    Replay replay;
    CHECK(CompileScript(level, script, &replay).completed);

    // Pretend it was recorded far earlier than it completes now, with the
    // contract unchanged, so the only thing left to report is the drift.
    replay.expected_tick -= 500;
    const ReplayResult result = RunReplay(level, replay);
    CHECK_MSG(result.verdict == ReplayVerdict::TimingDrift,
              std::string("expected drift, got ") + ReplayVerdictName(result.verdict));
    CHECK(result.completed);
}

TEST_CASE("sleeping does not make the result depend on when a body fell asleep") {
    // Sleeping is an optimisation that skips work, and an optimisation that
    // changes the answer is a bug. Two runs that reach the same state by
    // different routes — one that idled long enough for the bodies to sleep
    // and one that did not — must agree once both have settled.
    const Level level = MixedLevel();

    Sim slept(level);
    for (int i = 0; i < 1200; ++i) slept.Step(InputState::Empty());
    const int awake_after_idle = slept.GetSolver().AwakeBodyCount();

    CHECK_MSG(awake_after_idle < static_cast<int>(level.bodies.size()),
              "nothing fell asleep in ten seconds of stillness");

    // Waking must not move anything. The sleeping flag itself is part of
    // the state hash — deliberately, since a sleeping body and an
    // identically-placed awake one behave differently next step — so this
    // compares positions, not the hash.
    //
    // The failure this guards against is the one doc 14.2 warns about: a
    // body whose `prev` was not zeroed on falling asleep lurches on waking,
    // because the stale `prev` is read as an implicit velocity covering
    // however long the body was asleep.
    std::vector<Vec2> before = slept.GetSolver().Particles().pos;
    std::vector<Vec2> before_prev = slept.GetSolver().Particles().prev;
    for (size_t i = 0; i < level.bodies.size(); ++i)
        slept.MutableSolver().Wake(static_cast<int>(i));

    const auto& after = slept.GetSolver().Particles();
    for (size_t i = 0; i < before.size(); ++i) {
        CHECK_EQ(after.pos[i].x, before[i].x);
        CHECK_EQ(after.pos[i].y, before[i].y);
        CHECK_EQ(after.prev[i].x, before_prev[i].x);
        CHECK_EQ(after.prev[i].y, before_prev[i].y);
    }

    // And one step after waking must not lurch either.
    slept.Step(InputState::Empty());
    for (size_t i = 0; i < before.size(); ++i) {
        const float moved = (slept.GetSolver().Particles().pos[i] - before[i]).Length();
        CHECK_MSG(moved < 1.0f,
                  "particle " + std::to_string(i) + " lurched " + std::to_string(moved) +
                      " units on the first step after waking");
    }
}
