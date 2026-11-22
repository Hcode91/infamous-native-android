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
var city_node: Node3D
var autostart := false
var opening_shots: Array = []   # opening times (s) to save frames at, for checks
var opening_t := -1.0
var folder_edit: LineEdit
var manifest_edit: LineEdit
var access_button: Button
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
	autostart = args.has("autostart")
	if args.has("shots"):
		for x in String(args["shots"]).split(","):
			opening_shots.append(float(x))
		autostart = true
	if args.has("folder"):
		var f: String = args["folder"]
		_load.call_deferred(f, args.get("manifest", f + "/oid_manifest.txt"))
		return
	_log("start: %s, %s, renderer %s" % [OS.get_name(), OS.get_model_name(), RenderingServer.get_current_rendering_method()])
	_log("plugin: " + ("loaded" if ClassDB.class_exists("InaGame") else "NOT LOADED"))
	if OS.get_name() == "Android":
		OS.request_permissions()
		_log("permissions asked; granted: " + str(OS.get_granted_permissions()))
	_wait_for_storage()


# All-files access is granted on a settings page outside the app, so keep checking until the shared
# storage can be read, then look for the files; look again every few seconds until both are found.
func _wait_for_storage() -> void:
	await get_tree().process_frame
	await get_tree().process_frame
	var said_waiting := false
	while game == null:
		var readable := DirAccess.open("/storage/emulated/0/Download") != null or OS.get_name() != "Android"
		if not readable:
			if not said_waiting:
				_log("shared storage not readable: allow 'All files access'")
				said_waiting = true
			status.text = "This app needs 'All files access' to read your game files.\nTap 'Allow access', turn it on, then come back."
			access_button.visible = true
			await get_tree().create_timer(1.0).timeout
			continue
		access_button.visible = false
		status.text = "Looking for the game files..."
		await get_tree().process_frame
		await get_tree().process_frame
		if await _locate():
			return
		await get_tree().create_timer(5.0).timeout


# A start-up log: on screen until the title shows, and in Download/INA-Godot-log.txt so it can be sent.
var log_lines: Array[String] = []
var log_label: Label
var log_t0 := Time.get_ticks_msec()

func _log(msg: String) -> void:
	var line := "%6.1fs  %s" % [(Time.get_ticks_msec() - log_t0) / 1000.0, msg]
	print(line)
	log_lines.append(line)
	if log_label == null:
		var layer := CanvasLayer.new()
		layer.layer = 100
		add_child(layer)
		log_label = Label.new()
		log_label.position = Vector2(8, 8)
		log_label.add_theme_font_size_override("font_size", 14)
		log_label.add_theme_constant_override("outline_size", 4)
		log_label.add_theme_color_override("font_outline_color", Color.BLACK)
		log_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
		layer.add_child(log_label)
	log_label.text = "\n".join(log_lines.slice(max(0, log_lines.size() - 14)))
	for path in ["/storage/emulated/0/Download/INA-Godot-log.txt", "user://INA-Godot-log.txt"]:
		var f := FileAccess.open(path, FileAccess.WRITE)
		if f != null:
			f.store_string("\n".join(log_lines) + "\n")
			f.close()
			break


# Finds the game's USRDIR (holding infamous1.psarc_s) and oid_manifest.txt: the saved places first, then
# the usual folders of a phone, a few levels deep.
func _locate() -> bool:
	if game != null:
		return true
	var c := ConfigFile.new()
	var folder := ""
	var manifest := ""
	if c.load(SETTINGS) == OK:
		folder = c.get_value("files", "folder", "")
		manifest = c.get_value("files", "manifest", "")
	if folder == "" or not FileAccess.file_exists(folder + "/infamous1.psarc_s"):
		folder = ""
		for r in ["/storage/emulated/0/Download", "/storage/emulated/0/Documents", "/storage/emulated/0"]:
			folder = _search(r, "infamous1.psarc_s", 7)
			if folder != "":
				break
	if manifest == "" or not FileAccess.file_exists(manifest):
		manifest = ""
		var places := [folder, folder.get_base_dir(), "/storage/emulated/0/Download", "/storage/emulated/0/Documents", "/storage/emulated/0"]
		for r in places:
			if r == "":
				continue
			var d := _search(r, "oid_manifest.txt", 6 if r != folder else 0)
			if d != "":
				manifest = d + "/oid_manifest.txt"
				break
	folder_edit.text = folder
	manifest_edit.text = manifest
	_log("game folder: " + (folder if folder != "" else "not found"))
	_log("manifest: " + (manifest if manifest != "" else "not found"))
	if folder != "" and manifest != "":
		_load(folder, manifest)
		return true
	status.text = "Game folder (with infamous1.psarc_s): %s\noid_manifest.txt: %s\nLooking again every few seconds." % [
		folder if folder != "" else "NOT FOUND", manifest if manifest != "" else "NOT FOUND"]
	return false


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
	access_button = Button.new()
	access_button.text = "Allow access"
	access_button.visible = false
	access_button.pressed.connect(func(): OS.request_permission("android.permission.MANAGE_EXTERNAL_STORAGE"))
	row.add_child(access_button)
	row.name = "FolderRow"
	ui.add_child(row)


