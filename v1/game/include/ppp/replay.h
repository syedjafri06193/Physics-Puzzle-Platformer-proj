// Replays: the regression suite's input format (doc 3.3, 14.3, 15.2).
//
// Doc 3.3 calls the physics contract plus recorded solutions "the piece of
// infrastructure that makes the project survivable", and doc 15.2 calls the
// replay suite the most valuable test in the project. The reason is narrow
// and specific: at ~120 prototyped levels, a physics change you cannot
// verify against every level is a physics change you cannot make. The
// replay suite converts "did I break anything?" from a week of replaying
// levels by hand into a CI run.
//
// Inputs are recorded, never state (doc 14.3). State would be megabytes and
// would tell you nothing: replaying stored positions under new physics just
// reproduces the old positions. Replaying stored *inputs* asks the question
// that matters — is this level still solvable?

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ppp/input.h"
#include "ppp/sim.h"

namespace ppp {

inline constexpr uint32_t REPLAY_MAGIC = 0x50505231u;  // "PPR1"
inline constexpr uint32_t REPLAY_FORMAT = 2;

struct Replay {
    std::string level_id;
    /// Copied from `Sim::ContractHash()` at record time. This is the whole
    /// point of the format: when it does not match the running build, the
    /// replay is reported as "needs re-verification", never silently
    /// trusted and never silently failed (doc 3.3).
    uint64_t contract_hash = 0;
    int physics_version = PHYSICS_VERSION;
    int input_version = INPUT_FORMAT_VERSION;
    uint32_t seed = 0;

    /// Tick the recording completed on. The suite compares against this with
    /// a tolerance: some drift is acceptable, a lot means the physics
    /// changed materially (doc 15.2).
    int expected_tick = 0;
    /// State hash at the end of the recorded run, on the machine that
    /// recorded it. Same-machine only, by design (doc 6.3) — it is checked
    /// when the contract hash matches and reported, not failed, otherwise.
    uint64_t expected_state_hash = 0;
    int expected_deaths = 0;

    /// One packed input per tick (input.h).
    std::vector<uint16_t> frames;

    /// Free text: who recorded it, and what the solution is. A replay whose
    /// purpose nobody remembers is a replay nobody dares delete.
    std::string note;

    InputState At(int tick) const {
        if (tick < 0 || tick >= static_cast<int>(frames.size())) return InputState::Empty();
        return InputState::Unpack(frames[static_cast<size_t>(tick)]);
    }

    size_t Ticks() const { return frames.size(); }
};

/// Records inputs as they are consumed. The recorder is fed the *same*
/// InputState the sim is stepped with, so what is recorded is exactly what
/// happened — a recorder fed the raw stick position records a run that was
/// never played.
class ReplayRecorder {
public:
    ReplayRecorder(const std::string& level_id, const Sim& sim);

    void RecordTick(const InputState& in) { replay_.frames.push_back(in.Quantized().Pack()); }

    /// Call once the run ends. Stamps the final tick, deaths and state hash.
    void Finish(const Sim& sim);

    const Replay& Get() const { return replay_; }
    Replay& Mutable() { return replay_; }

private:
    Replay replay_;
};

/// Binary, little-endian, with the header laid out so the first 16 bytes of
/// a corrupt file still say what it was meant to be. Not text: a
/// three-minute replay is ~43 KB packed and ~250 KB as JSON, and there are
/// going to be a hundred of them in the repository.
bool SaveReplay(const Replay& replay, const std::string& path, std::string* error);
bool LoadReplay(const std::string& path, Replay* out, std::string* error);

/// Serialize to / from memory, which is what the round-trip test uses.
std::string EncodeReplay(const Replay& replay);
bool DecodeReplay(const std::string& bytes, Replay* out, std::string* error);

// ------------------------------------------------------------- playback

enum class ReplayVerdict : uint8_t {
    Passed = 0,
    /// The level no longer completes. This is the failure the suite exists
    /// to catch.
    NotCompleted = 1,
    /// It completed, but far enough from the recorded tick that the physics
    /// changed materially.
    TimingDrift = 2,
    /// The contract hash differs, so the replay was recorded under different
    /// physics. Reported separately: it is not a regression, it is a replay
    /// that needs a human to re-verify and re-record.
    NeedsReverification = 3,
};

struct ReplayResult {
    ReplayVerdict verdict = ReplayVerdict::Passed;
    std::string level_id;
    bool completed = false;
    int tick = 0;
    int expected_tick = 0;
    int deaths = 0;
    uint64_t state_hash = 0;
    /// True when the run reproduced the recorded state hash exactly. Only
    /// meaningful when the contract matched.
    bool state_hash_matched = false;
    std::string message;
};

struct ReplayOptions {
    /// Ticks of slack on the completion time. Doc 15.2: "start tight and
    /// loosen it only with a reason." 30 ticks is a quarter of a second.
    int tolerance_ticks = 30;
    /// Ticks to keep stepping past the end of the recorded input before
    /// giving up. A solution whose last input is a jump completes a few
    /// ticks after the input stream ends.
    int grace_ticks = 240;
    /// Report a contract mismatch instead of failing outright.
    bool allow_contract_mismatch = true;
};

/// Runs a replay against a level. Headless: no engine, no window, no
/// timing. This function is the regression suite.
ReplayResult RunReplay(const Level& level, const Replay& replay,
                       const ReplayOptions& options = ReplayOptions{},
                       const PhysicsContract& contract = DefaultContract(),
                       const PlayerTuning& tuning = DefaultPlayerTuning());

const char* ReplayVerdictName(ReplayVerdict v);

}  // namespace ppp
