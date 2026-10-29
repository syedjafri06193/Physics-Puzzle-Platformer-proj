# Level guidelines

What the validator checks, what it cannot check, and the authoring rules
learned by building the five levels in `levels/`.

## Run the validator

```
pppsim validate levels/*.json
```

It is also the first thing `tools/run_replays.py` does, so CI runs it on
every commit. A few hours of work that catches the errors which otherwise
surface as a bug report from a playtester (§9.3).

| Check | Why |
|---|---|
| Wall thickness ≥ `min_wall_thickness` | Tunnelling (§4.5) |
| Spawn not inside geometry or a hazard | It happens |
| Goal reachable by flood fill | Walled-off exits |
| Checkpoints reachable | Same |
| No soft body overlapping geometry at rest | Explodes on start |
| Level fits the camera bounds | |
| Particle count within budget | Performance (§10) |
| No duplicate body / plate / door ids | Replays address bodies by id |
| No door without plates | Can never open |
| No plate that opens nothing | Usually a typo |

Errors fail the build. Warnings do not — a level with no checkpoints is fine
if it is short, and a rope pinned into a wall is meant to be.

## What the validator cannot tell you

**Whether the level is solvable.** Only a recorded solution can (§3.3).
Write one, compile it, commit it:

```
pppsim compile levels/06-yours.json levels/solutions/06-yours.txt -o replays/06-yours.ppr
```

A script that does not complete the level produces no replay. An unverified
solution in `replays/` is worse than none, because the suite would then be
asserting on a run nobody checked.

**Whether the puzzle is any good.** You know the solution and you cannot
un-know it (§9.4). Your estimate of a puzzle's difficulty is worthless and
your estimate of whether the solution is *discoverable* is worse than
worthless. Five external playtesters catch most problems. Watch without
helping; the urge to help is overwhelming and helping destroys the data.

## Authoring rules

**Hazards need floors.** A hazard is a trigger. It kills the player and does
nothing to anything else. A spike pit whose bottom is a hazard rect and
nothing else has no floor, and the first blob pushed into it falls out of
the world. Put a solid box under every hazard.

**Do not make the player walk over the thing holding the door open.** Two
drafts of `05-ballast` died on this. If a plate is on the player's path they
will shove the blob off it on the way past, and with a non-latching door
they will shove it off, watch the door shut, and have no idea why. Put the
plate in a trough, or off the path, or latch the door.

**Do not make the player carry something across a gap.** They cannot stand
on a gap. A level that asks them to is a traversal puzzle pretending to be a
physics puzzle, and it reads as a bug.

**Leave room to approach at a walk.** A rigid box hitting a light soft body
approaches the elastic limit of twice the pusher's speed, so running into a
blob punts it. That is correct physics and occasionally a useful move, but a
blob placed a few units from the spawn can only be charged into. Leave at
least 60 units of clear floor before anything the player is meant to pick up.

**`pressure` sets size as well as firmness.** A body's ring is built at
`radius · √pressure` (see `notes-on-the-spec.md` §1a). A blob at
`radius 22, pressure 1.2` is really 24 across. For a squishier body of the
same size, lower `pressure_stiffness`, not `pressure`.

## The format

JSON, with `//` line comments and trailing commas allowed, because levels are
hand-edited and both are what hand-editing produces. Rects take `{x,y,w,h}`
or `[x, y, w, h]`.

`WriteLevel` emits `%.17g`, so a level written out and read back is
bit-identical. A level that shifts by one float bit invalidates every replay
recorded against it.

The Godot scene editor is the level editor (§9.1). This format is what a
`@tool` node exports to; it stays readable so a level's diff is reviewable
and a broken level is fixable without opening the editor.

## Seeing a level

```
pppsim render levels/02-the-weight.json -o /tmp/level.svg
pppsim render levels/02-the-weight.json --script levels/solutions/02-the-weight.txt -o /tmp/solved.svg
pppsim trace  levels/02-the-weight.json levels/solutions/02-the-weight.txt --every 20
```

`render` with a script draws the player's path, which is how you debug a
solution script that stopped working. `trace` prints positions, plate mass
and door state per tick, which is how you find out why.