func _load(folder: String, manifest: String) -> void:
	status.text = "Reading the game files..."
	await get_tree().process_frame
	await get_tree().process_frame
	_log("reading the archives...")
	var g := InaGame.new()
	var err := g.open(folder, manifest)
	if err != "":
		status.text = err
		_log("open failed: " + err)
		return
	_log("archives read")
	game = g
	game_folder = folder
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
		_log("splash: " + (merr if merr != "" else "playing"))
		if merr != "":
			splash.queue_free()
			splash = null

	var shot := game.intro_camera("start", 0.0)
	if shot.is_empty():
		status.text = "The opening's camera was not found: " + game.last_log()
		return
	duration = shot["duration"]
	var anchor: Vector3 = shot["eye_game"]

	status.text = "Building the city (this takes a while the first time)..."
	_log("building the city...")
	await get_tree().process_frame
	# built on a worker thread so the boot movie keeps playing
	var out := []   # a lambda can't assign to a captured local, so the result goes in an array
	var task := WorkerThreadPool.add_task(func(): out.append(game.build_city(anchor.x, anchor.y, near_radius, 26000.0, true)))
	while not WorkerThreadPool.is_task_completed(task):
		await get_tree().process_frame
	WorkerThreadPool.wait_for_task_completion(task)
	city_node = out[0]
	add_child(city_node)
	_log("city built: %d pieces; %s" % [city_node.get_child_count(), game.last_log().replace("\n", " | ").left(200)])
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
	_log("title showing")
	if log_label != null:
		var tw := create_tween()
		tw.tween_interval(4.0)
		tw.tween_property(log_label, "modulate:a", 0.0, 1.0)
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
	if autostart:
		shot_path = ""
		_start_opening()


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
	if free_look:
		_look_input(event)
		return
	if game == null or cam == null or playing:
		return
	var start: bool = (event is InputEventScreenTouch and event.pressed) or (event is InputEventMouseButton and event.pressed) \
		or (event is InputEventJoypadButton and event.pressed) or (event is InputEventKey and event.pressed)
	if start:
		_start_opening()


func _process(delta: float) -> void:
	if frames_after_load >= 0 and shot_path != "":
		frames_after_load += 1
		if frames_after_load == 30:
			var img := get_viewport().get_texture().get_image()
			img.save_png(shot_path)
			print("saved ", shot_path)
			get_tree().quit()
		return
	if opening_t >= 0.0:
		opening_t += delta
		if not opening_shots.is_empty() and opening_t >= opening_shots[0]:
			var at: float = opening_shots.pop_front()
			get_viewport().get_texture().get_image().save_png("/tmp/op_%05.1f.png" % at)
			print("saved opening frame at ", at)
			if opening_shots.is_empty():
				get_tree().quit()
	if dome != null:
		_grow_dome(delta)
	if free_look:
		_look_process(delta)
	if playing and cam != null and clip_running:
		play_t += delta
		_place_camera("start", min(play_t, duration))
	elif prompt != null and prompt.visible:
		prompt.modulate.a = 0.6 + 0.4 * sin(Time.get_ticks_msec() / 500.0)


# ---------------------------------------------------------------------------------------------------------
# The opening after Start: the blast seen from the park, the logo card, the voices in the dark, then the
# crater and the first objectives. Values marked "data" come from the intro mission's property sets
# (fill_explode_world_bright, fill_explode_world_dying, fill_prologue_post_explode, todp_prologue_*);
# the timings are matched to the reference recording.

const GROUND_ZERO := Vector3(-32900.0, 17800.0, 0.0)   # the crater in sector c8 (game space, cm)
var dome: MeshInstance3D
var dome_r := 0.0
var dome_mat: ShaderMaterial
var clip_running := false
var overlay: ColorRect
var subtitle: Label
var brief: Label
var texts := {}
var free_look := false
var yaw := 0.0
var pitch := 0.0
var look_vel := Vector2.ZERO
var sounds: Array[AudioStreamPlayer] = []


