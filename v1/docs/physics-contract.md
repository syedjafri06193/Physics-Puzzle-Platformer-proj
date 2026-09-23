# The physics contract

Every number that changes the simulation, in one versioned place, with a
hash. This is the document's §3.3, and it is the thing that makes a physics
change affordable once levels exist.

## Why it exists

At the ~120 prototyped levels §3.1 projects, a physics change you cannot
verify against every level is a physics change you cannot make. Without a
contract you have two bad options: never touch the solver again, or touch it
and replay every level by hand.

The contract plus recorded solutions gives a third: change the physics, run
CI, read a list of which levels broke.

## What is in it

`solver/include/ppp/contract.h` (`PhysicsContract`) and
`game/include/ppp/player.h` (`PlayerTuning`). Both are hashed into
`Sim::ContractHash()`, along with `PHYSICS_VERSION` and
`INPUT_FORMAT_VERSION`.

Player tuning is in the contract because a change to jump height invalidates
a recorded solution exactly as thoroughly as a change to gravity.

```
$ pppsim contract
physics version   1
rate              120.0 Hz (dt 0.00833333)
gravity           900.000
damping           0.995000
substeps          2
iterations        6
particle radius   3.000
restitution       0.150
friction          0.350
sleep threshold   0.000400 after 0.350 s
pressure stiff.   0.350
jump / gravity    330.0 / 1100.0 (fall x1.80)
coyote / buffer   100 ms / 100 ms

combined hash     0x37856125d040bd2a   <- replays are checked against this
```

## Terms, not tuning knobs

`substeps` and `iterations` look like performance settings. They are not.
Constraint stiffness is coupled to iteration count (§4.3): the same
`stiffness` value with 4 iterations and with 8 produces materially different
behaviour, because each iteration applies a fraction of the correction and
the total depends on how many run.

Lowering `iterations` to gain frame time changes every soft body in the
game. It is a contract change and it invalidates every replay. If frame time
is the problem, the levers in §10.3 — structure-of-arrays, the spatial hash,
sleeping, per-level particle budgets — come first, in that order, and none
of them change the answer.

## Compiler flags

`-ffp-contract=off -fno-fast-math` are part of the contract, enforced in
`CMakeLists.txt`.

`-ffast-math` lets the compiler reassociate floating-point arithmetic, and
`-ffp-contract` lets it fuse multiply-adds differently between builds. Either
changes results in the last bits, which is enough to make a recorded replay
stop reproducing — and it will do it silently, on one developer's machine,
weeks after the flag was added.

## Changing it

1. Make the change and bump `PHYSICS_VERSION`.
2. Run `tools/run_replays.py`. Every level will report a contract mismatch;
   that is the mechanism working.
3. **Play each level that no longer completes.** By hand. This is the step
   the whole apparatus exists to make small, and it is the step that cannot
   be automated — the suite can tell you a recorded solution stopped
   working, but only a person can tell you whether the level is still a good
   puzzle.
4. Re-record with `tools/run_replays.py --record`.
5. Commit the physics change and the new replays together, and note the
   change here.

A replay re-recorded without being re-verified is a test that asserts the
bug.

## Changelog

### Version 1 — initial lock

The contract as it stands above. Two deviations from the design document's
reference material are baked into it, both recorded in
`notes-on-the-spec.md`:

- the gas model is a positional area constraint rather than an explicit
  force (§5.3), because the force version injects energy and never settles;
- `pressure` is a target-area multiplier and a body's ring is built at
  `radius · √pressure`, so the area constraint and the ring springs share an
  equilibrium.
