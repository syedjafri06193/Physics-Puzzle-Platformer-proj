# Notes on the spec

Places where `Documentation/README.md` is wrong, underspecified, or right in
a way that only becomes clear once the code exists. Each entry says what the
document says, what actually happens, and what this implementation does.

The document is good. Most of what follows is the difference between a
design that is correct in prose and one that is correct in floating point.

---

## 1. The pressure model injects energy (§5.3) — the big one

**The document says** to model a blob as an ideal gas: compute the polygon's
area, derive `P = nRT / A`, and apply `P` as an outward force along each
edge normal, once per step.

**What happens.** It never settles. Measured, with a blob dropped on a flat
floor and left alone:

```
step  900  center (343.72, 284.09)  energy 0.880308  sleeping 0
step 1200  center (353.86, 282.49)  energy 0.840809  sleeping 0
```

Energy *rising* at step 900, having started at zero. The blob rolled 170
units sideways across a level floor with nothing touching it, and it never
fell asleep, so it never stopped.

**Why.** The force is explicit and everything around it is positional. Once
per substep the gas pushes outward; six times per substep the ring
constraints project the boundary back. That is an undamped driven
oscillator, and nothing in the loop removes the work the force does. Worse,
Gauss-Seidel sweeps the ring in index order, so the standoff has a
direction, and the accumulated bias comes out as net motion. Reversing the
sweep direction each iteration reversed the drift — which is what proved the
diagnosis, because a real physical force does not care which way you iterate.

**What this does instead.** Müller's positional area constraint. The area is
a constraint like any other, solved in the same loop as the distance
constraints, so it cannot fight them across the boundary between a force
pass and a projection pass:

```
C  = A − k·A_rest
∇ᵢC = ½·perp(pᵢ₊₁ − pᵢ₋₁)
λ  = −C / Σⱼ wⱼ|∇ⱼC|²  ·  pressure_stiffness
Δpᵢ = λ·wᵢ·∇ᵢC
```

After the change the same scene reaches zero energy and sleeps at step ~400,
with 12 units of drift during the landing transient and none afterwards.

The document's instinct — that a gas model is what makes a blob feel like a
blob rather than a bag of springs — is right, and `pressure` survives as a
per-body parameter meaning exactly what the document wants it to mean. Only
the integration scheme changed.

### 1a. Pressure above 1.0 needs the ring rebuilt

A consequence, and not an obvious one. `pressure` is a target-area
multiplier. The area constraint and the ring's distance constraints are both
hard positional constraints, so if the ring's rest lengths describe one
shape and the area target describes a bigger one, there is no configuration
that satisfies both, and the solver oscillates between them:

| pressure | drift over 900 steps | settles? |
|---|---|---|
| 1.00 | −5.5 | yes |
| 1.05 | −181.8 | no |
| 1.20 | −526.2 | no — escaped through the floor |

Area scales as length squared, so the ring is now built at
`radius · √pressure`. Both constraints are then satisfied by the same shape,
and every value in that table settles with about 5 units of drift.

`radius` therefore means the rest radius and `pressure` means how inflated
the body is, which is what the two words mean in English anyway. Firmness at
a fixed size is `pressure_stiffness` or `ring_stiffness`.

---

## 2. A replay frame does not fit in one byte (§14.3)

**The document says**, in one paragraph: `input_frames: PackedByteArray  #
one byte of button bits per tick`, and then "Quantize analog axes to a fixed
number of steps before recording."

Both cannot be true. Buttons as bits plus a quantized axis does not fit in
eight bits at any useful axis resolution.

This uses **two bytes per tick**: one for the quantized axis (65 steps, ±32),
one for the button bits. At 120 Hz a three-minute level is 43 KB rather than
the document's 21 KB — still small enough that the argument the document is
actually making (inputs are kilobytes, state is megabytes) is unaffected.

The packing is versioned (`INPUT_FORMAT_VERSION`) and stored in every replay,
because the packing *is* part of the contract: change the number of
quantization steps and every recorded stick position decodes to a different
number.

---

## 3. The game-feel sample reads input at the wrong rate (§7.1)

**The document's sample** is `_physics_process` containing
`Input.is_action_just_pressed("jump")`.

`is_action_just_pressed` is true for the rest of the *frame* in which the
press arrived, not the physics tick. When a frame runs several physics steps
— which is the normal case for a 120 Hz simulation on a 60 Hz display, and
the entire reason the document mandates an accumulator two sections earlier
— the same press reads as just-pressed on every step in that frame. The
buffered-jump logic then fires more than once.

The document is right that the timers must run on the fixed timestep. The
fix is that the *edge detection* has to as well. Here, `InputState` carries
the raw held state and `StepPlayer` computes the edges from the previous
tick's state, inside the fixed step:

```cpp
const bool jump_pressed  =  in.jump && !p->prev_jump;
const bool jump_released = !in.jump &&  p->prev_jump;
```

This also falls out of recording inputs rather than events: a replay stores
what was held each tick, and held state is what the fixed step can reason
about.

