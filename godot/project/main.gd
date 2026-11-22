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
var prompt: Control
var plate: TextureRect
var logo: TextureRect
var music: AudioStreamPlayer
var ambience: AudioStreamPlayer
var game_folder := ""
var folder_edit: LineEdit
var manifest_edit: LineEdit
var playing := false
var play_t := 0.0
var duration := 0.0
var shot_path := ""
var shot_time := 0.0
var shot_clip := "init"   # the title rests on the clip "init"; Start plays "start"
var frames_after_load := -1
var near_radius := 14000.0 if OS.get_name() != "Android" else 9000.0   # full detail for sectors this close
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
	if args.has("folder"):
		var f: String = args["folder"]
		_load.call_deferred(f, args.get("manifest", f + "/oid_manifest.txt"))
		return
	if OS.get_name() == "Android":
		get_tree().on_request_permissions_result.connect(func(_p, _g): _locate())
		OS.request_permissions()
	_locate.call_deferred()


# Finds the game's USRDIR (holding infamous1.psarc_s) and oid_manifest.txt: the saved places first, then
# the usual folders of a phone, a few levels deep.
func _locate() -> void:
	if game != null:
		return
	var c := ConfigFile.new()
	var folder := ""
	var manifest := ""
	if c.load(SETTINGS) == OK:
		folder = c.get_value("files", "folder", "")
		manifest = c.get_value("files", "manifest", "")
	if folder == "" or not FileAccess.file_exists(folder + "/infamous1.psarc_s"):
		folder = ""
		for r in ["/storage/emulated/0/Download", "/storage/emulated/0/Documents", "/storage/emulated/0"]:
			folder = _search(r, "infamous1.psarc_s", 6)
			if folder != "":
				break
	if manifest == "" or not FileAccess.file_exists(manifest):
		manifest = ""
		var places := [folder, folder.get_base_dir(), "/storage/emulated/0/Download", "/storage/emulated/0/Documents", "/storage/emulated/0"]
		for r in places:
			if r == "":
				continue
			var d := _search(r, "oid_manifest.txt", 3 if r != folder else 0)
			if d != "":
				manifest = d + "/oid_manifest.txt"
				break
	folder_edit.text = folder
	manifest_edit.text = manifest
	if folder != "" and manifest != "":
		_load(folder, manifest)
	else:
		status.text = "Game files not found. On Android allow 'All files access' for this app,\nthen tap Load. Or type the folders below (the game's USRDIR and the manifest file)."


func _search(dir: String, file: String, depth: int) -> String:
	if FileAccess.file_exists(dir + "/" + file):
		return dir
	if depth <= 0:
		return ""
	var d := DirAccess.open(dir)
	if d == null:
		return ""
	for sub in d.get_directories():
		if sub.begins_with(".") or sub == "Android":
			continue
		var f := _search(dir + "/" + sub, file, depth - 1)
		if f != "":
			return f
	return ""


