#include "ppp/replay.h"

#include <cstring>
#include <fstream>
#include <sstream>

namespace ppp {
namespace {

// Explicit little-endian encoding, byte at a time. Writing a struct with
// fwrite would make the file depend on the compiler's padding and the
// machine's byte order, and a replay that only loads on the machine that
// wrote it is not a regression suite.

void PutU16(std::string* out, uint16_t v) {
    out->push_back(static_cast<char>(v & 0xFFu));
    out->push_back(static_cast<char>((v >> 8) & 0xFFu));
}

void PutU32(std::string* out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}

void PutU64(std::string* out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}

void PutString(std::string* out, const std::string& s) {
    PutU32(out, static_cast<uint32_t>(s.size()));
    out->append(s);
}

struct Reader {
    const std::string& data;
    size_t pos = 0;
    bool ok = true;

    explicit Reader(const std::string& d) : data(d) {}

    bool Need(size_t n) {
        if (pos + n > data.size()) ok = false;
        return ok;
    }

    uint16_t U16() {
        if (!Need(2)) return 0;
        const uint16_t v = static_cast<uint16_t>(static_cast<uint8_t>(data[pos])) |
                           static_cast<uint16_t>(static_cast<uint8_t>(data[pos + 1])) << 8;
        pos += 2;
        return v;
    }

    uint32_t U32() {
        if (!Need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(data[pos + static_cast<size_t>(i)]))
                 << (8 * i);
        pos += 4;
        return v;
    }

