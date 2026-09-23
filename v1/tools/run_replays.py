#!/usr/bin/env python3
"""The replay regression suite (design doc 15.2).

Every level ships a recorded solution. This runs all of them against the
current build and reports any that no longer complete, complete far from
their recorded time, or were recorded under different physics.

Doc 15.2 calls this the most valuable test in the project, and the reason is
narrow: at the ~120 levels doc 3.1 projects, a physics change you cannot
verify against every level is a physics change you cannot make. This turns
"did I break anything?" from a week of replaying levels by hand into a
minute of CI.

It is a thin driver around `pppsim`, deliberately. The simulation lives in
one place and CI runs the same binary a developer runs, rather than a second
implementation that can drift from it.

    tools/run_replays.py                    # validate + replay everything
    tools/run_replays.py --build build      # a build directory other than ./build
    tools/run_replays.py --twice            # doc 15.3's fresh-process check
    tools/run_replays.py --record           # re-record from the solution scripts
    tools/run_replays.py --tolerance 60

Exit status is 0 only if every level validates and every replay passes.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Doc 15.2: "The timing tolerance is a judgement call ... Start tight and
# loosen it only with a reason." 30 ticks is a quarter of a second at 120 Hz.
DEFAULT_TOLERANCE = 30


def find_pppsim(build_dir: Path) -> Path:
    """Locate the pppsim binary, preferring an explicit build directory."""
    candidates = [
        build_dir / "pppsim",
        build_dir / "Release" / "pppsim.exe",
        build_dir / "pppsim.exe",
        ROOT / "build" / "pppsim",
    ]
    for c in candidates:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    found = shutil.which("pppsim")
    if found:
        return Path(found)
    sys.exit(
        f"pppsim not found. Looked in {build_dir} and on PATH.\n"
        f"Build it first:\n"
        f"    cmake -S {ROOT} -B {build_dir} && cmake --build {build_dir} -j"
    )


def run(binary: Path, args: list[str]) -> tuple[int, str]:
    result = subprocess.run(
        [str(binary), *args], capture_output=True, text=True, cwd=ROOT
    )
    return result.returncode, (result.stdout + result.stderr).rstrip()


def levels() -> list[Path]:
    return sorted((ROOT / "levels").glob("*.json"))


def replay_for(level: Path) -> Path:
    return ROOT / "replays" / (level.stem + ".ppr")


def script_for(level: Path) -> Path:
    return ROOT / "levels" / "solutions" / (level.stem + ".txt")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=str(ROOT / "build"))
    parser.add_argument("--tolerance", type=int, default=DEFAULT_TOLERANCE)
    parser.add_argument(
        "--twice",
        action="store_true",
        help="run every replay in two separate processes and require identical "
        "results (doc 15.3's hidden-global-state check)",
    )
    parser.add_argument(
        "--record",
        action="store_true",
        help="re-compile each solution script into its replay. Only after a "
        "deliberate physics change, and only with the results reviewed.",
    )
    parser.add_argument("--skip-validate", action="store_true")
    args = parser.parse_args()

    pppsim = find_pppsim(Path(args.build))
    print(f"using {pppsim.relative_to(ROOT) if pppsim.is_relative_to(ROOT) else pppsim}")

    code, contract = run(pppsim, ["contract"])
    combined = [ln for ln in contract.splitlines() if "combined hash" in ln]
    if combined:
        print(combined[0].strip())
    print()

    all_levels = levels()
    if not all_levels:
        sys.exit("no levels found in levels/")

    failures: list[tuple[str, str]] = []

    # --- validation (doc 9.3) --------------------------------------------
    if not args.skip_validate:
        print("validating levels")
        code, output = run(pppsim, ["validate", *[str(p) for p in all_levels]])
        print(output)
        if code != 0:
            failures.append(("validation", "one or more levels failed validation"))
        print()

    # --- re-recording -----------------------------------------------------
    if args.record:
        print("re-recording solutions")
        for level in all_levels:
            script = script_for(level)
            if not script.is_file():
                print(f"  --     {level.stem}: no solution script, skipped")
                continue
            code, output = run(
                pppsim, ["compile", str(level), str(script), "-o", str(replay_for(level))]
            )
            print("  " + output.replace("\n", "\n  "))
            if code != 0:
                failures.append((level.stem, "solution script no longer completes"))
        print()

    # --- the suite ---------------------------------------------------------
    print("running replays")
    for level in all_levels:
        replay = replay_for(level)
        if not replay.is_file():
            # A level with no recorded solution is not a pass. Doc 3.3 asks
            # every level to record one, and silently skipping the ones that
            # have not is how a suite ends up green and empty.
            print(f"  MISSING {level.stem}: no replay in replays/")
            failures.append((level.stem, "no recorded solution"))
            continue

        code, output = run(
            pppsim, ["replay", str(level), str(replay), "--tolerance", str(args.tolerance)]
        )
        print("  " + output.replace("\n", "\n  "))
        if code != 0:
            failures.append((level.stem, output.splitlines()[0] if output else "failed"))
            continue

        if args.twice:
            # Doc 15.3: the same replay in a fresh process. This catches
            # hidden global state, which an in-process repeat cannot.
            code2, output2 = run(
                pppsim,
                ["replay", str(level), str(replay), "--tolerance", str(args.tolerance)],
            )
            if output2 != output:
                print(f"  DIVERGED {level.stem}: a second process disagreed")
                print(f"    first:  {output}")
                print(f"    second: {output2}")
                failures.append((level.stem, "non-deterministic across processes"))

    # --- report -------------------------------------------------------------
    print()
    if failures:
        print(f"{len(failures)} failure(s):")
        for name, why in failures:
            print(f"  {name}: {why}")
        print()
        print(
            "If this followed a deliberate physics change: re-verify each level by\n"
            "hand, then re-record with --record and commit the new replays together\n"
            "with the change. A replay re-recorded without being re-verified is a\n"
            "test that asserts the bug."
        )
        return 1

    print(f"all {len(all_levels)} level(s) pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