func _make_ui() -> void:
	ui = CanvasLayer.new()
	add_child(ui)
	# letterbox bars as in the original title (11% of the height at the top, 15% at the bottom with the logo)
	for top in [true, false]:
		var bar := ColorRect.new()
		bar.color = Color.BLACK
		bar.anchor_left = 0.0
		bar.anchor_right = 1.0
		bar.anchor_top = 0.0 if top else 0.847
		bar.anchor_bottom = 0.11 if top else 1.0
		bar.mouse_filter = Control.MOUSE_FILTER_IGNORE
		ui.add_child(bar)
	# the prompt sits on the menu selector's two smeared lines, near the top of the picture
	prompt = Control.new()
	prompt.anchor_left = 0.385
	prompt.anchor_right = 0.615
	prompt.anchor_top = 0.165
	prompt.anchor_bottom = 0.295
	prompt.mouse_filter = Control.MOUSE_FILTER_IGNORE
	prompt.visible = false
	ui.add_child(prompt)
	plate = TextureRect.new()
	plate.set_anchors_preset(Control.PRESET_FULL_RECT)
	plate.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	plate.stretch_mode = TextureRect.STRETCH_SCALE
	plate.mouse_filter = Control.MOUSE_FILTER_IGNORE
	prompt.add_child(plate)
	var text := Label.new()
	text.text = "Press the Start Button"
	text.add_theme_font_size_override("font_size", 22)
	text.add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.6))
	text.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	text.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	text.set_anchors_preset(Control.PRESET_FULL_RECT)
	prompt.add_child(text)
	# the game's logo, in the bottom bar
	logo = TextureRect.new()
	logo.anchor_left = 0.36
	logo.anchor_right = 0.64
	logo.anchor_top = 0.85
	logo.anchor_bottom = 0.975
	logo.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	logo.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	logo.flip_v = true   # the UI's textures are stored bottom row first
	logo.mouse_filter = Control.MOUSE_FILTER_IGNORE
	logo.visible = false
	ui.add_child(logo)
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
	manifest_edit = LineEdit.new()
	manifest_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	manifest_edit.placeholder_text = "oid_manifest.txt"
	row.add_child(manifest_edit)
	var b := Button.new()
	b.text = "Load"
	b.pressed.connect(func():
		if folder_edit.text.strip_edges() == "":
			_locate()
		else:
			_load(folder_edit.text.strip_edges(), manifest_edit.text.strip_edges()))
	row.add_child(b)
	row.name = "FolderRow"
	ui.add_child(row)


func _load(folder: String, manifest: String) -> void:
	status.text = "Reading the game files..."
	await get_tree().process_frame
	await get_tree().process_frame
	var g := InaGame.new()
	var err := g.open(folder, manifest)
	if err != "":
		status.text = err
		return
	game = g
	var c := ConfigFile.new()
	c.set_value("files", "folder", folder)
	c.set_value("files", "manifest", manifest)
	c.save(SETTINGS)
	ui.get_node("FolderRow").visible = false

	# the boot movie (publisher and studio logos), while the city is built behind it
	var splash: BinkPlayer = null
	if shot_path == "" and FileAccess.file_exists(folder + "/movies/cutscene/splash_europe.bik"):
		splash = BinkPlayer.new()
		add_child(splash)
		var merr := splash.play(folder + "/movies/cutscene/splash_europe.bik")
		if merr != "":
			print(merr)
			splash.queue_free()
			splash = null

	var shot := game.intro_camera("start", 0.0)
	if shot.is_empty():
		status.text = "The opening's camera was not found: " + game.last_log()
		return
	duration = shot["duration"]
	var anchor: Vector3 = shot["eye_game"]

	status.text = "Building the city (this takes a while the first time)..."
	await get_tree().process_frame
	# built on a worker thread so the boot movie keeps playing
	var out := []   # a lambda can't assign to a captured local, so the result goes in an array
	var task := WorkerThreadPool.add_task(func(): out.append(game.build_city(anchor.x, anchor.y, near_radius, 26000.0, true)))
	while not WorkerThreadPool.is_task_completed(task):
		await get_tree().process_frame
	WorkerThreadPool.wait_for_task_completion(task)
	add_child(out[0])
	if splash != null and is_instance_valid(splash) and not splash.done:
		status.text = ""
		await splash.finished
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
	var lt := game.texture("infamous_logo.psd", "msn_jb_intro.sprig.xpp")
	if lt != null:
		logo.texture = lt
		logo.visible = true
	var pt := game.texture("pause_item_selector.psd")
	if pt != null:
		plate.texture = pt
	prompt.visible = true
	if shot_path == "":
		# Title music and the park at night. Which cues the game plays here is not read from its data yet:
		# these are the files named for the start menu and for this park.
		music = _play_mp3(folder + "/sounds/music/647_start_select_menu_temp.mp3", -2.0)
		ambience = _play_mp3(folder + "/sounds/streaming_grains/8/i1_park_grnd_nite_nonpwrd.mp3", -10.0)
	frames_after_load = 0


