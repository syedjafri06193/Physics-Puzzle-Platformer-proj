// Solution scripts: the authored half of a recorded solution (doc 3.3).
//
// Doc 3.3 asks every level to record a verified solution as an input
// replay. Recording one by playing the level is the obvious way to get one
// and the wrong way to keep one: a binary blob of inputs cannot be reviewed,
// cannot be edited when a level moves by eight units, and has to be
// re-performed by hand every time it breaks.
//
// A script is the same thing in text. It compiles to exactly the input
// stream a player would have produced, and the compiler *verifies* it by
// running the sim — a script that does not complete the level does not
// become a replay. So the repository holds a readable solution and a
// verified one, and the readable one is the source.
//
// The format is one segment per line:
//
//     <seconds> <move_x> [jump] [grab] [restart]     # comment
//
// `move_x` is in [-1, 1] and is quantized exactly as a stick would be.
// Buttons are held for the whole segment, so a tap is a short segment and a
// full-height jump is a long one — which is the variable-jump mechanic
// (doc 7.1) showing up in the format rather than being hidden behind it.

#pragma once

#include <string>
#include <vector>

#include "ppp/input.h"
#include "ppp/level.h"
#include "ppp/replay.h"
#include "ppp/sim.h"

namespace ppp {

struct ScriptSegment {
    float seconds = 0.0f;
    InputState input;
    int source_line = 0;
};

struct Script {
    std::string note;
    std::vector<ScriptSegment> segments;

    /// Expand to one input per tick. The tick count per segment is
    /// `round(seconds * PHYSICS_HZ)`, computed per segment rather than by
    /// accumulating a running time — accumulating lets a rounding error
    /// shift every later segment by a tick, which for a jump timed to a
    /// moving platform is the difference between a solution and a death.
    std::vector<InputState> Expand() const;
};

bool ParseScript(const std::string& text, Script* out, std::string* error);
bool LoadScriptFile(const std::string& path, Script* out, std::string* error);

struct CompileResult {
    bool completed = false;
    int tick = 0;
    int deaths = 0;
    /// Ticks of input that were left unused because the level completed
    /// first. A large number means the script overruns and should be
    /// trimmed; the compiler reports it rather than trimming silently.
    int unused_ticks = 0;
    std::string message;
};

/// Runs the script against the level and, if it completes, fills `out` with
/// a replay ready to save. A script that does not complete produces no
/// replay — an unverified solution in `replays/` is worse than none,
/// because the suite would then be asserting on a run nobody checked.
CompileResult CompileScript(const Level& level, const Script& script, Replay* out,
                            const PhysicsContract& contract = DefaultContract(),
                            const PlayerTuning& tuning = DefaultPlayerTuning());

}  // namespace ppp
