#include "ppp/script.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ppp {

std::vector<InputState> Script::Expand() const {
    std::vector<InputState> out;
    for (const auto& segment : segments) {
        const int ticks =
            static_cast<int>(std::lround(static_cast<double>(segment.seconds) * PHYSICS_HZ));
        for (int i = 0; i < ticks; ++i) out.push_back(segment.input);
    }
    return out;
}

bool ParseScript(const std::string& text, Script* out, std::string* error) {
    Script script;
    std::istringstream stream(text);
    std::string line;
    int line_number = 0;

    while (std::getline(stream, line)) {
        ++line_number;
        // Comments run to the end of the line, and a line that is only a
        // comment is how a solution explains itself.
        const size_t hash = line.find('#');
        std::string body = hash == std::string::npos ? line : line.substr(0, hash);

        if (hash != std::string::npos && script.note.empty() && body.find_first_not_of(" \t\r") == std::string::npos) {
            // The first standalone comment becomes the replay's note.
            std::string comment = line.substr(hash + 1);
            const size_t start = comment.find_first_not_of(" \t\r");
            if (start != std::string::npos) script.note = comment.substr(start);
        }

        std::istringstream fields(body);
        std::string token;
        if (!(fields >> token)) continue;  // blank

        ScriptSegment segment;
        segment.source_line = line_number;

        char* end = nullptr;
        segment.seconds = std::strtof(token.c_str(), &end);
        if (end == token.c_str() || *end != '\0') {
            if (error)
                *error = "line " + std::to_string(line_number) + ": expected a duration in seconds, got '" +
                         token + "'";
            return false;
        }
        if (segment.seconds < 0.0f) {
            if (error) *error = "line " + std::to_string(line_number) + ": duration is negative";
            return false;
        }

        if (!(fields >> token)) {
            if (error)
                *error = "line " + std::to_string(line_number) +
                         ": expected a move axis after the duration (0 for none)";
            return false;
        }
        const float move = std::strtof(token.c_str(), &end);
        if (end == token.c_str() || *end != '\0') {
            if (error)
                *error = "line " + std::to_string(line_number) + ": '" + token +
                         "' is not a move axis";
            return false;
        }
        if (move < -1.0f || move > 1.0f) {
            if (error)
                *error = "line " + std::to_string(line_number) +
                         ": the move axis must be within [-1, 1]";
            return false;
        }
        segment.input.move_x = move;

        while (fields >> token) {
            if (token == "jump") segment.input.jump = true;
            else if (token == "grab") segment.input.grab = true;
            else if (token == "restart") segment.input.restart = true;
            else {
                if (error)
                    *error = "line " + std::to_string(line_number) + ": unknown button '" + token +
                             "' (expected jump, grab or restart)";
                return false;
            }
        }

        script.segments.push_back(segment);
    }

    *out = std::move(script);
    return true;
}

bool LoadScriptFile(const std::string& path, Script* out, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (!ParseScript(buffer.str(), out, error)) {
        if (error) *error = path + ": " + *error;
        return false;
    }
    return true;
}

CompileResult CompileScript(const Level& level, const Script& script, Replay* out,
                            const PhysicsContract& contract, const PlayerTuning& tuning) {
    CompileResult result;
    const std::vector<InputState> inputs = script.Expand();

    Sim sim(level, contract, tuning);
    ReplayRecorder recorder(level.id, sim);
    recorder.Mutable().note = script.note;

    size_t consumed = 0;
    for (const auto& in : inputs) {
        ++consumed;
        recorder.RecordTick(in);
        if (sim.Step(in) == SimOutcome::Completed) break;
    }

    result.tick = sim.Tick();
    result.deaths = sim.Deaths();
    result.completed = sim.Completed();
    result.unused_ticks = static_cast<int>(inputs.size() - consumed);

    if (!result.completed) {
        result.message = "the script ran " + std::to_string(inputs.size()) +
                         " ticks without reaching the goal";
        return result;
    }

    recorder.Finish(sim);
    if (out) *out = recorder.Get();
    result.message = "completed at tick " + std::to_string(result.tick);
    return result;
}

}  // namespace ppp
