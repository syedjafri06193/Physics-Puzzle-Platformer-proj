# The game loop (doc 4.2, doc 9.2).
#
# This node does three things and nothing else: it feeds input to the
# simulation, it asks the simulation to advance by the frame's delta, and it
# draws the result at an interpolated position.
#
# It deliberately contains no gameplay. Whether a plate is pressed, whether
# a door opens, how far a jump goes — all of that is in libppp_game, where
# CI can run it without an engine (doc 12). Every line of gameplay that
# drifts into a script here is a line the replay regression suite (doc 15.2)
# stops covering, and it stops covering it silently.

extends Node2D

@export_file("*.json") var level_path: String = "res://levels/01-first-steps.json"
@export var debug_overlay := false

@onready var _sim: PppSim = $Sim
@onready var _input: PppInput = $Input

var _time_scale := 1.0
var _step_once := false


func _ready() -> void:
	if not _sim.load_level(level_path):
		push_error("could not load " + level_path)
		return
	PppSteam.set_level(level_path.get_file().get_basename())
	_sim.level_completed.connect(_on_completed)
	_sim.player_died.connect(_on_died)


func _process(delta: float) -> void:
	PppSteam.poll_input()

	_sim.set_input(
		_input.move_axis(),
		_input.jump_held(),
		_input.grab_held(),
		_input.restart_pressed())

	# Time controls (doc 9.2). Slow motion and single-stepping are
	# "enormously useful for debugging a physics puzzle" and cost almost
	# nothing to add; they are here rather than in a separate debug build
	# because a debug build that diverges from the shipping one is a debug
	# build that lies.
	if OS.is_debug_build():
		if Input.is_action_just_pressed("debug_slowmo"):
			_time_scale = 0.25 if is_equal_approx(_time_scale, 1.0) else 1.0
		if Input.is_action_just_pressed("debug_step"):
			_step_once = true
			_time_scale = 0.0

	var scaled := delta * _time_scale
	if _step_once:
		scaled = 1.0 / 120.0
		_step_once = false

	_sim.advance(scaled)
	queue_redraw()


func _draw() -> void:
	# Everything drawn here reads an interpolated position (doc 4.2). The
	# simulation runs at a fixed 120 Hz and the display does not; drawing
	# raw physics positions judders on any refresh rate that is not a
	# multiple of it, which on a Deck at 60 Hz means all the time.
	for i in _sim.body_count():
		var outline: PackedVector2Array = _sim.body_outline(i)
		if outline.size() < 3:
			continue
		var tint := Color(0.894, 0.651, 0.447)
		if _sim.body_sleeping(i):
			tint = tint.darkened(0.25)
		if _sim.grabbed_body() == i:
			tint = Color(0.949, 0.757, 0.306)
		draw_colored_polygon(outline, tint)
		var closed := outline.duplicate()
		closed.push_back(outline[0])
		draw_polyline(closed, Color(0.184, 0.282, 0.345), 1.5)

	var size: Vector2 = _sim.player_size()
	var pos: Vector2 = _sim.player_position()
	draw_rect(Rect2(pos - size * 0.5, size), Color(0.91, 0.91, 0.91))

	if debug_overlay:
		_draw_debug()


func _draw_debug() -> void:
	var font := ThemeDB.fallback_font
	var lines := [
		"tick %d" % _sim.tick(),
		"deaths %d" % _sim.deaths(),
		"alpha %.2f" % _sim.alpha(),
		"contract %s" % _sim.contract_hash(),
	]
	var y := 16.0
	for line in lines:
		draw_string(font, Vector2(8, y), line, HORIZONTAL_ALIGNMENT_LEFT, -1, 12,
			Color(0.62, 0.70, 0.78))
		y += 14.0


func _on_completed(tick: int) -> void:
	print("completed at tick %d with %d death(s)" % [tick, _sim.deaths()])
	if _sim.deaths() == 0:
		PppSteam.unlock("NO_DEATHS")
	if level_path.contains("01-first-steps"):
		PppSteam.unlock("FIRST_STEPS")


func _on_died(deaths: int) -> void:
	# Restart is instant and rebuilds the whole level, not just the player
	# (doc 7.3). "A 2-second restart makes experimentation cheap and a
	# 10-second one makes it expensive", and in a puzzle game built on
	# pushing things around, a respawn that leaves the pushed things where
	# they ended up can drop the player into a level they can no longer
	# solve.
	print("died (%d)" % deaths)
