// pppsim — the headless driver.
//
// Everything the project can do without an engine lives behind this one
// binary, because a second tool is a second thing to keep building. It is
// what CI runs (doc 15.2), what compiles a solution script into a verified
// replay (doc 3.3), and what produces the SVG a level review looks at.
//
//   pppsim validate  <level.json>...            — doc 9.3 checks
//   pppsim compile   <level.json> <script.txt> -o <replay.ppr>
//   pppsim replay    <level.json> <replay.ppr>  — the regression check
//   pppsim render    <level.json> [-o out.svg] [--replay r] [--tick N]
//   pppsim trace     <level.json> <script.txt>  — where the player went
//   pppsim contract                             — print the contract + hash

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ppp/level.h"
#include "ppp/replay.h"
#include "ppp/script.h"
#include "ppp/sim.h"
#include "render_svg.h"

using namespace ppp;

namespace {

int Usage() {
    std::printf(
        "pppsim — headless driver for the physics puzzle platformer\n"
        "\n"
        "  pppsim validate <level.json>...\n"
        "  pppsim compile  <level.json> <script.txt> -o <replay.ppr>\n"
        "  pppsim replay   <level.json> <replay.ppr> [--tolerance N]\n"
        "  pppsim render   <level.json> [-o out.svg] [--script s] [--tick N]\n"
        "  pppsim trace    <level.json> <script.txt> [--every N]\n"
        "  pppsim contract\n");
    return 2;
}

bool WriteFile(const std::string& path, const std::string& contents) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const size_t written = std::fwrite(contents.data(), 1, contents.size(), f);
    std::fclose(f);
    return written == contents.size();
}

const char* Arg(int argc, char** argv, const char* flag) {
    for (int i = 0; i < argc - 1; ++i)
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    return nullptr;
}

int CmdValidate(int argc, char** argv) {
    if (argc < 1) return Usage();
    int failures = 0;
    for (int i = 0; i < argc; ++i) {
        if (argv[i][0] == '-') continue;
        Level level;
        std::string error;
        if (!LoadLevelFile(argv[i], &level, &error)) {
            std::printf("ERROR  %s\n", error.c_str());
            ++failures;
            continue;
        }
        const ValidationReport report = ValidateLevel(level, DefaultContract());
        std::printf("%-7s %s  (%d particles, %zu boxes)\n", report.Ok() ? "ok" : "FAIL",
                    argv[i], level.TotalParticles(), level.boxes.size());
        for (const auto& issue : report.issues) {
            std::printf("    %-8s [%s] %s\n",
                        issue.severity == ValidationIssue::Severity::Error ? "error" : "warning",
                        issue.check.c_str(), issue.message.c_str());
        }
        if (!report.Ok()) ++failures;
    }
    if (failures > 0) std::printf("\n%d level(s) failed validation\n", failures);
    return failures == 0 ? 0 : 1;
}

int CmdCompile(int argc, char** argv) {
    if (argc < 2) return Usage();
    const char* out_path = Arg(argc, argv, "-o");
    Level level;
    Script script;
    std::string error;
    if (!LoadLevelFile(argv[0], &level, &error)) {
        std::printf("ERROR  %s\n", error.c_str());
        return 1;
    }
    if (!LoadScriptFile(argv[1], &script, &error)) {
        std::printf("ERROR  %s\n", error.c_str());
        return 1;
    }

    Replay replay;
    const CompileResult result = CompileScript(level, script, &replay);
    if (!result.completed) {
        std::printf("FAIL   %s: %s\n", argv[1], result.message.c_str());
        return 1;
    }

    std::printf("ok     %s -> tick %d, %d death(s)", argv[1], result.tick, result.deaths);
    if (result.unused_ticks > 0) std::printf(", %d unused tick(s)", result.unused_ticks);
    std::printf("\n");

    if (out_path != nullptr) {
        if (!SaveReplay(replay, out_path, &error)) {
            std::printf("ERROR  %s\n", error.c_str());
            return 1;
        }
        std::printf("       wrote %s (%zu ticks, contract 0x%016llx)\n", out_path, replay.Ticks(),
                    static_cast<unsigned long long>(replay.contract_hash));
    }
    return 0;
}

