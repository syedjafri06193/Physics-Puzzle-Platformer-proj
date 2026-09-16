# Physics Puzzle Platformer — Design & Build Guide

**Project:** 2D platformer built on a custom verlet soft-body solver, with full gamepad support, Steam achievements, and 40 hand-authored levels
**Language:** C++ (solver) + GDScript (game logic), Godot 4
**Status of this document:** planning + reference

---

## Table of contents

1. [Executive summary and scope](#1-executive-summary-and-scope)
2. [Reality check](#2-reality-check)
3. [The level-authoring risk](#3-the-level-authoring-risk)
4. [The verlet solver](#4-the-verlet-solver)
5. [Soft bodies](#5-soft-bodies)
6. [Determinism](#6-determinism)
7. [Game feel and input](#7-game-feel-and-input)
8. [Steam integration](#8-steam-integration)
9. [The level pipeline](#9-the-level-pipeline)
10. [Performance](#10-performance)
11. [Tech stack and setup](#11-tech-stack-and-setup)
12. [Repository layout](#12-repository-layout)
13. [Milestone ladder](#13-milestone-ladder)
14. [Reference implementations](#14-reference-implementations)
15. [Testing](#15-testing)
16. [Stretch goals](#16-stretch-goals)
17. [References](#17-references)

---

## 1. Executive summary and scope

### The original statement

> 2D platformer built on a custom verlet soft-body solver, with full gamepad support, Steam achievements, and 40 hand-authored levels.

Five findings reshape this:

1. **The 40 levels are the project, and they are coupled to the physics in a way that makes tuning expensive.** A shipped puzzle level costs roughly 6 hours of prototype, playtest, iterate, and polish — and most ideas don't survive playtesting, so 40 shipped levels means around 120 prototyped. That's on the order of 700 hours of level work *after* the mechanics are stable. And **any change to the solver can invalidate levels you already built.** See section 3.
2. **The mitigation is recorded solution replays as a regression suite**, which makes determinism a hard requirement rather than a nice property. Change the solver, re-run all 40 recorded solutions, see which no longer complete. That single piece of infrastructure is what makes it safe to tune physics in month six. See section 6.
3. **A soft-body player character fights everything a platformer needs.** Platformers require tight, frame-accurate, predictable response; soft bodies are squishy, laggy, and emergent. *Gish* is the famous exception and its looseness is the entire design. The scoping question to answer in week one: is the **player** soft-bodied, or are the **puzzle objects** soft with a rigid player? See section 2.2.
4. **Verlet requires a fixed timestep, and constraint stiffness is coupled to iteration count.** Position Verlet has no explicit velocity — it's implicit in `x − x_prev` — so a variable timestep corrupts it. And Gauss-Seidel relaxation means *changing the iteration count changes the physics*, which is exactly the kind of change that silently breaks level 23. See sections 4.1 and 4.3.
5. **Steam Deck's most-failed criterion is "seamlessness," not performance.** Valve evaluates input, display, seamlessness, and system support. A launcher dialog before the game starts fails verification outright. And the glyph requirement has a correct answer — Steam Input's glyph API returns Valve's official images by filepath, auto-updating for new hardware like the second-generation Steam Controller released in May 2026, without recompiling. See section 8.

### Revised project statement

> A 2D puzzle platformer with a rigid, tightly-tuned player character interacting with custom verlet soft-body puzzle objects; a fixed-timestep deterministic solver with a versioned physics contract and recorded-solution regression testing across all levels; Steam Input–driven gamepad support targeting Deck Verified from the start; and a level pipeline built for fast iteration because the levels are the bulk of the work.

### Explicit non-goals

- **Not a physics sandbox.** Emergent physics toys and hand-authored puzzles pull in opposite directions; every emergent solution is a level you have to re-verify.
- **Not multiplayer.** Deterministic lockstep over a soft-body solver is a project of its own.
- **Not procedurally generated.** "Hand-authored" is in the statement and it's the right call — procedural puzzle generation is a research problem.
- **Not cross-platform deterministic.** Same-machine determinism is achievable and sufficient for the regression suite (§6.3). Bit-identical results across platforms is a much larger discipline.
- **Not 3D soft bodies.** Every problem here gets substantially harder.

---

## 2. Reality check

### 2.1 Where this kind of project dies

| Failure | Cause |
|---|---|
| **Ship date slips by a year** | Level count underestimated (§3.1) |
| **Levels break after a physics tweak** | No regression suite, no physics contract (§3.3) |
| Solver explodes at high stiffness | Iteration count raised instead of substepping (§4.3) |
| Objects tunnel through thin walls | Verlet makes CCD awkward (§4.5) |
| Controls feel mushy | Soft-body player, or no coyote time / input buffering (§7.2) |
| Physics jitters at 144 Hz | No render interpolation (§4.2) |
| Deck Verified rejected | Seamlessness, not performance (§8.2) |
| Double input on controllers | SDL not told to ignore Steam Input devices (§8.3) |
| Achievements don't fire | Not defined in the partner backend first (§8.4) |
| Difficulty curve is wrong | You cannot assess your own puzzles (§9.4) |

### 2.2 The soft-body player question ★

Answer this before writing the solver, because it determines the genre.

| | **Soft player** | **Rigid player, soft objects** |
|---|---|---|
| Control feel | Loose, momentum-driven, indirect | Tight, precise, responsive |
| Genre it becomes | Physics toy / momentum game | Puzzle platformer |
| Reference | *Gish* | *Portal*-likes, *World of Goo*-adjacent |
| Difficulty of tuning | Very high | Moderate |
| Solver requirements | Player must be robust to every case | Player is a capsule; objects can misbehave |
| Level design | Emergent, forgiving | Authored, precise |

**A soft-bodied player is a different game**, and it's the harder one by a wide margin. Every collision case, every corner, every fast-moving platform has to not destroy the character, and players will judge it against platformers with pixel-perfect control.

**Recommendation: rigid player, soft-bodied puzzle objects.** The player is a well-tuned capsule with coyote time and input buffering (§7.2); the interesting physics lives in the things they push, squeeze, stack, and ride. That preserves platformer feel while making the soft-body solver the star of the puzzles rather than a source of frustration.

If a soft player is the point of the game, that's a legitimate choice — but scope for it explicitly, and expect the control tuning alone to take as long as the solver.

### 2.3 What "custom solver" buys you

Godot, Unity, and Box2D all ship 2D physics, and none of them do soft bodies well in 2D. That's the actual justification, and it's a good one — a verlet mass-spring or pressure solver is a few hundred lines and gives you behavior no rigid-body engine can produce.

But be clear about the trade: you're also giving up broadphase, CCD, sleeping, joints, and years of stability tuning. Use the engine for rendering, input, audio, and scene management; use your solver only for the thing it's actually for.

---

## 3. The level-authoring risk ★

### 3.1 The arithmetic

Per shipped puzzle level, for a solo or very small team:

| Stage | Hours |
|---|---|
| Idea and paper sketch | 0.5 |
| Blockout / prototype | 1–2 |
| First playtest and iterate | 1–3 |
| Second playtest | 1 |
| Art pass and polish | 1–2 |
| Integration, checkpointing, testing | 0.5 |
| **Total per shipped level** | **~6** |

Now the survival rate. Most puzzle ideas don't work — they turn out to be trivial, impossible, or a rehash of an earlier level. A hit rate around **1 in 3** is realistic, and prototypes that die still cost 1–2 hours each.

```
40 shipped   × 6 hours            = 240 hours
80 discarded × 2 hours            = 160 hours
playtest rounds, difficulty tuning = 150 hours
                                    ──────────
                                    ~550–700 hours
```

That's **14 to 18 weeks of full-time work on levels alone**, after the mechanics are stable and the tooling exists. It is the single largest line item in the project and it is routinely estimated at "a couple of months."

**Design the level count as a range, not a promise.** "40 levels" on a store page is a commitment. Build 25 excellent ones and a structure that accommodates more, rather than 40 where a third are filler.

### 3.2 Levels can't start until the physics is stable

You cannot author precise puzzles against physics that's still changing. But you also can't know whether the physics is good without building levels with it.

The way out is a staged commitment:

```
Phase 1   Solver prototyping        physics changes freely, no levels
Phase 2   Mechanic validation       5-8 throwaway test levels
Phase 3   PHYSICS CONTRACT LOCKED   version it, write the regression suite
Phase 4   Level production          40 levels, physics changes require re-verification
```

**Phase 3 is a real, dated milestone**, not a vibe. After it, any solver change is a versioned change that runs the regression suite.

### 3.3 The physics contract and the regression suite ★

This is the piece of infrastructure that makes the project survivable.

```gdscript
# Every level records a verified solution as an input replay.
class_name SolutionReplay
var level_id: String
var physics_version: int      # which solver produced this
var seed: int
var inputs: PackedByteArray   # one frame of input per physics tick
var expected_completion_tick: int
```

Then, after any solver change:

```
for each of 40 levels:
    replay the recorded solution against the new solver
    did the level still complete?
    did it complete within tolerance of the expected tick?
```

A CI run that reports "levels 7, 19, and 31 no longer complete" turns a terrifying change into a three-level fix list. Without it, a solver tweak in month eight means manually re-playing forty levels, which means you won't tweak the solver, which means you ship the first physics you settled on.

**This requires determinism** (§6), which is why determinism is a hard requirement rather than an aspiration.

Bump `physics_version` on any change that could affect simulation — stiffness, iteration count, substep count, gravity, damping, collision epsilon. Replays recorded under an older version are flagged as needing re-verification rather than silently trusted.

---

## 4. The verlet solver

### 4.1 Position Verlet

```
x_{n+1} = 2·x_n − x_{n−1} + a·dt²
```

There is no velocity variable. Velocity is *implicit* in the difference between the current and previous position, which is what makes the integrator stable and constraint-friendly — moving a particle to satisfy a constraint automatically adjusts its velocity.

```cpp
struct Particle {
    Vec2  pos;
    Vec2  prev;
    Vec2  accel;
    float invMass;   // 0 = pinned
};

void Integrate(Particle& p, float dt, float damping) {
    if (p.invMass == 0.0f) { p.prev = p.pos; return; }

    const Vec2 velocity = (p.pos - p.prev) * damping;
    p.prev = p.pos;
    p.pos  = p.pos + velocity + p.accel * (dt * dt);
    p.accel = Vec2::Zero();
}
```

**Damping multiplies the implicit velocity**, which is why it appears there rather than as a separate force. A value around 0.99 per step is typical; lower and everything feels like it's underwater.

### 4.2 Fixed timestep is mandatory ★

Because velocity is implicit in a position difference, **a changing `dt` corrupts it.** The stored `x − x_prev` represents velocity *at the previous timestep's scale*; integrate with a different `dt` and you get an unphysical impulse.

Time-corrected Verlet compensates:

```
x_{n+1} = x_n + (x_n − x_{n−1})·(dt_n / dt_{n−1}) + a·dt_n²
```

It helps and it is not exact, and it destroys determinism regardless. **Use a fixed timestep with an accumulator and interpolate for rendering.**

```gdscript
const PHYSICS_HZ := 120.0
const FIXED_DT := 1.0 / PHYSICS_HZ
const MAX_STEPS := 5          # spiral-of-death guard

var accumulator := 0.0

func _process(delta: float) -> void:
    accumulator += min(delta, 0.25)    # clamp a huge frame (alt-tab, breakpoint)

    var steps := 0
    while accumulator >= FIXED_DT and steps < MAX_STEPS:
        solver.step(FIXED_DT)
        accumulator -= FIXED_DT
        steps += 1

    if steps == MAX_STEPS:
        accumulator = 0.0              # give up rather than fall further behind

    # Render between the last two physics states, or 144Hz displays judder.
    render(accumulator / FIXED_DT)
```

Two guards that matter. The `min(delta, 0.25)` handles a frame where the process was suspended — without it the accumulator holds seconds of time and the solver runs hundreds of steps. And `MAX_STEPS` prevents the spiral of death, where physics takes longer than the timestep so more steps accumulate so it takes longer still.

**120 Hz physics rather than 60** is worth considering. Stiff constraints converge better at smaller timesteps (§4.3), and doubling the rate is usually cheaper than doubling the iteration count.

### 4.3 Stiffness is coupled to iteration count ★

Constraints are solved by Gauss-Seidel relaxation — visit each constraint, project the particles toward satisfying it, repeat N times. More iterations means stiffer behavior.

**Which means changing the iteration count changes the physics.** Raise iterations from 4 to 8 to fix a wobbly object in level 30, and every level's timing shifts. This is exactly the kind of change §3.3 exists to catch.

More importantly: **substepping converges better than iterating.** For stiff constraints, running the solver twice at half the timestep beats running twice the iterations at the full timestep — the same insight behind XPBD's substepped formulation.

```cpp
void Solver::Step(float dt) {
    const float subDt = dt / substeps;
    for (int s = 0; s < substeps; ++s) {
        ApplyForces(subDt);
        Integrate(subDt);
        for (int i = 0; i < iterations; ++i) {
            SolveConstraints();      // deterministic order — §6.2
            SolveCollisions();
        }
    }
}
```

Treat `substeps` and `iterations` as part of the physics contract (§3.3). They are not tuning knobs you adjust casually.

### 4.4 Distance constraints

The workhorse, from Jakobsen's original formulation:

```cpp
void SolveDistance(Particle& a, Particle& b, float rest, float stiffness) {
    const Vec2  delta = b.pos - a.pos;
    const float dist  = delta.Length();
    if (dist < 1e-6f) return;                 // coincident — no meaningful direction

    const float wSum = a.invMass + b.invMass;
    if (wSum == 0.0f) return;                 // both pinned

    const float diff = (dist - rest) / dist;
    const Vec2  corr = delta * diff * stiffness;

    a.pos += corr * (a.invMass / wSum);
    b.pos -= corr * (b.invMass / wSum);
}
```

The `dist < 1e-6` guard prevents a division that produces NaN, and a single NaN particle propagates through every constraint it touches and destroys the whole body within a frame.

Mass weighting by `invMass / wSum` is what makes a heavy particle move less than a light one — omitting it makes every constraint behave as though both ends have equal mass.

### 4.5 Collisions and tunneling ★

Verlet makes continuous collision detection awkward because there's no explicit velocity vector to sweep along. But `pos − prev` *is* the movement this step, and that's enough:

```cpp
void SolveCollision(Particle& p, const StaticGeometry& geo) {
    // Sweep from the previous position — catches fast movement through thin walls.
    HitResult hit;
    if (geo.Raycast(p.prev, p.pos, &hit)) {
        p.pos = hit.point + hit.normal * SKIN;

        // Reflect the implicit velocity by rewriting prev.
        const Vec2 v = p.pos - p.prev;
        const Vec2 reflected = v - hit.normal * (2.0f * Vec2::Dot(v, hit.normal));
        p.prev = p.pos - reflected * restitution;
    }
}
```

**Rewriting `prev` is how you apply an impulse in Verlet.** There's no velocity to modify, so you change where the particle "came from." That's the idiom, and it's non-obvious the first time.

Three defenses against tunneling, in order:

1. **Raycast from `prev`** as above — handles most cases
2. **Substep** (§4.3) — smaller steps mean smaller movements
3. **Minimum wall thickness** as a level-design rule, enforced by a validator (§9.3)

That third one is worth making explicit. A level authoring rule of "no static collider thinner than 8 units" and an automated check is cheaper and more reliable than making the solver handle arbitrarily thin geometry.

---

## 5. Soft bodies

### 5.1 Two constructions

| | **Mass-spring lattice** | **Pressure (gas) model** |
|---|---|---|
| Structure | Grid of particles + distance constraints | Boundary particles + internal pressure |
| Holds shape via | Structural + shear + bend springs | Outward force ∝ 1/area |
| Particle count | Higher (interior filled) | Lower (boundary only) |
| Feels like | Jelly, cloth, rubber sheet | Balloon, blob, bubble |
| Cost | Higher | Lower |
| Best for | Deformable platforms, ropes, cloth | Blobs, characters, bouncy objects |

**Ship both.** They're different puzzle vocabularies — a rope bridge and a bouncy blob are different toys — and the pressure model in particular is cheap for how good it looks.

### 5.2 Lattice needs three spring types

A grid with only edge springs shears into a parallelogram and collapses. You need:

```cpp
void BuildLattice(SoftBody& b, int w, int h, float spacing) {
    // Structural: horizontal and vertical neighbours — resists stretch
    // Shear: diagonals — resists the parallelogram collapse
    // Bend: skip-one neighbours — resists folding
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        if (x + 1 < w) AddSpring(b, Idx(x,y), Idx(x+1,y), spacing, kStructural);
        if (y + 1 < h) AddSpring(b, Idx(x,y), Idx(x,y+1), spacing, kStructural);
        if (x + 1 < w && y + 1 < h) {
            AddSpring(b, Idx(x,y),   Idx(x+1,y+1), spacing * SQRT2, kShear);
            AddSpring(b, Idx(x+1,y), Idx(x,y+1),   spacing * SQRT2, kShear);
        }
        if (x + 2 < w) AddSpring(b, Idx(x,y), Idx(x+2,y), spacing * 2, kBend);
        if (y + 2 < h) AddSpring(b, Idx(x,y), Idx(x,y+2), spacing * 2, kBend);
      }
}
```

Shear springs are the ones people forget, and the symptom is a "solid" object that quietly folds flat under its own weight.

### 5.3 The pressure model

Compute the enclosed area from the boundary, then push each edge outward with a force inversely proportional to it.

```cpp
float PolygonArea(const std::vector<Particle>& ring) {
    // Shoelace. Signed — the sign tells you the winding order.
    float a = 0.0f;
    const size_t n = ring.size();
    for (size_t i = 0; i < n; ++i) {
        const Vec2& p0 = ring[i].pos;
        const Vec2& p1 = ring[(i + 1) % n].pos;
        a += p0.x * p1.y - p1.x * p0.y;
    }
    return a * 0.5f;
}

void ApplyPressure(SoftBody& b, float nRT) {
    const float area = fabsf(PolygonArea(b.ring));
    if (area < 1e-4f) return;                 // fully collapsed — don't divide

    const float pressure = nRT / area;        // ideal gas: P = nRT/V

    const size_t n = b.ring.size();
    for (size_t i = 0; i < n; ++i) {
        Particle& p0 = b.ring[i];
        Particle& p1 = b.ring[(i + 1) % n];

        const Vec2  edge = p1.pos - p0.pos;
        const float len  = edge.Length();
        if (len < 1e-6f) continue;

        const Vec2 normal = Vec2(edge.y, -edge.x) / len;   // outward for CW winding
        const Vec2 force  = normal * (pressure * len * 0.5f);

        p0.accel += force * p0.invMass;
        p1.accel += force * p1.invMass;
    }
}
```

Two details: the normal's handedness depends on winding order, and getting it backwards makes the body implode rather than inflate — a satisfying bug to watch and a confusing one to diagnose. And `nRT` is the single tuning parameter that controls how firm the blob is, which makes it a great thing to expose to level design.

Combine pressure with a ring of distance constraints so the boundary doesn't stretch indefinitely.

### 5.4 Rendering

Rendering the particle lattice directly looks like a physics demo, not a game. Two approaches:

- **Deform a mesh** — build a triangle mesh once, skin its vertices to the nearest particles with barycentric weights, update positions each frame. Sharp, art-directable, the right default.
- **Metaballs / marching squares** over the particle field for a gooey look. More expensive and very effective for blobs.

Keep rendering fully decoupled from simulation. Render interpolates between the two most recent physics states (§4.2) and never writes back.

---

## 6. Determinism ★

### 6.1 Why it's a requirement, not a preference

The level regression suite (§3.3) replays recorded inputs and asserts the level still completes. That only works if the same inputs produce the same simulation. Determinism also gives you reproducible bug reports and verifiable speedrun times, but the regression suite is the reason it's non-negotiable.

### 6.2 What it takes

| Requirement | Why |
|---|---|
| **Fixed timestep** | §4.2 — variable `dt` corrupts Verlet regardless |
| **Deterministic constraint order** | Gauss-Seidel results depend on visit order |
| **Deterministic iteration over collections** | Never iterate a hash map; use arrays or sorted keys |
| **Seeded RNG, seed recorded** | Any randomness must be reproducible |
| **No frame-rate dependence in logic** | Everything driven by the fixed step |
| **No reliance on wall-clock time** | Use the simulation tick |
| **Stable object IDs** | Spawn order must be reproducible |

The constraint-ordering one catches people. If constraints are stored per-body and bodies are iterated in creation order, you're fine — but add a spatial sort "for cache efficiency" and the results change, silently, for every existing replay.

```cpp
// Constraints live in a flat array in a fixed, authored order.
// Never sort, never parallelize without a deterministic merge.
for (int i = 0; i < constraints.size(); ++i) {
    SolveConstraint(constraints[i]);
}
```

This also means **no naive multithreading of the constraint solve.** Parallel Gauss-Seidel is order-dependent. If you need it, use graph colouring with a fixed colour assignment so each colour's constraints are independent and the order within a colour doesn't matter.

### 6.3 Same-machine is enough; cross-platform is not

Floating point is deterministic for basic operations on the same binary and architecture. It is *not* deterministic across platforms — different compilers contract multiply-adds differently, different `libm` implementations give different results for transcendentals, and x87 versus SSE differ in intermediate precision.

**Same-machine determinism is sufficient** for the regression suite, which runs in your CI on one architecture. Say so in the docs and don't promise more.

If you later want verified leaderboards, that needs either fixed-point arithmetic throughout or a server-side replay on a reference machine. Both are real projects; neither belongs in v1.

Practical guards: avoid `sin`/`cos`/`pow` in the solver where possible, and compile with strict floating-point semantics (`-ffp-contract=off`, no `-ffast-math`) so the compiler doesn't reassociate your arithmetic between builds.

### 6.4 Test it

```gdscript
func test_determinism() -> void:
    var a := simulate_level("level_12", recorded_inputs, seed=42)
    var b := simulate_level("level_12", recorded_inputs, seed=42)
    assert(a.state_hash == b.state_hash)
    assert(a.completion_tick == b.completion_tick)
```

Hash the full particle state — positions and previous positions — at the end. Run it in CI on every commit. A determinism break introduced quietly is one that invalidates every recorded replay you have.

---

## 7. Game feel and input

### 7.1 Feel is mostly a small number of cheats

Platformer control quality comes from a handful of forgiveness mechanisms that players never notice and immediately miss:

| Mechanism | Typical value | What it fixes |
|---|---|---|
| **Coyote time** | 100 ms | Jump still works just after leaving a ledge |
| **Jump buffering** | 100 ms | Jump pressed just before landing still fires |
| **Variable jump height** | — | Release early to cut the jump short |
| **Apex hang** | ~0.1 s reduced gravity | A moment of float at the top |
| **Corner correction** | 1–2 units | Nudge past a corner you barely clipped |
| **Fast-fall gravity** | 1.5–2× rise gravity | Falling feels weighty, rising feels floaty |

```gdscript
func _physics_process(_dt: float) -> void:
    if is_on_floor():
        coyote_timer = COYOTE_TIME
    else:
        coyote_timer -= FIXED_DT

    if Input.is_action_just_pressed("jump"):
        jump_buffer = JUMP_BUFFER

    jump_buffer -= FIXED_DT

    if jump_buffer > 0.0 and coyote_timer > 0.0:
        velocity.y = JUMP_VELOCITY
        jump_buffer = 0.0
        coyote_timer = 0.0        # consume both, or you get a double jump

    # Variable height: cut the rise when the button releases.
    if Input.is_action_just_released("jump") and velocity.y < 0.0:
        velocity.y *= 0.5
```

Consuming both timers on a successful jump is the bug people hit — leave `coyote_timer` set and a buffered input can fire twice.

**These run on the fixed timestep**, not `_process`, or the forgiveness windows vary with frame rate.

### 7.2 Gamepad handling

**Radial deadzone, never per-axis.** Per-axis deadzones make diagonal input feel wrong — you can be past the deadzone on both axes while barely moving the stick, or blocked on one axis while pushing hard diagonally.

```gdscript
func apply_deadzone(stick: Vector2, dead: float) -> Vector2:
    var mag := stick.length()
    if mag < dead:
        return Vector2.ZERO
    # Rescale so output starts at 0 right at the deadzone edge —
    # otherwise there's a jump from 0 to `dead` the moment you cross it.
    return stick.normalized() * ((mag - dead) / (1.0 - dead))
```

The rescale is what makes the stick feel continuous. Without it, crossing the deadzone jumps the output from zero to the deadzone magnitude.

Also worth having: analog triggers as buttons with a threshold and hysteresis, rumble for impacts (subtle — a physics game can easily overdo it), and every UI screen fully navigable by d-pad and stick.

### 7.3 Physics objects need feel too

The soft bodies are the puzzle, so their responsiveness matters as much as the player's:

- **Grab and release should be immediate**, with a visible indicator of what's grabbed
- **Predictable rest state** — an object that never quite settles is maddening in a puzzle. Add sleeping: when a body's total kinetic energy stays below a threshold for N ticks, freeze it until something touches it.
- **Undo, or at least fast restart.** Puzzle players experiment; a 2-second restart makes experimentation cheap and a 10-second one makes it expensive. This is a bigger deal for pacing than it sounds.

---

## 8. Steam integration ★

### 8.1 The Deck Verified criteria

Valve evaluates four categories: **input, display, seamlessness, and system support.** The headline requirements:

| Category | Requirement |
|---|---|
| **Input** | Full controller support with **no manual enable step**; default configuration reaches all content; **glyphs match the active input device** |
| **Display** | All text legible at **1280×800**; renders correctly without an external display; default graphics settings playable |
| **Performance** | **25 fps minimum** at default settings |
| **Seamlessness** | **No launcher, EULA popup, or setup requiring mouse/keyboard**; no compatibility warnings at launch |
| **Text input** | Any text field must have a controller-usable path (the Steam on-screen keyboard) |

A physics puzzle platformer should clear performance and display trivially. The two that bite are seamlessness and glyphs.

### 8.2 Seamlessness is the most-failed criterion ★

It's not performance. It's a dialog.

A launcher window, a "choose your graphics API" prompt, a first-run EULA, or an account creation screen before gameplay will fail verification, because none of them are navigable with a controller. Engine-default launcher dialogs are a classic cause.

**Test it the way Valve does: boot the game on a Deck (or in Desktop Mode with only a gamepad connected) and reach gameplay without touching a mouse.** If you can't, it fails.

The rest of the checklist:

- Every menu, settings screen, and pause overlay navigable by controller
- Save-name entry, if any, uses the Steam on-screen keyboard
- Smallest important UI text readable at 1280×800 — err generous
- No first-time setup of any kind

### 8.3 Steam Input, and the double-input bug ★

**Use the Steam Input API for glyphs.** Its glyph function returns a local filepath to Valve's official image for the given action origin, covering every device Steam recognizes — so a PlayStation controller shows PlayStation buttons, a Deck shows Deck buttons, and the second-generation Steam Controller released in May 2026 shows its own. **It updates automatically as Valve adds hardware, without you recompiling.** Baking your own glyph atlas means shipping a patch every time new hardware appears.

The concrete gotcha: **if you use SDL alongside Steam Input, use SDL 2.0.8 or later.** From that version, Steam can tell SDL to ignore controllers it has already configured — without it, a single button press arrives twice, once through each path. Godot uses SDL's controller database internally, so this applies even if you never call SDL yourself.

Glyph failures Valve cites specifically:

- "Press E" appearing after the player used a controller
- Trigger and bumper prompts not matching the active layout
- Glyphs flipping constantly when trackpad and stick input mix
- One submenu falling back to desktop prompts

That last one is the trap. It's usually a settings screen or a confirmation dialog someone built early and never revisited. **Audit every screen.**

### 8.4 Achievements

```gdscript
func unlock(id: String) -> void:
    if not Steam.is_initialized():
        return                                  # game must work without Steam
    if Steam.get_achievement(id).achieved:
        return                                  # already unlocked
    Steam.set_achievement(id)
    Steam.store_stats()                         # required — set alone doesn't persist
```

Four things that catch people:

- **Achievements must be created in the Steamworks partner backend first.** You cannot define them from code, and `SetAchievement` on an undefined ID silently does nothing.
- **`StoreStats()` is required.** Setting without storing means nothing persists.
- **The game must run fine without Steam.** `SteamAPI_Init` fails when launched outside the client, during development, and for anyone running a DRM-free build. Never gate gameplay on it.
- **`RunCallbacks()` must be pumped every frame** or nothing from Steam arrives.

Design the achievement list early — several will imply tracking that needs to exist from the start, and retrofitting "complete a level without touching a soft body" into a finished game is far more annoying than building it in.

### 8.5 The rest of the Steam surface

| Feature | Effort | Worth it |
|---|---|---|
| **Steam Cloud** | Low | Yes — Auto-Cloud needs only a file pattern |
| **Rich Presence** | Low | "Playing level 23" costs almost nothing |
| Steam Input configurator | Low | Ship a good default binding |
| Trading cards / workshop | Medium | Only if a level editor ships (§16) |

---

## 9. The level pipeline

### 9.1 Godot's editor is the level editor

Building a custom level editor is weeks of work. Godot's scene editor gives you placement, prefabs (scenes), undo, snapping, and multi-select for free — and custom nodes with `@tool` scripts let your soft bodies draw and configure themselves in-editor.

```gdscript
@tool
class_name SoftBodyNode extends Node2D

@export var body_type: BodyType = BodyType.PRESSURE:
    set(v): body_type = v; queue_redraw()
@export var particle_count: int = 24:
    set(v): particle_count = v; queue_redraw()
@export var stiffness: float = 0.8
@export var pressure: float = 1000.0

func _draw() -> void:
    if Engine.is_editor_hint():
        draw_preview()          # show the rest shape while authoring
```

Drawing the rest shape in-editor matters more than it sounds — authoring an invisible object and pressing play to see it is a slow loop.

### 9.2 Iteration speed is the multiplier

At ~120 prototyped levels (§3.1), the loop from "change something" to "see the result" gets run thousands of times. Every second saved is hours.

- **Hot reload of level data** without restarting the game
- **Play-from-here** — start the level at the cursor position
- **Instant restart** bound to a single button
- **Debug overlay** toggled with one key: particle positions, constraints, contact points, energy
- **Time controls** — slow-motion and step-frame are enormously useful for debugging a physics puzzle

Build these in Phase 2 (§3.2), before level production starts. They pay for themselves within a week.

### 9.3 Automated level validation

Beyond solvability replays (§3.3), cheap static checks catch authoring mistakes:

| Check | Why |
|---|---|
| Wall thickness ≥ minimum | Tunneling (§4.5) |
| Player spawn not inside geometry | Obvious, and it happens |
| Goal reachable by a naive flood fill | Catches walled-off exits |
| No soft body overlapping static geometry at rest | Explodes on start |
| Level fits the camera bounds | |
| Checkpoints exist and are reachable | |
| Total particle count within budget | Performance (§10) |

Run on every level in CI. These are a few hours of work and they catch the errors that otherwise surface as a bug report from a playtester.

### 9.4 You cannot assess your own puzzles ★

You know the solution. You cannot un-know it. Your estimate of a puzzle's difficulty is worthless, and your estimate of whether the solution is *discoverable* is worse than worthless.

**External playtesting is mandatory**, and it's the step solo developers skip most often.

- Watch someone play without helping. The urge to help is overwhelming and helping destroys the data.
- Record sessions if you can — the moment someone gets stuck is usually 30 seconds before they say anything.
- **Track time-to-solve per level.** It's the difficulty curve, measured. A level where the median is 90 seconds sitting after one where it's 8 minutes is an ordering problem.
- Five playtesters catch most problems. You don't need twenty.
- **Test the first ten minutes separately and often.** That's where players decide whether to continue, and it's the part you'll have seen so many times you're blind to it.

---

## 10. Performance

### 10.1 The budget

At 120 Hz physics you have 8.3 ms per step, and you want to use a fraction of it.

```
20 soft bodies × 60 particles = 1,200 particles
each with ~4 constraints      = 4,800 constraints
× 8 iterations × 2 substeps   = 76,800 constraint solves per step
× 120 steps/sec               = 9.2 M constraint solves/sec
```

A distance constraint is roughly 20 floating-point operations. That's ~190 MFLOP/s — trivial in C++, and **completely out of reach for GDScript.**

### 10.2 The solver must be native ★

GDScript is fast enough for game logic and nowhere near fast enough for a per-particle constraint solver at these counts. The same applies to Unity's C# without aggressive Burst/Jobs work.

**Write the solver as a GDExtension in C++ (or Rust).** Game logic, level scripting, and UI stay in GDScript, where iteration is fast and the performance doesn't matter.

That split is also what makes §6.4's determinism testing and §15's unit tests possible — the solver is a standalone native library that can be tested without the engine at all.

### 10.3 Optimizations that matter, in order

1. **Structure of arrays.** Store positions, previous positions, and inverse masses in separate contiguous arrays rather than an array of `Particle` structs. The constraint solve touches positions repeatedly and nothing else.
2. **Spatial hash for broadphase.** Uniform grid sized to the largest particle radius. Rebuild each step; it's cheap and cache-friendly.
3. **Sleeping.** A settled body costs nothing. In a puzzle game most objects are at rest most of the time, so this is often the single largest win.
4. **Budget particles per level**, validated in CI (§9.3). A level that spawns 4,000 particles is a design error, not a performance problem to solve.
5. **SIMD** the distance constraint solve, if you still need it. Four constraints at a time with SSE is straightforward — but do this last, and verify determinism afterward, because vectorized reductions can reassociate.

### 10.4 Measure on the Deck

25 fps minimum at default settings is the Verified bar, and the Deck is meaningfully slower than a development machine.

**Profile on actual hardware.** A physics-heavy level that runs at 400 fps on a desktop can land under 60 on a Deck, and finding that out during Valve's review is an expensive way to learn it.

---

## 11. Tech stack and setup

| Layer | Choice | Why |
|---|---|---|
| **Engine** | **Godot 4** | Scene editor as the level editor (§9.1); its 2D physics goes unused |
| **Solver** | **C++ GDExtension** | GDScript can't do 9 M constraint solves/sec (§10.2) |
| **Game logic** | GDScript | Fast iteration where performance doesn't matter |
| **Steam** | GodotSteam (Steamworks bindings) | Achievements, Input, Cloud |
| **Build** | Godot export templates, Windows + **native Linux** | Deck prefers native over Proton |
| **CI** | GitHub Actions | Headless determinism and replay tests (§15) |
| **Version control** | Git + LFS for art | |

Three setup notes:

**Ship a native Linux build**, not Windows-plus-Proton only. It's a Godot export preset and it removes a whole class of Deck compatibility risk.

**Get a Steam Deck, or at minimum test in Desktop Mode with only a gamepad plugged in.** The seamlessness criterion (§8.2) is impossible to evaluate honestly on a machine with a mouse attached.

**Set up the headless solver test harness in week one.** The solver as a standalone library with its own tests, buildable and runnable without Godot, is what makes §6.4 and §15 work at all.

---

## 12. Repository layout

```
Physics-Puzzle-Platformer/
├── README.md
├── docs/
│   ├── design.md               ← this document
│   ├── physics-contract.md     ← ★ versioned solver parameters (§3.3)
│   ├── level-guidelines.md     ← authoring rules incl. min wall thickness
│   └── deck-checklist.md       ← ★ the §8.1 criteria as a checklist
├── solver/                     ← ★ standalone C++, no Godot dependency
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── particle.h
│   │   ├── constraint.h
│   │   ├── softbody.h
│   │   ├── pressure.h
│   │   └── solver.h
│   ├── src/
│   └── tests/                  ← runs headless, no engine
├── gdextension/                ← thin Godot binding over solver/
├── game/
│   ├── project.godot
│   ├── scripts/
│   │   ├── player.gd           ← coyote time, buffering (§7.1)
│   │   ├── input.gd            ← radial deadzone, Steam Input glyphs
│   │   ├── steam.gd            ← ★ must no-op cleanly without Steam
│   │   └── replay.gd           ← ★ record and playback (§3.3)
│   ├── nodes/                  ← @tool nodes for in-editor authoring
│   ├── levels/                 ← 40 .tscn files
│   └── ui/                     ← every screen controller-navigable
├── replays/                    ← ★ one verified solution per level
└── tools/
    ├── validate_levels.py      ← §9.3
    └── run_replays.py          ← ★ the regression suite
```

`solver/` having no Godot dependency is the structural decision that makes determinism testing, unit testing, and the replay harness all straightforward.

---

## 13. Milestone ladder

### M0 — Scope decisions ★
**Est. 3–4 days**

Decide: soft player or rigid player (§2.2). Target level count as a range. Write the achievement list. Write `docs/deck-checklist.md`.

**Done when:** you've written down the player-body decision and can justify it.

---

### M1 — The solver, standalone ★
**Est. 3 weeks**

C++ library, no engine. Verlet integration, distance constraints, mass-spring lattice, pressure model, collision against static geometry with sweep, substepping, sleeping. Unit tests and a headless determinism test.

**Build it outside the engine.** You'll iterate faster, and everything downstream depends on it being correct and deterministic.

**Done when:** a headless test simulates 10,000 steps twice and produces identical state hashes.

---

### M2 — Godot integration
**Est. 1.5 weeks**

GDExtension binding, `@tool` nodes with in-editor preview, mesh deformation rendering, fixed-timestep loop with render interpolation.

**Done when:** a soft body authored in the Godot editor simulates and renders correctly, and physics is smooth at 60, 120, and 144 Hz display rates.

---

### M3 — Player and game feel ★
**Est. 2 weeks**

Rigid player controller, coyote time, jump buffering, variable height, corner correction, grab/release of soft bodies, camera.

**Spend longer here than feels reasonable.** Control feel is the thing players judge in the first thirty seconds, and it can't be fixed later without invalidating every level built against it.

**Done when:** movement feels good to someone who isn't you.

---

### M4 — Mechanic validation
**Est. 2 weeks**

Six to eight throwaway test levels exploring what the physics can do. Not shippable levels — experiments.

**Done when:** you know which mechanics are fun and which aren't, and you've thrown away the ones that aren't.

---

### M5 — Physics contract lock + replay infrastructure ★
**Est. 1.5 weeks**

Version the solver parameters. Build recording and playback. Build the CI regression runner. Re-verify the test levels.

**This milestone is the hinge of the project.** After it, physics changes are safe. Before it, they're terrifying.

---

### M6 — Tooling
**Est. 1.5 weeks**

Hot reload, play-from-here, instant restart, debug overlay, slow-motion and frame step, level validation in CI.

**Done when:** the edit-to-play loop is under two seconds.

---

### M7 — Level production
**Est. 14–18 weeks** ← **the bulk of the project**

Build, playtest, iterate. Record a verified solution replay for every level as it's finished. Run the regression suite continuously.

Track time-to-solve per level from playtests and use it to order the game.

---

### M8 — Steam integration
**Est. 1.5 weeks**

Achievements, Steam Input with the glyph API, Cloud saves, Rich Presence. Verify the game runs with Steam absent.

---

### M9 — Deck Verified pass ★
**Est. 1.5 weeks**

Work `docs/deck-checklist.md` on real hardware. Audit **every** screen for glyphs and controller navigation. Confirm no launcher, no mouse-required setup. Native Linux export. Profile for the 25 fps floor. Submit.

---

### M10 — Polish and launch
**Est. 3 weeks**

Audio, particles, transitions, settings, accessibility, store page, trailer, the first-ten-minutes pass.

---

## 14. Reference implementations

### 14.1 The solver step

```cpp
void Solver::Step(float dt) {
    const float subDt = dt / static_cast<float>(substeps);

    for (int s = 0; s < substeps; ++s) {
        for (auto& p : particles) {
            if (p.invMass > 0.0f) p.accel += gravity;
        }
        for (auto& body : bodies) {
            if (body.type == BodyType::Pressure && !body.sleeping) {
                ApplyPressure(body, body.nRT);
            }
        }

        for (auto& p : particles) Integrate(p, subDt, damping);

        // Fixed iteration count and fixed order — part of the physics
        // contract (docs/physics-contract.md). Changing either changes
        // the simulation and invalidates every recorded replay.
        for (int i = 0; i < iterations; ++i) {
            for (auto& c : constraints) SolveDistance(c);
            for (auto& p : particles)   SolveCollision(p, geometry);
        }
    }

    UpdateSleeping(dt);
}
```

The comment is load-bearing. Six months in, someone will want to bump `iterations` to fix one wobbly object, and the comment is what makes them run the regression suite instead.

### 14.2 Sleeping

```cpp
void Solver::UpdateSleeping(float dt) {
    for (auto& body : bodies) {
        float energy = 0.0f;
        for (int idx : body.particleIndices) {
            const Vec2 v = particles[idx].pos - particles[idx].prev;
            energy += v.LengthSquared();
        }
        energy /= static_cast<float>(body.particleIndices.size());

        if (energy < SLEEP_THRESHOLD) {
            body.sleepTimer += dt;
            if (body.sleepTimer > SLEEP_DELAY) {
                body.sleeping = true;
                // Zero the implicit velocity so it doesn't lurch on waking.
                for (int idx : body.particleIndices) {
                    particles[idx].prev = particles[idx].pos;
                }
            }
        } else {
            body.sleepTimer = 0.0f;
            body.sleeping = false;
        }
    }
}
```

Zeroing `prev` on sleep is the detail — without it, a body that wakes up resumes with whatever residual velocity it had, producing a small unexplained jolt.

### 14.3 Replay recording

```gdscript
class_name Replay extends Resource

@export var level_id: String
@export var physics_version: int
@export var seed: int
@export var input_frames: PackedByteArray   # one byte of button bits per tick

func record_tick(input: InputState) -> void:
    input_frames.append(input.pack())        # buttons as bits; axes quantized

func playback_tick(tick: int) -> InputState:
    if tick >= input_frames.size():
        return InputState.EMPTY
    return InputState.unpack(input_frames[tick])
```

Record **inputs**, never state. Inputs are a byte per tick — a three-minute level is about 21 KB at 120 Hz. State would be megabytes, and it would tell you nothing about whether the level is still solvable under new physics.

Quantize analog axes to a fixed number of steps before recording, so replay is exactly reproducible rather than nearly so.

---

## 15. Testing

### 15.1 Solver unit tests, headless

```cpp
TEST_CASE("distance constraint converges to rest length") {
    Particle a{{0, 0}, {0, 0}, {0, 0}, 1.0f};
    Particle b{{10, 0}, {10, 0}, {0, 0}, 1.0f};
    for (int i = 0; i < 20; ++i) SolveDistance(a, b, 5.0f, 1.0f);
    REQUIRE((b.pos - a.pos).Length() == Approx(5.0f).epsilon(0.001));
}

TEST_CASE("pinned particles never move") {
    Particle pinned{{0, 0}, {0, 0}, {0, 0}, 0.0f};   // invMass 0
    Particle free_{{10, 0}, {10, 0}, {0, 0}, 1.0f};
    SolveDistance(pinned, free_, 5.0f, 1.0f);
    REQUIRE(pinned.pos.x == 0.0f);
}

TEST_CASE("solver never produces NaN under stress") {
    auto solver = BuildStressScene();           // overlapping bodies, extreme forces
    for (int i = 0; i < 10000; ++i) {
        solver.Step(1.0f / 120.0f);
        for (const auto& p : solver.particles) {
            REQUIRE(std::isfinite(p.pos.x));
            REQUIRE(std::isfinite(p.pos.y));
        }
    }
}
```

That last one is the most valuable. A single NaN propagates through constraints and destroys a body within a frame, and it's usually introduced by a division guard someone removed.

### 15.2 The replay regression suite ★

```python
# tools/run_replays.py — runs in CI on every commit
def main():
    failures = []
    for replay in load_all("replays/"):
        result = run_headless(replay.level_id, replay.inputs, replay.seed)
        if not result.completed:
            failures.append((replay.level_id, "no longer completable"))
        elif abs(result.tick - replay.expected_tick) > TOLERANCE_TICKS:
            failures.append((replay.level_id,
                             f"timing drift: {result.tick} vs {replay.expected_tick}"))
    report(failures)
    sys.exit(1 if failures else 0)
```

This is the most valuable test in the project. Run it on every commit once levels exist.

The timing tolerance is a judgement call — some drift is acceptable, a lot means the physics changed materially. Start tight and loosen it only with a reason.

### 15.3 Determinism

Run the same replay twice in one process, and again in a fresh process. Compare state hashes. Catches both nondeterministic solver code and hidden global state.

### 15.4 The Deck checklist as a test pass

Manual, but written down and repeated:

- Boot on Deck, reach gameplay without a mouse
- Every screen navigable by controller
- Every glyph matches the active device, **including the ones nobody visits**
- Text readable at 1280×800
- 25 fps floor on the heaviest level
- Suspend and resume mid-level
- Steam overlay opens without crashing

### 15.5 Playtest instrumentation

Log time-to-solve, death count, restart count, and time-stuck per level, with consent. The data is what tells you the difficulty curve, and it's more reliable than anything a playtester says afterward.

---

## 16. Stretch goals

| Feature | Effort | Value |
|---|---|---|
| **Level editor for players** | Large | Steam Workshop, effectively unlimited content — but it changes the physics contract into a public API |
| **Speedrun mode with verified replays** | Medium | The replay infrastructure already exists (§14.3) |
| **Ghost replays** | Small | Race a previous attempt; falls out of replays for free |
| **Time-rewind mechanic** | Medium | Ring-buffer the state; a puzzle mechanic *and* a quality-of-life feature |
| **Level select with time-to-solve stats** | Small | Players enjoy seeing where they were slow |
| **Colour-blind and motion accessibility** | Small | Do it — it's cheap and it matters |
| **Additional soft-body types** | Medium | Cloth, rope, fluid — each is a new puzzle vocabulary |
| **Console port** | Large | The rigid-player, fixed-timestep, deterministic design ports well |
| **Deterministic cross-platform for leaderboards** | Large | Needs fixed-point arithmetic (§6.3) |

The rewind mechanic is worth a second look — it's a genuinely good puzzle-game feature (experimentation becomes free), a quality-of-life improvement, and it's largely free if state snapshots already exist for the replay system.

---

## 17. References

### Physics

- **Thomas Jakobsen**, "Advanced Character Physics" (GDC 2001) — the foundational Verlet-plus-constraints paper; §4.1 and §4.4 come from it
- **Matthias Müller et al.**, "Position Based Dynamics" — the modern formalization of constraint projection
- **Müller, Macklin et al.**, "XPBD: Position-Based Simulation of Compliant Constrained Dynamics" — why substepping beats iterating (§4.3)
- **Maciej Matyka**, "How To Implement a Pressure Soft Body Model" — the gas model in §5.3
- **Glenn Fiedler**, "Fix Your Timestep!" — the accumulator and render interpolation in §4.2
- **Erin Catto**'s GDC talks and the Box2D source — collision, sleeping, and solver stability

### Game feel

- **Steve Swink**, *Game Feel* — the vocabulary for what §7.1 is doing
- **"Celeste and Forgiveness"** and the various Celeste player-controller breakdowns — coyote time, buffering, corner correction
- GDC's *Platformer Toolkit* material — the forgiveness mechanisms, demonstrated interactively

### Steam

| Source | For |
|---|---|
| **Steam Deck Compatibility Review** (Steamworks) | The §8.1 criteria, verbatim |
| **Steam Input — Getting Started for Developers** | The glyph API and the SDL 2.0.8 note (§8.3) |
| Steamworks `ISteamUserStats` | Achievements (§8.4) |
| Steam Auto-Cloud | Save sync with a file pattern |
| GodotSteam documentation | The Godot bindings |

### Production

- **Jonathan Blow**'s talks on puzzle design — on why most puzzle ideas are bad and how to tell
- *The Witness* and *Portal* postmortems — puzzle sequencing and difficulty curves
- **Derek Yu**, *Spelunky* (Boss Fight Books) — an honest account of scope and iteration on an indie physics-adjacent game

---

## Appendix A — Decision record

| Decision | Rationale |
|---|---|
| **Rigid player, soft-bodied puzzle objects** | A soft player fights every expectation of platformer control; *Gish* is the exception that proves it. This preserves feel while keeping soft bodies as the puzzle. |
| **Level count as a range, not a promise** | ~700 hours for 40 shipped levels at a 1-in-3 survival rate. 25 excellent levels beats 40 with filler. |
| **Physics contract locked as a dated milestone** | You can't author precise puzzles against changing physics, and you can't evaluate physics without levels. Staging is the only way out. |
| **Recorded solution replays as a CI regression suite** | Turns "a solver tweak might break everything" into a three-level fix list. Without it you ship the first physics you settled on. |
| Record inputs, never state | A byte per tick versus megabytes — and state tells you nothing about whether the level is still *solvable* |
| **Determinism is a requirement, not a preference** | The regression suite depends on it entirely |
| Same-machine determinism only | Cross-platform needs fixed-point or a reference server; both are separate projects |
| No sorting or parallelizing the constraint loop | Gauss-Seidel is order-dependent; a cache optimization would silently invalidate every replay |
| **Fixed timestep, mandatory** | Verlet's velocity is implicit in `x − x_prev`; a variable `dt` corrupts it, and time-corrected Verlet is neither exact nor deterministic |
| Render interpolation between physics states | Otherwise 144 Hz displays judder against 120 Hz physics |
| Accumulator clamp plus max-steps | A suspended process otherwise runs hundreds of steps on resume; and the spiral of death is real |
| **Substeps over iterations for stiffness** | Converges better at the same cost, per XPBD. And both are contract parameters, not tuning knobs. |
| Guard every division in the solver | One NaN propagates through constraints and destroys a body within a frame |
| Rewrite `prev` to apply impulses | There's no velocity to modify in Verlet; this is the idiom |
| Raycast from `prev` for collisions, plus a minimum-wall-thickness rule | CCD is awkward in Verlet; a level-design rule with an automated check is cheaper than a general solution |
| Ship both lattice and pressure soft bodies | Different puzzle vocabularies, and pressure is cheap for how good it looks |
| Shear springs in the lattice | Without them a "solid" object folds flat under its own weight |
| **Solver as a native GDExtension, not GDScript** | ~9 M constraint solves/sec is out of reach for GDScript, and native also enables headless testing |
| Solver has zero engine dependencies | Unit tests, determinism tests, and the replay harness all run without Godot |
| Sleeping, with `prev` zeroed on sleep | Most objects are at rest most of the time; zeroing prevents a lurch on waking |
| Godot's scene editor as the level editor | A custom editor is weeks of work for tooling Godot already has |
| Tooling built before level production | The edit-to-play loop runs thousands of times; seconds compound into hours |
| **Coyote time, jump buffering, corner correction — on the fixed timestep** | Players never notice them and immediately miss them; on `_process` the windows vary with frame rate |
| Consume both timers on a successful jump | Otherwise a buffered input can fire twice |
| **Radial deadzone with rescaling**, never per-axis | Per-axis makes diagonals feel wrong; without rescaling the output jumps at the threshold |
| **Target Deck Verified from the start** | Retrofitting controller navigation into every screen is far worse than building it in |
| Seamlessness is the criterion to watch | A launcher dialog fails verification outright; it's the most commonly missed requirement |
| **Steam Input's glyph API, not a baked atlas** | Returns Valve's official images per device and auto-updates for new hardware — the 2nd-gen Steam Controller shipped May 2026 |
| SDL 2.0.8+ when using Steam Input | Earlier versions produce double input, and Godot uses SDL's controller layer internally |
| Game must run fully without Steam | `SteamAPI_Init` fails outside the client and in development; gating gameplay on it is a common bug |
| Achievement list designed early | Several imply tracking that must exist from the start |
| Native Linux export, not Proton-only | Removes a class of Deck compatibility risk for the cost of an export preset |
| **External playtesting is mandatory** | You know the solution and cannot un-know it; your difficulty estimates are worthless |
| Track time-to-solve as data | It *is* the difficulty curve, and it's more reliable than post-hoc opinions |

---

## Appendix B — Quick reference card

```
SCOPE — the levels are the project
  ~6 h per shipped level · ~1-in-3 survival rate
  40 shipped ⇒ ~120 prototyped ⇒ 550–700 hours ⇒ 14–18 weeks
  ★ lock the PHYSICS CONTRACT before level production
  ★ record a solution replay per level → CI regression suite
    change the solver → re-run 40 replays → get a fix list, not a crisis

VERLET
  x₁ = 2x₀ − x₋₁ + a·dt²      velocity is IMPLICIT in (x − x_prev)
  ★ FIXED TIMESTEP MANDATORY — variable dt corrupts the implicit velocity
  damping multiplies (x − x_prev), ~0.99/step
  apply impulses by REWRITING prev, not by touching a velocity
  accumulator: clamp delta to 0.25s · MAX_STEPS 5 · interpolate for render
  120 Hz physics ≥ 60 Hz — stiff constraints converge better

CONSTRAINTS
  ★ stiffness is COUPLED to iteration count — changing it changes physics
  ★ substeps converge better than iterations (XPBD)
  ★ Gauss-Seidel is ORDER-DEPENDENT → never sort, never naively parallelize
  guard every division — one NaN destroys a body in one frame

SOFT BODIES
  lattice: structural + SHEAR + bend   (no shear ⇒ folds flat)
  pressure: P = nRT/area, force = P × edgeLen × outward normal
            wrong winding ⇒ implodes instead of inflating
  sleep when mean |x−x_prev|² < threshold; ZERO prev on sleep

DETERMINISM — required, not optional
  fixed dt · fixed constraint order · no hash-map iteration
  seeded RNG · no wall-clock · stable IDs
  -ffp-contract=off, no -ffast-math
  same-machine only; cross-platform needs fixed-point

FEEL
  coyote time 100ms · jump buffer 100ms · variable height
  corner correction 1–2 units · fast-fall gravity 1.5–2×
  consume BOTH timers on jump, or it double-fires
  run all of it on the FIXED timestep
  radial deadzone, rescaled: normalize × (mag − dead)/(1 − dead)

STEAM DECK VERIFIED — four categories
  input · display · seamlessness · system support
  25 fps min · text legible at 1280×800 · no manual controller enable
  ★ SEAMLESSNESS is the most-failed: no launcher, no EULA popup,
    nothing needing a mouse before gameplay
  ★ glyphs must match the ACTIVE device — use Steam Input's glyph API
    (returns Valve's official images, auto-updates for new hardware)
  ★ SDL 2.0.8+ or you get DOUBLE INPUT with Steam Input
  audit EVERY screen — one stale submenu fails the review
  ship a NATIVE LINUX build

ACHIEVEMENTS
  define in the partner backend FIRST — SetAchievement on an
    undefined id silently does nothing
  StoreStats() is required · RunCallbacks() every frame
  game must run fine with Steam absent

PERFORMANCE
  20 bodies × 60 particles × 4 constraints × 8 iter × 2 sub × 120 Hz
    ≈ 9.2 M constraint solves/sec
  ★ far beyond GDScript → native GDExtension
  SoA layout · spatial hash · sleeping · per-level particle budget in CI
  profile on actual Deck hardware, not the dev machine
```
