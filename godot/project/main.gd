extends Node3D
# The opening, rebuilt from the user's own game files: the title shot over the real city, then (on a tap or
# Start) the camera move of the original. Everything the game's data does not say yet is marked as a guess.
#
# Command line (desktop checks): --folder=<dir with the archives and oid_manifest.txt>
#   --shot=<png> [--time=<s>]  render one frame of the intro camera at that time, save it and quit.

const SETTINGS := "user://settings.cfg"

var game: InaGame
var cam: Camera3D
var env: Environment
var sun: DirectionalLight3D
var ui: CanvasLayer
var status: Label
var prompt: Label
var folder_edit: LineEdit
var playing := false
var play_t := 0.0
var duration := 0.0
var shot_path := ""
var shot_time := 0.0
var shot_clip := "init"   # the title rests on the clip "init"; Start plays "start"
var frames_after_load := -1
var fov_as_vertical := true   # the rig's angle read as the vertical field of view (matches the reference framing)


func _ready() -> void:
	_make_ui()
	var args := {}
	for a in OS.get_cmdline_user_args() + OS.get_cmdline_args():
		if a.begins_with("--") and a.contains("="):
			var kv := a.substr(2).split("=", true, 1)
			args[kv[0]] = kv[1]
	shot_path = args.get("shot", "")
	shot_time = float(args.get("time", "0"))
	shot_clip = args.get("clip", "init")
	var folder: String = args.get("folder", "")
	if folder == "":
		folder = _saved_folder()
	if folder == "":
		folder = _find_folder()
	if OS.get_name() == "Android":
		OS.request_permissions()
	folder_edit.text = folder
	if folder != "":
		_load.call_deferred(folder)
	else:
		status.text = "Put infamous1.psarc_s, infamous2.psarc_s and oid_manifest.txt in one folder,\ntype its path below and press Load."


func _saved_folder() -> String:
	var c := ConfigFile.new()
	if c.load(SETTINGS) == OK:
		return c.get_value("files", "folder", "")
	return ""


func _find_folder() -> String:
	# the usual places on a phone, a few folders deep
	var roots := ["/storage/emulated/0/Download", "/storage/emulated/0/Documents", "/storage/emulated/0"]
	for r in roots:
		var f := _search(r, 3)
		if f != "":
			return f
	return ""


func _search(dir: String, depth: int) -> String:
	if FileAccess.file_exists(dir + "/infamous1.psarc_s") and FileAccess.file_exists(dir + "/oid_manifest.txt"):
		return dir
	if depth <= 0:
		return ""
	var d := DirAccess.open(dir)
	if d == null:
		return ""
	for sub in d.get_directories():
		if sub.begins_with("."):
			continue
		var f := _search(dir + "/" + sub, depth - 1)
		if f != "":
			return f
	return ""


func _make_ui() -> void:
	ui = CanvasLayer.new()
	add_child(ui)
	# letterbox bars as in the original title (13% of the height each)
	for top in [true, false]:
		var bar := ColorRect.new()
		bar.color = Color.BLACK
		bar.anchor_left = 0.0
		bar.anchor_right = 1.0
		bar.anchor_top = 0.0 if top else 0.87
		bar.anchor_bottom = 0.13 if top else 1.0
		bar.mouse_filter = Control.MOUSE_FILTER_IGNORE
		ui.add_child(bar)
	prompt = Label.new()
	prompt.text = "Press the Start Button"
	prompt.add_theme_font_size_override("font_size", 30)
	prompt.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	prompt.anchor_left = 0.0
	prompt.anchor_right = 1.0
	prompt.anchor_top = 0.17
	prompt.anchor_bottom = 0.25
	prompt.visible = false
	ui.add_child(prompt)
	status = Label.new()
	status.anchor_left = 0.02
	status.anchor_top = 0.02
	status.add_theme_font_size_override("font_size", 18)
	ui.add_child(status)
	var row := HBoxContainer.new()
	row.anchor_left = 0.02
	row.anchor_right = 0.98
	row.anchor_top = 0.88
	row.anchor_bottom = 0.98
	folder_edit = LineEdit.new()
	folder_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	folder_edit.placeholder_text = "Folder with the game files"
	row.add_child(folder_edit)
	var b := Button.new()
	b.text = "Load"
	b.pressed.connect(func(): _load(folder_edit.text.strip_edges()))
	row.add_child(b)
	row.name = "FolderRow"
	ui.add_child(row)


