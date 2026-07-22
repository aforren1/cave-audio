## Phase-0 smoke scene: proves the extension loaded, the engine came up, and the readbacks
## agree with what was configured.
##
##   godot --headless --path bindings/godot
##
## Exits 0 on pass, 1 on fail — so it can stand in for a ctest until the binding grows a
## real suite. Pass `-- --stay` to keep it running (for poking at it in the editor).
extends Node

@onready var engine: BwaEngine = $BwaEngine


func _ready() -> void:
	# Child _ready() runs before the parent's, so the engine is already up (or already failed).
	var ok := true

	if not engine.is_running():
		push_error("smoke: engine did not start: %s" % engine.get_last_error())
		ok = false
	else:
		print("backend   : ", engine.get_audio_backend())
		print("channels  : ", engine.get_channel_count())
		print("rate      : ", engine.get_resolved_sample_rate())
		print("block     : ", engine.get_resolved_block_size())

		if engine.get_resolved_sample_rate() != 48000:
			push_error("smoke: expected 48000 Hz, got %d" % engine.get_resolved_sample_rate())
			ok = false
		# The default grid is 26; a layout file may legitimately be smaller. Either way it
		# must be a real count, never a hard-coded assumption.
		if engine.get_channel_count() < 4:
			push_error("smoke: implausible channel count %d" % engine.get_channel_count())
			ok = false

	print("smoke: ", "PASS" if ok else "FAIL")
	if not "--stay" in OS.get_cmdline_user_args():
		get_tree().quit(0 if ok else 1)