func _make_world(tod: Dictionary) -> void:
	env = Environment.new()
	var haze: Color = tod.get("haze", Color(0.10, 0.17, 0.28))
	env.background_mode = Environment.BG_COLOR
	env.background_color = haze
	# the time of day's sky: a cube texture in its sky pack, drawn around the camera
	var sky_pack: String = tod.get("sky_pack", "")
	if sky_pack != "":
		var faces := game.cube_faces("cube_" + sky_pack + ".dds", sky_pack + ".xpp")
		if faces.size() == 6:
			var cm := Cubemap.new()
			cm.create_from_images(faces)
			var sh := Shader.new()
			sh.code = """
shader_type sky;
uniform samplerCube cube : source_color, filter_linear;
void sky() {
	// Godot's (x, y, z) is the game's (x, z, -y)
	vec3 d = vec3(EYEDIR.x, -EYEDIR.z, EYEDIR.y);
	COLOR = texture(cube, d).rgb * 0.45;   // undoes most of the scene's exposure (matched by eye)
}
"""
			var sm := ShaderMaterial.new()
			sm.shader = sh
			sm.set_shader_parameter("cube", cm)
			var sky := Sky.new()
			sky.sky_material = sm
			env.background_mode = Environment.BG_SKY
			env.sky = sky
			env.fog_sky_affect = 0.5   # the sky shows through the haze near the top of the reference
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	# the game lights in gamma space (texture x light, then shown); Godot lights in linear space, so the
	# game's light values are converted to keep the same result: light_linear = light ^ 2.2
	env.ambient_light_color = _lin(tod.get("ambient", Color(0.5, 0.5, 0.5)))
	env.ambient_light_energy = 1.0
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.tonemap_exposure = float(OS.get_environment("INA_EXPOSURE")) if OS.get_environment("INA_EXPOSURE") != "" else 2.2   # the game shows its scene brighter than its light values say (matched by eye)
	# haze: the game's start and end distances, as depth fog
	env.fog_enabled = true
	env.fog_mode = Environment.FOG_MODE_DEPTH
	env.fog_light_color = haze
	env.fog_depth_begin = tod.get("haze_start", 45.0)
	env.fog_depth_end = tod.get("haze_end", 100.0)
	env.fog_depth_curve = 1.0
	# the far city stays visible through the haze in the reference: the haze is capped (a guess, from the
	# time of day's middle value)
	env.fog_density = float(OS.get_environment("INA_FOG")) if OS.get_environment("INA_FOG") != "" else 0.6
	# glow around lamps and bright surfaces (strength is a guess, to be matched to the reference)
	env.glow_enabled = true
	env.glow_intensity = 0.8
	env.glow_bloom = 0.05
	env.glow_hdr_threshold = 0.9
	var we := WorldEnvironment.new()
	we.environment = env
	var attrs := CameraAttributesPractical.new()
	attrs.dof_blur_far_enabled = OS.get_environment("INA_DOF") == "1"   # the reference is sharp; off unless asked
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


func _play_mp3(path: String, db: float) -> AudioStreamPlayer:
	if not FileAccess.file_exists(path):
		print("missing ", path)
		return null
	var st := AudioStreamMP3.load_from_file(path)
	if st == null:
		return null
	st.loop = true
	var p := AudioStreamPlayer.new()
	p.stream = st
	p.volume_db = db
	add_child(p)
	p.play()
	return p


func _fade_out(p: AudioStreamPlayer, secs: float) -> void:
	if p == null:
		return
	var tw := create_tween()
	tw.tween_property(p, "volume_db", -60.0, secs)
	tw.tween_callback(p.queue_free)


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
		logo.visible = false
		_fade_out(music, 1.5)
		music = null


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
