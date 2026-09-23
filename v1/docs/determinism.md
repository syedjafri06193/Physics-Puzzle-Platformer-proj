# Determinism

The simulation is deterministic on a given machine and build. Same inputs,
same results, bit for bit. This is a requirement, not a preference (§6.1):
the replay regression suite is the only affordable way to verify a physics
change against every level, and a replay suite on a non-deterministic
simulation reports noise.

## Scope: same machine, same build

Cross-platform bit-identical floating point would mean a software float
library and a large performance cost, and the project does not need it
(§6.3). Replays are recorded and verified on CI. They are not shared between
a player's machine and someone else's, and nothing in the game is networked.

What this buys, and it is enough:

- CI can verify every level on every commit
- a developer can reproduce a CI failure exactly
- a recorded solution is a regression test with a stable expected value

## What it takes

**Fixed timestep.** 120 Hz, always. `dt` is a parameter to `Solver::Step`
only so tests can probe sub-contract behaviour; the game never passes
anything but `FIXED_DT`.

**Fixed constraint order.** Constraints live in one flat array in authored
order and are never sorted. Gauss-Seidel is order-dependent, so a "cache
efficiency" sort silently invalidates every recorded replay. Parallelising
the solve naively does the same thing and is harder to notice.

**No hash-map iteration.** The broadphase is a counting-sorted uniform grid
built from plain arrays. `Json` uses `std::map`, not `std::unordered_map`,
because load order decides particle indices and particle indices decide
solve order.

**Seeded RNG.** `Rng` is xorshift64*, seeded per level from the level file.
The standard library's *engines* are specified but its *distributions* are
not — `std::uniform_int_distribution` may differ between implementations,
which is exactly the kind of thing that breaks a replay after a toolchain
upgrade.

**Stable IDs.** Bodies carry an id from the level file, so reordering a
level's `bodies` array does not change what a replay refers to.

**Strict FP flags.** See `physics-contract.md`.

**Quantized input, always.** `Sim::Step` quantizes on the way in, so live
play and playback consume identical floats. Quantizing only on the way to
disk leaves the two on different inputs, and the drift shows up minutes
later as a replay that stops completing.

## How it is tested

`solver/tests/test_determinism.cpp`, in-process:

- the same inputs produce the same state hash, twice and then a third time
  after other allocations have churned the heap
- differing inputs produce differing hashes — otherwise the hash covers
  nothing and every other test passes vacuously
- a recorded replay reproduces its run exactly, including after a round trip
  through the binary encoding
- the contract hash responds to player tuning as well as physics
- waking a sleeping body moves nothing, and the first step after waking does
  not lurch

`tools/run_replays.py --twice`, across processes: every replay is run in two
separate `pppsim` invocations and the results compared. This is §15.3's
fresh-process check, and it has to live outside the test binary — a test
that re-runs itself to check for hidden global state carries the global
state with it.

## Two states that legitimately converge

Two runs that started from different inputs can end up bit-identical, and
this is not a determinism failure. A player pushing into a wall is clamped
to the same position whatever the stick is doing; bodies that fall asleep
freeze at whatever they had.

The first draft of the divergence test nudged the stick for one tick and
compared 200 ticks later, by which point the player was resting against a
door and everything was asleep. It failed, correctly. The test now checks
shortly after the difference, while the player is still free to move.

## What is *not* guaranteed

- across machines, compilers, or optimisation levels
- across a change to any contract term (that is what the hash detects)
- across a change to the input packing (`INPUT_FORMAT_VERSION`)
- across a change to level geometry — a level's floats are part of the
  simulation, which is why `WriteLevel` emits `%.17g`