func _g(p: Vector3) -> Vector3:   # game space (cm, z up) to Godot (m, y up)
	return Vector3(p.x, p.z, -p.y) * 0.01


func _wait(secs: float) -> void:
	await get_tree().create_timer(secs).timeout


func _sfx(rel: String, db := 0.0, loop := false) -> AudioStreamPlayer:
	var p := _play_mp3(game_folder + "/sounds/" + rel, db)
	if p != null:
		(p.stream as AudioStreamMP3).loop = loop
		sounds.append(p)
	return p


func _dialog_archive() -> String:
	for c in [game_folder + "/dialog/archive/english.psarc", game_folder.get_base_dir() + "/dialog/archive/english.psarc",
			game_folder.get_base_dir().get_base_dir() + "/dialog/archive/english.psarc"]:
		if FileAccess.file_exists(c):
			return c
	return ""


# One line of dialogue: its voice from the dialogue archive and its text as a subtitle.
func _say(id: String, pad := 0.4) -> void:
	subtitle.text = texts.get(id, "")
	var len := 2.5
	var arc := _dialog_archive()
	if arc != "":
		var bytes := game.archive_file(arc, "dialog/" + id + ".mp3")
		if bytes.size() > 0:
			var st := AudioStreamMP3.new()
			st.data = bytes
			var p := AudioStreamPlayer.new()
			p.stream = st
			add_child(p)
			p.play()
			p.finished.connect(p.queue_free)
			len = st.get_length()
	await _wait(len + pad)
	subtitle.text = ""


func _make_opening_ui() -> void:
	overlay = ColorRect.new()
	overlay.color = Color(1, 1, 1, 0)
	overlay.set_anchors_preset(Control.PRESET_FULL_RECT)
	overlay.mouse_filter = Control.MOUSE_FILTER_IGNORE
	ui.add_child(overlay)
	subtitle = Label.new()
	subtitle.anchor_left = 0.1
	subtitle.anchor_right = 0.9
	subtitle.anchor_top = 0.86
	subtitle.anchor_bottom = 0.98
	subtitle.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	subtitle.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	subtitle.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	subtitle.add_theme_font_size_override("font_size", 22)
	subtitle.add_theme_constant_override("outline_size", 6)
	subtitle.add_theme_color_override("font_outline_color", Color.BLACK)
	ui.add_child(subtitle)
	brief = Label.new()
	brief.anchor_left = 0.15
	brief.anchor_right = 0.85
	brief.anchor_top = 0.04
	brief.anchor_bottom = 0.14
	brief.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	brief.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	brief.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	brief.add_theme_font_size_override("font_size", 24)
	brief.add_theme_constant_override("outline_size", 6)
	brief.add_theme_color_override("font_outline_color", Color.BLACK)
	ui.add_child(brief)


func _fade(to_alpha: float, secs: float, color := Color.BLACK) -> void:
	overlay.color = Color(color.r, color.g, color.b, overlay.color.a)
	var tw := create_tween()
	tw.tween_property(overlay, "color:a", to_alpha, secs)
	await tw.finished


func _make_dome() -> void:
	# the Ray Sphere's blast: a hemisphere of electric light growing out of ground zero
	dome = MeshInstance3D.new()
	var m := SphereMesh.new()
	m.radius = 1.0
	m.height = 2.0
	m.is_hemisphere = true
	m.radial_segments = 48
	m.rings = 24
	dome.mesh = m
	var sh := Shader.new()
	sh.code = """
shader_type spatial;
render_mode unshaded, cull_disabled, blend_add, depth_draw_never, fog_disabled;
uniform sampler2D noise : filter_linear_mipmap, repeat_enable;
uniform float glow = 1.0;
void fragment() {
	vec2 uv = UV * vec2(6.0, 3.0);
	float n = texture(noise, uv + vec2(TIME * 0.05, -TIME * 0.11)).r;
	float n2 = texture(noise, uv * 1.7 + vec2(-TIME * 0.07, TIME * 0.04)).r;
	float rim = pow(1.0 - abs(dot(NORMAL, VIEW)), 2.0);
	float v = smoothstep(0.35, 0.8, n * n2 * 2.0) + rim * 0.8;
	ALBEDO = mix(vec3(0.12, 0.35, 0.9), vec3(0.7, 0.85, 1.0), v) * (0.25 + 0.6 * v) * glow;
}
"""
	dome_mat = ShaderMaterial.new()
	dome_mat.shader = sh
	var nt := game.texture("JB_noise_01.psd", "msn_jb_intro.sprig.xpp")
	if nt != null:
		dome_mat.set_shader_parameter("noise", nt)
	dome.material_override = dome_mat
	dome.position = _g(GROUND_ZERO)
	dome.scale = Vector3.ONE * 0.01
	add_child(dome)
	dome_r = 1.0