int CmdReplay(int argc, char** argv) {
    if (argc < 2) return Usage();
    Level level;
    Replay replay;
    std::string error;
    if (!LoadLevelFile(argv[0], &level, &error) || !LoadReplay(argv[1], &replay, &error)) {
        std::printf("ERROR  %s\n", error.c_str());
        return 1;
    }
    ReplayOptions options;
    if (const char* t = Arg(argc, argv, "--tolerance")) options.tolerance_ticks = std::atoi(t);

    const ReplayResult result = RunReplay(level, replay, options);
    std::printf("%-22s %s  tick %d (recorded %d)\n", ReplayVerdictName(result.verdict),
                result.level_id.c_str(), result.tick, result.expected_tick);
    if (!result.message.empty()) std::printf("    %s\n", result.message.c_str());
    if (result.state_hash_matched) std::printf("    state hash reproduced exactly\n");
    return result.verdict == ReplayVerdict::Passed ? 0 : 1;
}

int CmdTrace(int argc, char** argv) {
    if (argc < 2) return Usage();
    Level level;
    Script script;
    std::string error;
    if (!LoadLevelFile(argv[0], &level, &error) || !LoadScriptFile(argv[1], &script, &error)) {
        std::printf("ERROR  %s\n", error.c_str());
        return 1;
    }
    int every = 30;
    if (const char* e = Arg(argc, argv, "--every")) every = std::atoi(e);
    if (every < 1) every = 1;

    Sim sim(level, DefaultContract());
    const std::vector<InputState> inputs = script.Expand();

    std::printf("tick    player            vel          floor grab  plates  doors  body0\n");
    for (size_t t = 0; t <= inputs.size(); ++t) {
        if (static_cast<int>(t) % every == 0 || t == inputs.size()) {
            const Player& p = sim.GetPlayer();
            std::printf("%5zu  (%7.1f,%7.1f) (%7.1f,%7.1f)  %d    %2d   ", t, p.pos.x, p.pos.y,
                        p.vel.x, p.vel.y, p.on_floor ? 1 : 0, p.grabbed_body);
            for (const auto& plate : sim.Plates())
                std::printf("%d:%.0f ", plate.pressed ? 1 : 0, plate.mass);
            std::printf("  ");
            for (const auto& door : sim.Doors()) std::printf("%d", door.open ? 1 : 0);
            for (size_t b = 0; b < sim.GetSolver().Bodies().size(); ++b) {
                const Vec2 c = sim.GetSolver().BodyCenter(static_cast<int>(b));
                std::printf("  (%5.0f,%5.0f)%s", c.x, c.y,
                            sim.GetSolver().Bodies()[b].sleeping ? "z" : " ");
            }
            std::printf("\n");
        }
        if (t == inputs.size()) break;
        const SimOutcome outcome = sim.Step(inputs[t]);
        if (outcome == SimOutcome::Died) std::printf("%5zu  *** died ***\n", t);
        if (outcome == SimOutcome::Completed) {
            std::printf("%5zu  *** completed ***\n", t);
            break;
        }
    }
    std::printf("\n%s at tick %d, %d death(s)\n", sim.Completed() ? "COMPLETED" : "not completed",
                sim.Tick(), sim.Deaths());
    return sim.Completed() ? 0 : 1;
}

