# Steam integration (doc 8).
#
# The single rule this file exists to enforce: **the game must run fine
# without Steam.** Doc 8.2 is blunt about it, and it is also the difference
# between a build you can hand a playtester and one you cannot. Every entry
# point here checks `available()` first and returns something harmless, so a
# developer build, a CI run, and a DRM-free copy all work with no Steam
# client anywhere.
#
# The Steamworks calls go through GodotSteam, which is a separate GDExtension
# and may simply not be installed. That is why every call is reached through
# `Engine.has_singleton("Steam")` rather than a hard reference: a missing
# singleton must be a quiet no-op, not a parse error that takes the whole
# project down.

extends Node

## Set this to the app's real ID before shipping. 480 is Spacewar, Valve's
## public test app, which is what a development build should use.
const APP_ID := 480

## Achievements (doc 8.4). Keep the list short and make each one mean
## something; an achievement per level is noise.
const ACHIEVEMENTS := {
	"FIRST_STEPS": "Finish the first level",
	"NO_DEATHS": "Finish a level without dying",
	"SQUEEZE": "Squeeze a blob through a gap narrower than it is",
	"ALL_LEVELS": "Finish every level",
}

var _initialised := false
var _steam: Object = null
var _pending_stats := false


func _ready() -> void:
	_try_init()
	# StoreStats() is required (doc 8.5): stats and achievements set during
	# play are not persisted until it is called, and a player who closes the
	# game after earning one loses it. Batched rather than called per unlock,
	# because Valve rate-limits it.
	var timer := Timer.new()
	timer.wait_time = 10.0
	timer.autostart = true
	timer.timeout.connect(_flush)
	add_child(timer)


func _try_init() -> void:
	if not Engine.has_singleton("Steam"):
		print("Steam: not present; running standalone")
		return
	_steam = Engine.get_singleton("Steam")
	var result: Dictionary = _steam.steamInitEx(APP_ID, true)
	if result.get("status", 1) != 0:
		# Not fatal. A failed init is the normal case outside Steam and the
		# game has to carry on exactly as if Steam were never installed.
		print("Steam: init failed (%s); running standalone" % result.get("verbal", "unknown"))
		_steam = null
		return
	_initialised = true
	_steam.requestCurrentStats()


func available() -> bool:
	return _initialised and _steam != null


func _process(_delta: float) -> void:
	if available():
		_steam.run_callbacks()


## Unlocks an achievement, once. Unknown names are rejected loudly in a
## debug build and ignored in a release one: a typo should fail a playtest,
## not a player's session.
func unlock(name: String) -> void:
	if not ACHIEVEMENTS.has(name):
		assert(false, "unknown achievement: " + name)
		return
	if not available():
		return
	var state: Dictionary = _steam.getAchievement(name)
	if state.get("achieved", false):
		return
	_steam.setAchievement(name)
	_pending_stats = true


func _flush() -> void:
	if available() and _pending_stats:
		_steam.storeStats()
		_pending_stats = false


func _notification(what: int) -> void:
	# The last chance to persist. Without this, anything earned in the final
	# ten seconds of a session is lost.
	if what == NOTIFICATION_WM_CLOSE_REQUEST or what == NOTIFICATION_PREDELETE:
		_flush()


## The glyph for an input action under the player's current Steam Input
## configuration (doc 8.3).
##
## Steam Input remaps controllers, so the physical button bound to an action
## is only knowable at runtime. Asking for it every time a prompt is drawn
## is the only correct approach; a baked atlas shows a confident, wrong
## button and is the most-failed part of the Deck input criteria.
func action_glyph(action: String) -> String:
	if not available():
		return ""
	var handles: Array = _steam.getConnectedControllers()
	if handles.is_empty():
		return ""
	var handle: int = handles[0]
	var action_handle: int = _steam.getDigitalActionHandle(action)
	if action_handle == 0:
		return ""
	var origins: Array = _steam.getDigitalActionOrigins(
		handle, _steam.getActionSetHandle("GameControls"), action_handle)
	if origins.is_empty():
		return ""
	return _steam.getGlyphPNGForActionOrigin(origins[0], 0, 0)


## Steam Input must be polled before the game reads its actions, or the
## first frame of every session reads stale state.
func poll_input() -> void:
	if available():
		_steam.runFrame()


## Rich presence, cheap to set and the kind of thing that never gets done
## because nothing breaks without it.
func set_level(level_id: String) -> void:
	if available():
		_steam.setRichPresence("steam_display", "#Level")
		_steam.setRichPresence("level", level_id)