func _grow_dome(delta: float) -> void:
	# fast at first, then slowing: about 300 m across by the end of the blast shots
	dome_r += delta * max(3.0, 14.0 - dome_r * 0.05)
	dome.scale = Vector3.ONE * dome_r


func _start_opening() -> void:
	if playing:
		return
	playing = true
	_run_opening()


func _run_opening() -> void:
	opening_t = 0.0
	texts = game.strings("english")
	_make_opening_ui()
	prompt.visible = false
	logo.visible = false
	_fade_out(music, 1.0)
	music = null
	play_t = 0.0
	clip_running = true
	# the city's power dies (todp_prologue_startup: ambient and sky brightness 0)
	_sfx("streaming_grains/4/jbintro_electroblast_v1.mp3")
	var tw := create_tween().set_parallel()
	for l in city_node.get_children():
		if l is OmniLight3D:
			tw.tween_property(l, "light_energy", 0.0, 0.4)
	tw.tween_property(env, "ambient_light_energy", 0.3, 0.6)
	tw.tween_property(env, "tonemap_exposure", 1.4, 0.6)
	_make_dome()
	await _wait(1.0)
	# the blast: fill_explode_world_bright (white, at once) then fill_explode_world_dying (fading over 5 s)
	_sfx("streaming_grains/4/jbintro_blastinitial.mp3")
	overlay.color = Color(1, 1, 1, 1)
	var glow0 := env.glow_intensity
	env.glow_intensity = 3.0
	var tw2 := create_tween().set_parallel()
	tw2.tween_property(overlay, "color:a", 0.0, 1.6).set_ease(Tween.EASE_IN)
	tw2.tween_property(env, "glow_intensity", glow0, 5.0)
	# fill_prologue_post_explode (data): fog (30, 30, 43) from 100 to 700 m at 0.8, colours desaturated
	env.fog_light_color = Color8(30, 30, 43)
	env.fog_depth_begin = 100.0
	env.fog_depth_end = 700.0
	env.fog_density = 0.8
	env.adjustment_enabled = true
	env.adjustment_saturation = 0.5
	if ambience != null:
		_fade_out(ambience, 2.0)
		ambience = null
	await _wait(3.0)
	_sfx("streaming_grains/4/jbintro_blastwall_no_screams_v2.mp3", -2.0)
	while play_t < duration + 3.0:
		await get_tree().process_frame
	clip_running = false
	await _fade(1.0, 1.0)   # fill_fade_to_black (data: 1 s)
	for p in sounds:
		if is_instance_valid(p):
			_fade_out(p, 1.5)
	sounds.clear()
	dome.queue_free()
	dome = null
	logo.visible = false

	# the logo card, while the crater is built
	remove_child(city_node)
	city_node.queue_free()
	var out := []
	var task := WorkerThreadPool.add_task(func(): out.append(game.build_city(GROUND_ZERO.x, GROUND_ZERO.y, near_radius, 26000.0, true, true)))
	var logo_path := game_folder + "/movies/cutscene/infamous_logo.bik"
	if FileAccess.file_exists(logo_path):
		var card := BinkPlayer.new()
		add_child(card)
		if card.play(logo_path, -1) == "":
			card.skippable = false
			_sfx("streaming_grains/4/jbintro_blastwave.mp3")
			await card.finished
		else:
			card.queue_free()
	# in the dark: Kessler's voice over the radio (m_jbintro_intro_01-03)
	await _wait(1.0)
	for id in ["m_jbintro_intro_01_kes", "m_jbintro_intro_02_kes", "m_jbintro_intro_03_kes"]:
		await _say(id)
	while not WorkerThreadPool.is_task_completed(task):
		await get_tree().process_frame
	WorkerThreadPool.wait_for_task_completion(task)
	city_node = out[0]
	add_child(city_node)
	_make_crater_world()
	await _place_in_crater()
	_sfx("streaming_grains/3/jbintro_gz_amb.mp3", -4.0, true)
	await _fade(0.0, 3.0)   # fill_fade_from_black (data: 3 s)
	await _say("m_jbintro_intro_04_hro")
	await _say("m_jbintro_intro_05_hro")

	# the first objective: look up at the news chopper
	free_look = true
	_sfx("streaming_grains/3/raysphereloop.mp3", -14.0, true)
	await _say("m_jbintro_crater_01_brd")
	brief.text = texts.get("u_brief_jbintro_right_analog_stick_up", "")
	while pitch < deg_to_rad(30.0):
		if opening_shots.size() > 0 and opening_t > 60.0:
			break   # unattended check run: carry on
		await get_tree().process_frame
	brief.text = ""
	for id in ["m_jbintro_crater_02_brd", "m_jbintro_crater_03_brd", "m_jbintro_crater_04_hro"]:
		await _say(id)
	await _say("m_pickups_jbintro_01_hro")
	brief.text = texts.get("u_brief_jbintro_bridge", "")


