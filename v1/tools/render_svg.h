// SVG rendering, for looking at a level without an engine.
//
// The design document assumes Godot for everything visual (doc 9.1) and it
// is right to. This exists for the case Godot does not cover: CI has no
// engine, and "the validator says this level is fine" is not the same claim
// as "this level looks like what I meant". A 4 KB SVG per level, regenerated
// on every build, is the cheapest way to see a level and a replay's path
// without opening anything.
//
// It also renders the solution trace, which is what makes a broken solution
// script debuggable: the path shows where the player actually went.

#pragma once

#include <string>
#include <vector>

#include "ppp/level.h"
#include "ppp/sim.h"

namespace ppp {

struct RenderOptions {
    bool show_grid = true;
    bool show_plates = true;
    bool show_constraints = false;
    /// Player positions sampled over a run, drawn as a path.
    std::vector<Vec2> trail;
    std::string caption;
};

/// Renders the level and the sim's current state.
std::string RenderSvg(const Level& level, const Sim& sim, const RenderOptions& options);

/// Renders a level with nothing simulated — the authored state, which is
/// what a level review wants to look at.
std::string RenderLevelSvg(const Level& level, const RenderOptions& options);

}  // namespace ppp
