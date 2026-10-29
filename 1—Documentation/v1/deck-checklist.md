# Steam Deck Verified checklist

From §8, as a pass to run before submitting, with the parts this codebase
already handles marked.

## The criteria

| Area | Requirement |
|---|---|
| Input | Full controller support; correct glyphs; no keyboard prompts |
| Display | Default resolution 1280×800; readable text; no cut-off UI |
| Seamlessness | No launcher, no external dialogs, no mid-game keyboard needs |
| System | Works on the Deck's Proton version out of the box |

## Seamlessness is the most-failed criterion (§8.2)

Not the technically hardest — the most failed. A game that plays perfectly
fails here because something asks for a mouse once.

- [ ] No launcher window before the game. None.
- [ ] No "configure graphics" dialog on first run.
- [ ] Any text entry (a player name, a level name) uses the Steam on-screen
      keyboard, not a bare text field.
- [ ] Every menu is fully navigable by d-pad and stick, including settings,
      pause, and any confirmation dialog.
- [ ] No mouse-only affordance anywhere: no drag handles, no hover-only
      tooltips, no right-click menus.
- [ ] Nothing external opens mid-game. A link to a changelog is a failure if
      it takes over the screen.

## Input

- [x] Radial deadzone with rescaling — `godot/scripts/input.gd`,
      `ApplyRadialDeadzone` in `game/include/ppp/input.h`, tested at the
      boundary values in `test_game.cpp`
- [x] Analog triggers as buttons with hysteresis — `PppInput.trigger_pressed`
- [x] Glyphs from the Steam Input API at runtime, never a baked atlas —
      `PppSteam.action_glyph`
- [ ] Every action rebindable
- [ ] The HUD switches to controller glyphs the moment a pad is touched
      (`PppInput.using_gamepad` tracks it; wire it to the prompts)

### The double-input bug (§8.3)

SDL 2.0.8 and later cooperate with Steam Input. Older SDL does not, and the
symptom is every button registering twice — once through Steam Input and
once through the raw device. Godot 4 ships a new enough SDL, but verify it
after any engine upgrade, because the failure looks like a gameplay bug
rather than a build-configuration one.

## Running without Steam

- [x] Steam is reached only through `Engine.has_singleton("Steam")`, and a
      missing singleton is a quiet no-op — `godot/scripts/steam.gd`
- [x] A failed `steamInitEx` logs and continues
- [ ] Verify a DRM-free build end to end before shipping one

This matters beyond DRM-free sales: it is what lets a playtester run a build
you emailed them, and what lets CI open the project at all.

## Achievements (§8.4)

- [x] `StoreStats()` is called — batched on a timer and again on quit.
      Without it, anything earned in the last seconds of a session is lost.
- [ ] Achievement art uploaded for every entry in `PppSteam.ACHIEVEMENTS`
- [ ] Each one tested from a clean account

## Display

- [ ] 1280×800 default, and 1280×720 letterboxed cleanly
- [ ] No text below ~18px at Deck scale
- [ ] Safe area respected; nothing important in the outer 3%
- [ ] Sustained 60 fps with the busiest level loaded, measured on the Deck
      and not on a desktop (§10.4)

## Measuring on the Deck (§10.4)

Desktop numbers predict nothing. The Deck's CPU is the constraint and the
solver is CPU-bound. Profile the real device with the heaviest level, and
budget against 8.3 ms per physics step at 120 Hz — while remembering the
game also has to render.

If it is short, the order from §10.3 is: structure-of-arrays (done), spatial
hash broadphase (done), sleeping (done), per-level particle budget (done,
enforced by the validator), SIMD last. Lowering `iterations` is not on the
list — it is a contract change that alters every soft body in the game.
