# Physics Puzzle Platformer

A 2D puzzle platformer built on a custom Verlet soft-body solver: a rigid
player pushing, dragging, squeezing and standing on deformable blobs. Targets
full gamepad support, Steam achievements, and 40 hand-authored levels.

> **Status.** v1 ships the solver, the game layer, tooling and **5 levels**,
> each with a recorded solution that CI replays bit-for-bit. The path to 40
> levels (and why level authoring is the real cost) is in
> [`docs/design.md`](docs/design.md) §3.

```
 solver/  (deterministic Verlet soft bodies, engine-free)
    ▲
 game/    (player, levels, scripts, replay)
    ▲
 godot/   (GDExtension binding, input, Steam)      tools/ (pppsim CLI, SVG renderer, replay runner)
```

## Quick start

Needs CMake, a C++17 compiler and Python 3 — no game engine required:

```bash
cd v1
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ppp_tests                     # 69 cases: solver, game layer, determinism
python3 tools/run_replays.py --twice  # replay every level's solution, twice
```

CI runs the same steps on every push and uploads an SVG render of each level
([`.github/workflows/ci.yml`](.github/workflows/ci.yml)).

## Repository layout

```
.
├── README.md          ← you are here
├── .github/workflows/ ← CI: build, tests, replay suite, level renders, g++/clang++
├── docs/
│   ├── design.md      ← full design guide (the spec code comments cite)
│   └── design.pdf     ← same guide, PDF
└── v1/                ← first implementation
    ├── solver/        Verlet soft-body solver + tests
    ├── game/          player, level loading, scripting, replay
    ├── godot/         Godot 4 GDExtension project
    ├── levels/        5 levels (JSON) + scripted solutions
    ├── replays/       recorded replays checked in CI
    ├── tools/         pppsim, SVG renderer, replay runner
    └── docs/          physics contract, determinism, level guidelines, spec errata
```

Each `vN/` directory is a self-contained iteration. Start with
[`v1/README.md`](v1/README.md).

## Versions and feedback

| Version | Summary | Feedback |
|---|---|---|
| [v1](v1/) | Deterministic soft-body solver, game layer, Godot binding, tooling, 5 levels with CI-verified replays | — |

Add a row per version as new iterations land.
