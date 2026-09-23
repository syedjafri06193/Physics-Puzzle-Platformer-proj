# Input handling (doc 7.2, doc 8.3).
#
# Two jobs, and both of them are places where a plausible shortcut costs a
# Deck Verified failure or a game that feels wrong:
#
#   - the deadzone is radial with rescaling, never per-axis
#   - controller glyphs come from Steam Input at runtime, never from a baked
#     atlas of Xbox buttons
#
# The values this produces go straight into PppSim.set_input, which quantizes
# them. Nothing here may branch on frame rate or wall clock: the forgiveness
# windows are measured in seconds and applied on the fixed timestep inside
# the solver (doc 7.1), and a script that tries to help with them will only
# make them frame-rate dependent again.

class_name PppInput
extends Node

const DEADZONE := 0.2

# Set false when the player is remapping, so a bind does not also act.
var enabled := true

var _last_device := "keyboard"


## Radial deadzone with rescaling (doc 7.2).
##
## Per-axis deadzones make diagonals feel wrong: you can be past the
## deadzone on both axes while barely moving the stick, or blocked on one
## axis while pushing hard diagonally.
##
## The rescale is what makes the stick feel continuous. Without it the
## output jumps from zero to the deadzone magnitude the instant you cross
## it, which reads as the stick being sticky.
##
## This mirrors ApplyRadialDeadzone in game/include/ppp/input.h, and
## test_game.cpp checks the C++ side at the boundary values. Two
## implementations is one more than anyone wants; the alternative is
## crossing the GDExtension boundary once per axis per frame for six lines
## of arithmetic.
static func apply_deadzone(stick: Vector2, dead: float = DEADZONE) -> Vector2:
	var mag := stick.length()
	if mag <= dead or mag <= 0.0:
		return Vector2.ZERO
	var scale := (mag - dead) / (1.0 - dead)
	# A stick corner reads past 1.0 on some pads; clamping here stops a
	# diagonal from being faster than a cardinal.
	scale = minf(scale, 1.0)
	return stick / mag * scale


## Analog triggers as buttons, with hysteresis so a trigger held near the
## threshold does not chatter (doc 7.2).
static func trigger_pressed(value: float, was_pressed: bool) -> bool:
	if was_pressed:
		return value > 0.35
	return value > 0.55


func move_axis() -> float:
	if not enabled:
		return 0.0
	var stick := Vector2(
		Input.get_action_strength("move_right") - Input.get_action_strength("move_left"),
		Input.get_action_strength("move_down") - Input.get_action_strength("move_up"))
	return apply_deadzone(stick).x


func jump_held() -> bool:
	return enabled and Input.is_action_pressed("jump")


func grab_held() -> bool:
	return enabled and Input.is_action_pressed("grab")


func restart_pressed() -> bool:
	return enabled and Input.is_action_pressed("restart")


func _input(event: InputEvent) -> void:
	# Tracked so the HUD can switch between key names and controller glyphs
	# the moment the player picks up a pad. A Deck player who sees WASD
	# prompts has found a Verified failure.
	if event is InputEventJoypadButton or event is InputEventJoypadMotion:
		_last_device = "gamepad"
	elif event is InputEventKey or event is InputEventMouseButton:
		_last_device = "keyboard"


func using_gamepad() -> bool:
	return _last_device == "gamepad"


## The glyph for an action, asked for at runtime (doc 8.3).
##
## Steam Input remaps controllers, so the button the player will actually
## press is not knowable from the hardware and definitely not knowable at
## build time. A baked atlas of Xbox glyphs is the single most common way a
## game fails the Deck's input-glyph criterion: it shows a correct-looking
## prompt for the wrong button.
##
## Returns a texture path when Steam can name one, and otherwise an empty
## string so the caller falls back to the action's own key name. Running
## without Steam must work (doc 8.2), so every path through here has to
## survive Steam not being there at all.
func glyph_for(action: StringName) -> String:
	if not PppSteam.available():
		return ""
	return PppSteam.action_glyph(String(action))


## What to print next to a prompt when there is no glyph.
func label_for(action: StringName) -> String:
	var events := InputMap.action_get_events(action)
	for event in events:
		if using_gamepad() and event is InputEventJoypadButton:
			return "Button %d" % event.button_index
		if not using_gamepad() and event is InputEventKey:
			return event.as_text_key_label()
	return String(action)