    uint64_t U64() {
        if (!Need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(static_cast<uint8_t>(data[pos + static_cast<size_t>(i)]))
                 << (8 * i);
        pos += 8;
        return v;
    }

    std::string Str() {
        const uint32_t n = U32();
        if (!ok || !Need(n)) return {};
        std::string s = data.substr(pos, n);
        pos += n;
        return s;
    }
};

}  // namespace

// ------------------------------------------------------------- recording

ReplayRecorder::ReplayRecorder(const std::string& level_id, const Sim& sim) {
    replay_.level_id = level_id;
    replay_.contract_hash = sim.ContractHash();
    replay_.physics_version = PHYSICS_VERSION;
    replay_.input_version = INPUT_FORMAT_VERSION;
    replay_.seed = sim.GetLevel().seed;
}

void ReplayRecorder::Finish(const Sim& sim) {
    replay_.expected_tick = sim.Tick();
    replay_.expected_state_hash = sim.StateHash();
    replay_.expected_deaths = sim.Deaths();
}

// ----------------------------------------------------------- serialization

std::string EncodeReplay(const Replay& r) {
    std::string out;
    PutU32(&out, REPLAY_MAGIC);
    PutU32(&out, REPLAY_FORMAT);
    PutU64(&out, r.contract_hash);
    PutU32(&out, static_cast<uint32_t>(r.physics_version));
    PutU32(&out, static_cast<uint32_t>(r.input_version));
    PutU32(&out, r.seed);
    PutU32(&out, static_cast<uint32_t>(r.expected_tick));
    PutU64(&out, r.expected_state_hash);
    PutU32(&out, static_cast<uint32_t>(r.expected_deaths));
    PutString(&out, r.level_id);
    PutString(&out, r.note);
    PutU32(&out, static_cast<uint32_t>(r.frames.size()));
    for (const uint16_t f : r.frames) PutU16(&out, f);
    return out;
}

bool DecodeReplay(const std::string& bytes, Replay* out, std::string* error) {
    Reader in(bytes);
    if (in.U32() != REPLAY_MAGIC) {
        if (error) *error = "not a replay file (bad magic)";
        return false;
    }
    const uint32_t format = in.U32();
    if (format != REPLAY_FORMAT) {
        if (error)
            *error = "replay format " + std::to_string(format) + ", this build reads " +
                     std::to_string(REPLAY_FORMAT);
        return false;
    }

    Replay r;
    r.contract_hash = in.U64();
    r.physics_version = static_cast<int>(in.U32());
    r.input_version = static_cast<int>(in.U32());
    r.seed = in.U32();
    r.expected_tick = static_cast<int>(in.U32());
    r.expected_state_hash = in.U64();
    r.expected_deaths = static_cast<int>(in.U32());
    r.level_id = in.Str();
    r.note = in.Str();
    const uint32_t count = in.U32();
    if (!in.ok) {
        if (error) *error = "replay header is truncated";
        return false;
    }
    if (!in.Need(static_cast<size_t>(count) * 2)) {
        if (error) *error = "replay input stream is truncated";
        return false;
    }
    r.frames.reserve(count);
    for (uint32_t i = 0; i < count; ++i) r.frames.push_back(in.U16());
    if (!in.ok) {
        if (error) *error = "replay input stream is truncated";
        return false;
    }

    *out = std::move(r);
    return true;
}

bool SaveReplay(const Replay& replay, const std::string& path, std::string* error) {
    const std::string bytes = EncodeReplay(replay);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        if (error) *error = "write failed for " + path;
        return false;
    }
    return true;
}

bool LoadReplay(const std::string& path, Replay* out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string bytes = buffer.str();
    if (!DecodeReplay(bytes, out, error)) {
        if (error) *error = path + ": " + *error;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- playback

const char* ReplayVerdictName(ReplayVerdict v) {
    switch (v) {
        case ReplayVerdict::Passed: return "PASS";
        case ReplayVerdict::NotCompleted: return "NOT COMPLETED";
        case ReplayVerdict::TimingDrift: return "TIMING DRIFT";
        case ReplayVerdict::NeedsReverification: return "NEEDS RE-VERIFICATION";
    }
    return "?";
}

ReplayResult RunReplay(const Level& level, const Replay& replay, const ReplayOptions& options,
                       const PhysicsContract& contract, const PlayerTuning& tuning) {
    ReplayResult result;
    result.level_id = replay.level_id;
    result.expected_tick = replay.expected_tick;

    Sim sim(level, contract, tuning);

    const bool contract_matches = sim.ContractHash() == replay.contract_hash;
    if (replay.input_version != INPUT_FORMAT_VERSION) {
        result.verdict = ReplayVerdict::NeedsReverification;
        result.message = "recorded with input format " + std::to_string(replay.input_version) +
                         ", this build packs version " + std::to_string(INPUT_FORMAT_VERSION) +
                         "; the recorded stick positions decode differently and the run would be "
                         "meaningless";
        return result;
    }

    const int total = static_cast<int>(replay.Ticks()) + options.grace_ticks;
    for (int t = 0; t < total; ++t) {
        const SimOutcome outcome = sim.Step(replay.At(t));
        if (outcome == SimOutcome::Completed) break;
    }

    result.completed = sim.Completed();
    result.tick = sim.Tick();
    result.deaths = sim.Deaths();
    result.state_hash = sim.StateHash();
    result.state_hash_matched = contract_matches && result.state_hash == replay.expected_state_hash;

    if (!result.completed) {
        result.verdict = ReplayVerdict::NotCompleted;
        result.message = "the recorded solution no longer completes the level";
        if (!contract_matches) {
            // Order matters here. A replay recorded under different physics
            // that no longer completes is exactly the signal the suite is
            // for — the physics change broke the level — so it is reported
            // as a failure, with the mismatch as context, rather than being
            // excused as "needs re-verification".
            result.message += " (recorded under a different contract: 0x" +
                              std::to_string(replay.contract_hash) + " vs 0x" +
                              std::to_string(sim.ContractHash()) + ")";
        }
        return result;
    }

    const int drift = result.tick - replay.expected_tick;
    const int magnitude = drift < 0 ? -drift : drift;
    if (magnitude > options.tolerance_ticks) {
        if (!contract_matches && options.allow_contract_mismatch) {
            result.verdict = ReplayVerdict::NeedsReverification;
            result.message = "completes, but " + std::to_string(drift) +
                             " ticks from the recorded time, under a different physics contract; "
                             "re-record it";
            return result;
        }
        result.verdict = ReplayVerdict::TimingDrift;
        result.message = "completed at tick " + std::to_string(result.tick) + ", recorded " +
                         std::to_string(replay.expected_tick) + " (" + std::to_string(drift) +
                         " ticks)";
        return result;
    }

    if (!contract_matches && options.allow_contract_mismatch) {
        result.verdict = ReplayVerdict::NeedsReverification;
        result.message = "still completes on time, but under a different physics contract; "
                         "re-record it to restore the guarantee";
        return result;
    }

    result.verdict = ReplayVerdict::Passed;
    return result;
}

}  // namespace ppp