func _make_crater_world() -> void:
	# after the blast: the intro's own time of day (fires, orange haze) and fill_prologue_post_explode
	for c in get_children():
		if c is WorldEnvironment or c is DirectionalLight3D:
			c.queue_free()
	var tod := game.time_of_day("jbintro")
	_make_world(tod)
	env.tonemap_exposure = 1.6
	env.fog_light_color = Color8(30, 30, 43).lerp(tod.get("haze", Color(0.2, 0.1, 0.04)), 0.5)
	env.fog_depth_begin = 10.0
	env.fog_depth_end = 250.0
	env.fog_density = 0.8
	env.adjustment_enabled = true
	env.adjustment_saturation = 0.65


func _place_in_crater() -> void:
	# Somewhere open on the crater floor near ground zero, from the built geometry: collision for the pieces
	# around it, then rays down to the floor and up to the sky (the wreckage leaves pockets roofed over).
	var at := _g(GROUND_ZERO)
	var area := AABB(at - Vector3(45, 40, 45), Vector3(90, 60, 90))
	for mi in city_node.get_children():
		if mi is MeshInstance3D:
			var box: AABB = mi.get_aabb()
			if box.intersects(area):
				mi.create_trimesh_collision()
	await get_tree().physics_frame
	await get_tree().physics_frame
	var space := get_world_3d().direct_space_state
	var best := Vector3.INF
	var best_score := INF
	for ix in range(-8, 9):
		for iz in range(-8, 9):
			var p := at + Vector3(ix * 5.0, 0, iz * 5.0)
			var down := space.intersect_ray(PhysicsRayQueryParameters3D.create(p + Vector3(0, -2, 0), p + Vector3(0, -40, 0)))
			if not down.has("position"):
				continue
			var f: Vector3 = down["position"]
			if down["normal"].y < 0.7:
				continue
			var up := space.intersect_ray(PhysicsRayQueryParameters3D.create(f + Vector3(0, 0.3, 0), f + Vector3(0, 40, 0)))
			if up.has("position"):
				continue
			# room to look around: nothing within 8 m at eye height
			var open := true
			for k in 8:
				var d := Vector3(cos(k * PI / 4.0), 0, sin(k * PI / 4.0)) * 8.0
				var e := f + Vector3(0, 1.7, 0)
				if space.intersect_ray(PhysicsRayQueryParameters3D.create(e, e + d)).has("position"):
					open = false
					break
			if not open:
				continue
			var score := Vector2(ix, iz).length() + f.y * 0.2   # near the centre, then low
			if score < best_score:
				best_score = score
				best = f
	if best == Vector3.INF:
		best = at + Vector3(0, -7, 0)
	cam.global_position = best + Vector3(0, 1.7, 0)
	# face ground zero's centre, slightly down
	var to := at - best
	yaw = atan2(-to.x, -to.z) if Vector2(to.x, to.z).length() > 1.0 else 0.0
	pitch = -0.1
	cam.fov = 60.0
	cam.rotation = Vector3(pitch, yaw, 0)
	print("crater floor at ", best)


func _look_input(event: InputEvent) -> void:
	if event is InputEventScreenDrag:
		look_vel = Vector2.ZERO
		yaw -= event.relative.x * 0.004
		pitch = clamp(pitch - event.relative.y * 0.004, -1.2, 1.3)
	elif event is InputEventMouseMotion and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT):
		yaw -= event.relative.x * 0.004
		pitch = clamp(pitch - event.relative.y * 0.004, -1.2, 1.3)


func _look_process(delta: float) -> void:
	# the right stick, as in the game's prompt
	var stick := Vector2(Input.get_joy_axis(0, JOY_AXIS_RIGHT_X), Input.get_joy_axis(0, JOY_AXIS_RIGHT_Y))
	if stick.length() > 0.15:
		yaw -= stick.x * 2.0 * delta
		pitch = clamp(pitch - stick.y * 1.6 * delta, -1.2, 1.3)
	if opening_shots.size() > 0:
		pitch = min(pitch + 0.4 * delta, 0.7)   # unattended check run: look up on its own
	cam.rotation = Vector3(pitch, yaw, 0)