int CmdRender(int argc, char** argv) {
    if (argc < 1) return Usage();
    Level level;
    std::string error;
    if (!LoadLevelFile(argv[0], &level, &error)) {
        std::printf("ERROR  %s\n", error.c_str());
        return 1;
    }

    const char* out_path = Arg(argc, argv, "-o");
    const char* script_path = Arg(argc, argv, "--script");
    const int tick = Arg(argc, argv, "--tick") ? std::atoi(Arg(argc, argv, "--tick")) : -1;

    RenderOptions options;
    options.show_constraints = Arg(argc, argv, "--constraints") != nullptr;
    options.caption = level.id + " — " + level.name;

    std::string svg;
    if (script_path == nullptr && tick < 0) {
        svg = RenderLevelSvg(level, options);
    } else {
        Sim sim(level, DefaultContract());
        std::vector<InputState> inputs;
        if (script_path != nullptr) {
            Script script;
            if (!LoadScriptFile(script_path, &script, &error)) {
                std::printf("ERROR  %s\n", error.c_str());
                return 1;
            }
            inputs = script.Expand();
        }
        const int target = tick < 0 ? static_cast<int>(inputs.size()) : tick;
        for (int t = 0; t < target; ++t) {
            options.trail.push_back(sim.GetPlayer().pos);
            const InputState in =
                t < static_cast<int>(inputs.size()) ? inputs[static_cast<size_t>(t)]
                                                    : InputState::Empty();
            if (sim.Step(in) == SimOutcome::Completed) break;
        }
        options.caption += "  tick " + std::to_string(sim.Tick());
        if (sim.Completed()) options.caption += " — completed";
        svg = RenderSvg(level, sim, options);
    }

    if (out_path == nullptr) {
        std::fwrite(svg.data(), 1, svg.size(), stdout);
        return 0;
    }
    if (!WriteFile(out_path, svg)) {
        std::printf("ERROR  cannot write %s\n", out_path);
        return 1;
    }
    std::printf("ok     wrote %s\n", out_path);
    return 0;
}

int CmdContract() {
    const PhysicsContract& c = DefaultContract();
    const PlayerTuning& t = DefaultPlayerTuning();
    std::printf("physics version   %d\n", PHYSICS_VERSION);
    std::printf("input format      %d\n", INPUT_FORMAT_VERSION);
    std::printf("rate              %.1f Hz (dt %.8f)\n", static_cast<double>(PHYSICS_HZ),
                static_cast<double>(FIXED_DT));
    std::printf("gravity           %.3f\n", static_cast<double>(c.gravity));
    std::printf("damping           %.6f\n", static_cast<double>(c.damping));
    std::printf("substeps          %d\n", c.substeps);
    std::printf("iterations        %d\n", c.iterations);
    std::printf("particle radius   %.3f\n", static_cast<double>(c.particle_radius));
    std::printf("restitution       %.3f\n", static_cast<double>(c.restitution));
    std::printf("friction          %.3f\n", static_cast<double>(c.friction));
    std::printf("sleep threshold   %.6f after %.3f s\n", static_cast<double>(c.sleep_threshold),
                static_cast<double>(c.sleep_delay));
    std::printf("pressure stiff.   %.3f\n", static_cast<double>(c.pressure_stiffness));
    std::printf("jump / gravity    %.1f / %.1f (fall x%.2f)\n", static_cast<double>(t.jump_speed),
                static_cast<double>(t.gravity), static_cast<double>(t.fall_gravity_scale));
    std::printf("coyote / buffer   %.0f ms / %.0f ms\n", static_cast<double>(t.coyote_time) * 1000.0,
                static_cast<double>(t.jump_buffer) * 1000.0);
    std::printf("\ncontract hash     0x%016llx\n", static_cast<unsigned long long>(c.Hash()));
    std::printf("player hash       0x%016llx\n", static_cast<unsigned long long>(t.Hash()));

    Level empty;
    empty.id = "contract";
    empty.camera_bounds = AABB::FromRect(0, 0, 100, 100);
    Sim sim(empty);
    std::printf("combined hash     0x%016llx   <- replays are checked against this\n",
                static_cast<unsigned long long>(sim.ContractHash()));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return Usage();
    const std::string command = argv[1];
    if (command == "validate") return CmdValidate(argc - 2, argv + 2);
    if (command == "compile") return CmdCompile(argc - 2, argv + 2);
    if (command == "replay") return CmdReplay(argc - 2, argv + 2);
    if (command == "trace") return CmdTrace(argc - 2, argv + 2);
    if (command == "render") return CmdRender(argc - 2, argv + 2);
    if (command == "contract") return CmdContract();
    return Usage();
}
