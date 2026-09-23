# Physics Puzzle Platformer — v1

A 2D puzzle platformer built on a custom Verlet soft-body solver: a rigid
player pushing, dragging, squeezing and standing on deformable blobs.

The implementation of the design in [`../Documentation/README.md`](../Documentation/README.md).
Where this deviates from that document, [`docs/notes-on-the-spec.md`](docs/notes-on-the-spec.md)
says where and why — including one place the document's reference
implementation injects energy and cannot settle.

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/ppp_tests             # 69 cases: solver, game layer, determinism
python3 tools/run_replays.py  # the regression suite: every level, verified
```

No engine, no dependencies beyond a C++17 compiler and Python 3 for the CI
driver. That is deliberate — see *Layering* below.

```
$ python3 tools/run_replays.py --twice
combined hash     0x37856125d040bd2a   <- replays are checked against this

running replays
  PASS   01-first-steps       tick 294 (recorded 294)   state hash reproduced exactly
  PASS   02-the-weight        tick 593 (recorded 593)   state hash reproduced exactly
  PASS   03-stepping-stone    tick 534 (recorded 534)   state hash reproduced exactly
  PASS   04-the-squeeze       tick 528 (recorded 528)   state hash reproduced exactly
  PASS   05-ballast           tick 692 (recorded 692)   state hash reproduced exactly

all 5 level(s) pass
```

## Layering

```
solver/     Verlet solver. No engine, no dependencies, no game concepts.
game/       Player, levels, plates, doors, replays. Depends on solver only.
tools/      pppsim (validate / compile / replay / render / trace), CI driver.
godot/      GDExtension binding and GDScript. The only code that knows Godot.
levels/     Level data, and the solution script for each one.
replays/    Those solutions compiled to verified input replays.
```

The dependency arrow never points at Godot. That is what lets CI simulate
the actual game — the same `Sim` the player drives — on a machine with no
GPU and no engine installed. Every gameplay decision that drifts into a
script file is a decision the regression suite silently stops covering.

## The two load-bearing ideas

**Determinism.** Fixed 120 Hz timestep, fixed constraint order, no hash-map
iteration, seeded RNG, strict floating-point flags. Same machine, same
build, bit-identical results. [`docs/determinism.md`](docs/determinism.md).

**The physics contract.** Every number that changes the simulation lives in
one versioned struct with a hash, and every replay stores the hash it was
recorded under. Change the physics and CI tells you which levels broke
instead of you finding out from a playtester six weeks later.
[`docs/physics-contract.md`](docs/physics-contract.md).

Together they make the replay suite possible, and the replay suite is what
makes the project survivable at the ~120 levels the design projects. It
caught two real regressions while this was being built.

## Levels and solutions

Each level ships a solution as a readable script, and the script is the
source — the binary replay is generated from it:

```
# levels/solutions/02-the-weight.txt
# Walk up to the blob, take hold, walk it to the lip of the pit, let it
# drop in, then jump the pit and go through the door it holds open.
0.40  1.0
0.30  0.0
0.10  0.0  grab
1.55  0.6  grab
...
```

One segment per line: duration, stick position, buttons held. It compiles to
exactly the input stream a player would have produced, and it only becomes a
replay if it actually completes the level.

Recording by playing gives you a binary blob nobody can review, nobody can
edit when a level moves eight units, and somebody has to re-perform by hand
every time it breaks.

```bash
pppsim validate levels/*.json
pppsim compile  levels/02-the-weight.json levels/solutions/02-the-weight.txt -o replays/02-the-weight.ppr
pppsim replay   levels/02-the-weight.json replays/02-the-weight.ppr
pppsim trace    levels/02-the-weight.json levels/solutions/02-the-weight.txt --every 20
pppsim render   levels/02-the-weight.json --script levels/solutions/02-the-weight.txt -o level.svg
pppsim contract
```

`render` writes an SVG of the level and the player's path, which is how you
review a level or debug a solution without an engine.

The five levels teach, in order: movement; grab, drag and pressure plates;
standing on a soft body; deformation through a gap narrower than the body;
and mass adding up.

## Godot

```bash
# build godot-cpp first, then:
cmake -S . -B build -DGODOT_CPP_DIR=/path/to/godot-cpp
cmake --build build -j        # writes godot/bin/libppp.so
```

The extension exposes two nodes: `PppSim`, which owns a simulation and is
driven by `advance(delta)`, and `PppSoftBody`, a `@tool` node that draws a
body's rest shape in the editor so authoring is not invisible until you
press play.

Everything else about the engine side is in `godot/scripts/`: radial
deadzone and runtime Steam Input glyphs in `input.gd`, a Steam layer that
no-ops cleanly when Steam is not there in `steam.gd`, and a game loop in
`game.gd` that contains no gameplay.

## Status

Built and verified: the solver, the game layer, the tooling, five levels
with recorded solutions, the regression suite, and a GDExtension that
compiles and links against godot-cpp.

**Not run in an engine.** Godot's release binaries were unreachable from the
machine this was built on, so the GDExtension is verified to compile, link
and export its entry symbol, and the GDScript is written but has never been
executed. Everything below the engine boundary — which is all of the physics
and all of the game logic — is covered by the 69 tests and the replay suite.

Also not done, from the design's later milestones: external playtesting
(§9.4, and the design is right that it is mandatory and right that it is the
step solo developers skip), a Deck hardware pass (§10.4), and the Steam
surface beyond the code in `steam.gd`. [`docs/deck-checklist.md`](docs/deck-checklist.md)
tracks what remains.