func _load(folder: String) -> void:
	status.text = "Reading the game files..."
	await get_tree().process_frame
	await get_tree().process_frame
	game = InaGame.new()
	var err := game.open(folder)
	if err != "":
		status.text = err
		return
	var c := ConfigFile.new()
	c.set_value("files", "folder", folder)
	c.save(SETTINGS)
	ui.get_node("FolderRow").visible = false

	var shot := game.intro_camera("start", 0.0)
	if shot.is_empty():
		status.text = "The opening's camera was not found: " + game.last_log()
		return
	duration = shot["duration"]
	var anchor: Vector3 = shot["eye_game"]

	status.text = "Building the city..."
	await get_tree().process_frame
	var city := game.build_city(anchor.x, anchor.y, 14000.0, 26000.0, true)
	add_child(city)
	print(game.last_log())

	var tod := game.time_of_day("predawn")
	print("time of day: ", tod)
	_make_world(tod)
	cam = Camera3D.new()
	cam.far = 4000.0
	cam.near = 0.1
	add_child(cam)
	cam.make_current()
	_place_camera(shot_clip, shot_time)
	status.text = ""
	prompt.visible = true
	frames_after_load = 0


func _make_world(tod: Dictionary) -> void:
	env = Environment.new()
	var haze: Color = tod.get("haze", Color(0.10, 0.17, 0.28))
	env.background_mode = Environment.BG_COLOR
	env.background_color = haze
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	# the game lights in gamma space (texture x light, then shown); Godot lights in linear space, so the
	# game's light values are converted to keep the same result: light_linear = light ^ 2.2
	env.ambient_light_color = _lin(tod.get("ambient", Color(0.5, 0.5, 0.5)))
	env.ambient_light_energy = 1.0
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	# haze: the game's start and end distances, as depth fog
	env.fog_enabled = true
	env.fog_mode = Environment.FOG_MODE_DEPTH
	env.fog_light_color = haze
	env.fog_depth_begin = tod.get("haze_start", 45.0)
	env.fog_depth_end = tod.get("haze_end", 100.0)
	env.fog_depth_curve = 1.0
	env.fog_density = 1.0
	# glow around lamps and bright surfaces (strength is a guess, to be matched to the reference)
	env.glow_enabled = true
	env.glow_intensity = 0.8
	env.glow_bloom = 0.05
	env.glow_hdr_threshold = 0.9
	var we := WorldEnvironment.new()
	we.environment = env
	var attrs := CameraAttributesPractical.new()
	attrs.dof_blur_far_enabled = true
	attrs.dof_blur_far_distance = 80.0
	attrs.dof_blur_far_transition = 60.0
	attrs.dof_blur_amount = 0.08
	we.camera_attributes = attrs
	add_child(we)
	sun = DirectionalLight3D.new()
	var to_sun: Vector3 = tod.get("to_sun", Vector3(0.3, 0.8, 0.3))
	sun.light_color = _lin(tod.get("sun", Color(1, 0.9, 0.7)))
	sun.light_energy = pow(0.35, 2.2)   # most of the sun is baked into the city's ambient values; a guess for the rest
	add_child(sun)
	sun.look_at_from_position(Vector3.ZERO, -to_sun, Vector3.UP if abs(to_sun.y) < 0.99 else Vector3.FORWARD)


func _lin(c: Color) -> Color:
	return Color(pow(c.r, 2.2), pow(c.g, 2.2), pow(c.b, 2.2))


func _place_camera(clip: String, t: float) -> void:
	var s := game.intro_camera(clip, t)
	if s.is_empty():
		return
	var eye: Vector3 = s["eye"]
	var fwd: Vector3 = s["forward"]
	var up: Vector3 = s["up"]
	cam.fov = s["fov_y"] if not fov_as_vertical else s["fov_rig"]
	cam.keep_aspect = Camera3D.KEEP_HEIGHT
	cam.global_transform = Transform3D(Basis.looking_at(fwd, up), eye)


func _unhandled_input(event: InputEvent) -> void:
	if game == null or cam == null or playing:
		return
	var start: bool = (event is InputEventScreenTouch and event.pressed) or (event is InputEventMouseButton and event.pressed) \
		or (event is InputEventJoypadButton and event.pressed) or (event is InputEventKey and event.pressed)
	if start:
		playing = true
		play_t = 0.0
		prompt.visible = false


func _process(delta: float) -> void:
	if frames_after_load >= 0 and shot_path != "":
		frames_after_load += 1
		if frames_after_load == 30:
			var img := get_viewport().get_texture().get_image()
			img.save_png(shot_path)
			print("saved ", shot_path)
			get_tree().quit()
		return
	if playing and cam != null:
		play_t += delta
		_place_camera("start", play_t)
		if play_t >= duration:
			playing = false
	elif prompt != null and prompt.visible:
		prompt.modulate.a = 0.6 + 0.4 * sin(Time.get_ticks_msec() / 500.0)
