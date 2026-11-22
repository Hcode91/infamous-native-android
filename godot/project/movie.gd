class_name BinkPlayer
extends CanvasLayer
# Plays one of the game's Bink movies over everything, letterboxed on black, with its sound.
# Emits `finished` at the end (or when skipped, if skippable).

signal finished

var movie: InaMovie
var rect: TextureRect
var tex: ImageTexture
var player: AudioStreamPlayer
var playback: AudioStreamGeneratorPlayback
var t := 0.0
var skippable := true
var done := false


func play(path: String, audio_track := 0) -> String:
	layer = 20
	var bg := ColorRect.new()
	bg.color = Color.BLACK
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(bg)
	movie = InaMovie.new()
	var err := movie.open(path, audio_track)
	if err != "":
		return err
	tex = ImageTexture.create_from_image(movie.get_image())
	rect = TextureRect.new()
	rect.texture = tex
	rect.set_anchors_preset(Control.PRESET_FULL_RECT)
	rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	rect.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	add_child(rect)
	if movie.get_audio_rate() > 0:
		var gen := AudioStreamGenerator.new()
		gen.mix_rate = movie.get_audio_rate()
		gen.buffer_length = 0.5
		player = AudioStreamPlayer.new()
		player.stream = gen
		add_child(player)
		player.play()
		playback = player.get_stream_playback()
	return ""


func _process(delta: float) -> void:
	if movie == null or done:
		return
	# a long frame (loading in the background) shouldn't make the movie jump ahead
	t += min(delta, 1.0 / 15.0)
	if movie.update(t):
		tex.update(movie.get_image())
	if playback != null:
		var room := playback.get_frames_available()
		if room > 0:
			var buf := movie.take_audio(room)
			if buf.size() > 0:
				playback.push_buffer(buf)
	if movie.is_finished():
		_end()


func _input(event: InputEvent) -> void:
	if not skippable or done:
		return
	if (event is InputEventScreenTouch and event.pressed) or (event is InputEventJoypadButton and event.pressed) \
			or (event is InputEventKey and event.pressed):
		get_viewport().set_input_as_handled()
		_end()


func _end() -> void:
	done = true
	finished.emit()
	queue_free()