---

## 4. "Fast-fall gravity" is an asymmetry, not a button (§7.1)

The feel table lists *Fast-fall gravity — 1.5–2× rise gravity — falling
feels weighty, rising feels floaty*. Read quickly, "fast fall" sounds like
the hold-down-to-drop move from other platformers.

The description column settles it: it is a permanent asymmetry between rise
and fall gravity, and there is no button. Implemented as
`fall_gravity_scale = 1.8`. A held fast-fall would also need its own
terminal-speed handling, because the collision sweep's guarantees are
written against `max_fall_speed`.

---

## 5. The flood fill has to treat doors as open (§9.3)

**The document says** to check that the goal is "reachable by a naive flood
fill", to catch walled-off exits.

Taken literally against all geometry, this rejects every level in the game:
a puzzle whose goal is behind a door the player has to open is precisely
what the project is. The check has to ignore door geometry.

Whether a door *can* be opened is a different question, and the document
already answers it in §3.3: that is what the recorded solution replay is
for. The two checks are complementary and the split matters —
`FloodReachable` catches "you sealed the exit", the replay suite catches
"you made it unsolvable".

### 5a. Do not expand the flood-fill bounds

A bug found by the test suite rather than by reasoning. The first version
expanded the camera bounds by a couple of cells "for safety". The margin ran
underneath the floor — the one edge of a level that has no wall — so the
fill went down one side of any barrier, along the strip below the world, and
up the other side. Every sealed level reported its goal as reachable.

The bounds are now used exactly, and everything outside them is solid.

---

## 6. A contract mismatch is its own verdict (§15.2)

**The document's CI sketch** has two outcomes: `not completed`, and
`timing drift`.

A third case matters more than either. When a replay's stored contract hash
does not match the running build, the replay was recorded under different
physics, and neither a pass nor a failure is the honest report — §3.3 says
so itself ("reported as 'needs re-verification', never silently trusted"),
but the CI sketch has nowhere to put it.

`ReplayVerdict` has four values, and the ordering between them is a decision
worth stating: **a replay that no longer completes is a failure even when
the contract differs.** That case is exactly the regression the suite exists
to find, and filing it under "needs re-verification" would hide it behind a
warning on the one commit that broke the game.

---

## 7. Hazards need floors under them

Not a spec error — a level-authoring rule the spec does not state, learned
by watching a blob fall through a spike pit and out of the world.

Hazards are triggers: they kill the player and do nothing to anything else.
A pit whose bottom is a hazard rect and nothing else has no floor. Every
hazard needs solid geometry beneath it, and `levels/03-stepping-stone.json`
carries a comment saying so.

---

## Things the document gets exactly right

Worth recording, because these were all load-bearing:

- **Fixed timestep, mandatory (§4.2).** Everything else depends on it.
- **Stiffness is coupled to iteration count (§4.3).** Confirmed by test:
  changing `iterations` alone changes the trajectory. It belongs in the
  contract, and it is in it.
- **Constraint order must never change (§6.2).** The "cache efficiency" sort
  that invalidates every replay is a real temptation with the
  structure-of-arrays layout sitting right there.
- **Shear springs (§5.2).** A lattice without them folds flat. There is a
  test that builds one both ways and compares the heights.
- **Sleeping, with `prev` zeroed (§14.2).** Without the zeroing a body
  lurches on waking, because the stale `prev` reads as a velocity covering
  however long it was asleep.
- **Record inputs, never state (§14.3).** Replaying stored positions under
  new physics reproduces the old positions and tells you nothing.
- **The replay suite is the most valuable test in the project (§15.2).** It
  caught two real regressions during this build.
- **A soft-bodied player is a different, harder game (§2.2).** Never tested,
  and on this evidence correctly never tested.

---

## Findings with no counterpart in the document

Solver bugs that the document could not have predicted, kept here because
all four are the same mistake wearing different clothes: **a positional
correction must never become velocity.**

1. **Contact surface motion accumulated across iterations.** The player's
   per-step motion was added to a particle's contact record once per solver
   iteration — six times per substep — so walking into a blob launched it
   the width of the level at roughly six times walking pace. Surface motion
   is a property of one surface, not a sum over however many times the
   position passes noticed it.

2. **Depenetration was handed back as velocity.** A particle already
   separating kept its full post-solve normal velocity, so "you are 12 units
   inside this box" became "you are travelling at 12 units per substep". Now
   capped at the separation speed the particle had *before* the position
   passes ran.

3. **Sleeping bodies were skipped before the kinematic test.** The player
   walked through any blob that had settled. The overlap test has to happen
   before the sleep skip, not after it.

4. **The player had no depenetration at all.** A swept box refuses to
   *enter* a solid and reports nothing when it is already inside one — so a
   blob shoving the player a fraction of a unit into the floor meant the
   next downward sweep saw a clear path, and the player accelerated through
   the level and out of the world. Recovery has to be part of the step.
