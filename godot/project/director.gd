class_name Director
extends Node
# Runs a mission of the game the way the game does: its own script (in the plugin's script machine) decides
# what happens and when; this node shows what the script asks for. Everything shown comes from the game's
# files: the camera files' clips, the fills' and times of day's property sets, the spawned objects' models,
# the script objects' geometry, voices and text. What the script asks for that is not shown yet is written to
# the start-up log, so the gaps are visible.

signal mission_finished(entry: String)

var main: Node3D          # the scene (main.gd): game, cam, env, ui, city
var game: InaGame
var entry := ""
var pressed := false      # the start button

# camera playback
var cam_object := ""      # the camera object playing (or resting on its first frame)
var cam_file := ""
var cam_clip := ""
var cam_t := 0.0
var cam_len := 0.0
var cam_events: Array = []
var cam_event_i := 0
var cam_playing := false
var cam_anchor_override = null   # Vector3 (game cm) when the camera file's own anchor is not where it plays

# what is on screen
var props := {}           # handle -> Node3D
var prop_models := {}     # model name -> [intact, wrecked] template nodes
var objects := {}         # script object name -> {node, pivot}
var shockwaves: Array = []    # {node, t, time, fn}
var props_cache := {}     # object name -> property set
var overlay: ColorRect
var mul_overlay: ColorRect
var subtitle: Label
var brief: Label
var unhandled_seen := {}
var base := {}            # the environment as the time of day made it
var lamps_on := true
var sounds := {}
var texts := {}
var shake_t := 0.0


func setup(scene: Node3D) -> void:
	main = scene
	game = scene.game
	texts = game.strings("english")
	var ui: CanvasLayer = scene.ui
	mul_overlay = ColorRect.new()
	mul_overlay.set_anchors_preset(Control.PRESET_FULL_RECT)
	mul_overlay.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var mm := CanvasItemMaterial.new()
	mm.blend_mode = CanvasItemMaterial.BLEND_MODE_MUL
	mul_overlay.material = mm
	mul_overlay.color = Color.WHITE
	ui.add_child(mul_overlay)
	overlay = ColorRect.new()
	overlay.set_anchors_preset(Control.PRESET_FULL_RECT)
	overlay.mouse_filter = Control.MOUSE_FILTER_IGNORE
	overlay.color = Color(0, 0, 0, 0)
	ui.add_child(overlay)
	subtitle = _label(ui, 0.1, 0.9, 0.86, 0.98, 22)
	brief = _label(ui, 0.15, 0.85, 0.04, 0.14, 24)
	_capture_base()


func _label(ui: CanvasLayer, l: float, r: float, t: float, b: float, size: int) -> Label:
	var x := Label.new()
	x.anchor_left = l
	x.anchor_right = r
	x.anchor_top = t
	x.anchor_bottom = b
	x.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	x.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	x.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	x.add_theme_font_size_override("font_size", size)
	x.add_theme_constant_override("outline_size", 6)
	x.add_theme_color_override("font_outline_color", Color.BLACK)
	x.mouse_filter = Control.MOUSE_FILTER_IGNORE
	ui.add_child(x)
	return x


func _capture_base() -> void:
	var env: Environment = main.env
	base = {
		"ambient": env.ambient_light_energy, "fog_begin": env.fog_depth_begin, "fog_end": env.fog_depth_end,
		"fog_density": env.fog_density, "fog_color": env.fog_light_color, "glow": env.glow_intensity,
		"exposure": env.tonemap_exposure, "sky": env.background_energy_multiplier,
	}


func start(mission_entry: String) -> String:
	entry = mission_entry
	var err := game.mission_start("msn_jb_intro.sprig.xpp", entry)
	main._log("mission " + entry + ": " + (err if err != "" else "running"))
	return err


func _process(delta: float) -> void:
	if game == null or entry == "":
		return
	game.mission_update(delta, pressed)
	for e in game.mission_events():
		_event(e)
	for u in game.mission_unhandled():
		var key: String = u.left(48)
		if not unhandled_seen.has(key) and unhandled_seen.size() < 400:
			unhandled_seen[key] = true
			main._log("script asks (not shown yet): " + u.left(160))
	var st := game.mission_state()
	_camera(delta, st)
	_screen(st)
	_props()
	_objects()
	_shockwaves(delta)
	main.prompt.visible = st.get("prompt", false)
	main.logo.visible = st.get("prompt", false) or (cam_object == "" and entry == "RunCheckpointPrologue" and not pressed)
	if st.get("finished", false):
		var done := entry
		entry = ""
		main._log("mission " + done + " finished")
		mission_finished.emit(done)


# ---- events -------------------------------------------------------------------------------------------

func _event(e: Dictionary) -> void:
	var kind: String = e["kind"]
	var name: String = e["name"]
	match kind:
		"camera_ready":
			_use_camera(name, false)
		"camera":
			_use_camera(name, true)
		"spawn":
			_spawn(int(e["value"]), name)
		"show":
			_show_object(name, e["detail"])
		"hide":
			if objects.has(name):
				objects[name]["node"].visible = false
		"shockwave":
			_shockwave(int(e["value"]), name)
		"power":
			_power(e["value"] > 0.5)
		"music":
			_music(name)
		"sound":
			_sound(name)
		"voice":
			_voice(name)
		"text":
			brief.text = texts.get(name, "")
		"ui_off":
			if name == "uno_controller_prompt":
				brief.text = ""
		"particles":
			main._log("particle effect (not drawn yet): %s at %s" % [name, e["detail"]])
		_:
			pass


func _use_camera(object: String, play: bool) -> void:
	var file := game.mission_file_for(object)
	if file == "":
		main._log("camera without a file: " + object)
		game.mission_camera_done(object)
		return
	cam_object = object
	cam_file = file
	cam_anchor_override = null
	# the crater reveal's file has no anchor of its own: it plays where the player wakes up
	if file == "jb_intro_camera_2b":
		var w = game.mission_object("warp_cp_intro")
		if w != null:
			cam_anchor_override = w
	if not play:
		cam_clip = "init"
		var r := game.camera_clip(file, "init", 0.0)
		if r.is_empty():
			cam_clip = ""
		cam_playing = false
		cam_t = 0.0
		_place(0.0)
		return
	# the clip the camera's file plays: "start" for the first camera, otherwise its first clip that is not "init"
	cam_clip = "start" if not game.camera_clip(file, "start", 0.0).is_empty() else ""
	var r := game.camera_clip(file, cam_clip, 0.0)
	if r.is_empty():
		main._log("camera %s: no clip in %s" % [object, file])
		game.mission_camera_done(object)
		return
	cam_len = r["duration"]
	cam_events = r["events"]
	cam_event_i = 0
	cam_t = 0.0
	cam_playing = true
	main._log("camera %s plays %s/%s (%.1f s)" % [object, file, cam_clip, cam_len])


func _camera(delta: float, st: Dictionary) -> void:
	if cam_object == "":
		return
	if cam_playing:
		cam_t += delta
		while cam_event_i < cam_events.size() and cam_events[cam_event_i]["time"] <= cam_t:
			game.mission_label(cam_events[cam_event_i]["name"])
			cam_event_i += 1
		if cam_t >= cam_len:
			cam_playing = false
			cam_t = cam_len
			game.mission_camera_done(cam_object)
	_place(cam_t)
	# the script's camera shake: amplitudes in radians; the waveform is this engine's (two sines per axis)
	var sh: Vector3 = st.get("shake", Vector3.ZERO)
	if sh != Vector3.ZERO:
		shake_t += delta
		var t := shake_t
		main.cam.rotate_object_local(Vector3.UP, sh.x * (sin(t * 23.0) + 0.5 * sin(t * 37.0)))
		main.cam.rotate_object_local(Vector3.RIGHT, sh.y * (sin(t * 29.0) + 0.5 * sin(t * 41.0)))
		main.cam.rotate_object_local(Vector3.BACK, sh.z * (sin(t * 31.0) + 0.5 * sin(t * 43.0)))


func _place(t: float) -> void:
	var s := game.camera_clip(cam_file, cam_clip, min(t, max(cam_len - 0.01, 0.0)) if cam_playing or t > 0.0 else 0.0)
	if s.is_empty():
		return
	var eye: Vector3 = s["eye"]
	if cam_anchor_override != null:
		var a: Vector3 = s["anchor"]
		var o: Vector3 = cam_anchor_override
		eye += Vector3(o.x - a.x, o.z - a.z, -(o.y - a.y)) * 0.01   # game cm (z up) to Godot metres
	var basis := Basis.looking_at(s["forward"], s["up"])
	var parent := game.mission_camera_parent(cam_object)
	if not parent.is_empty():   # riding on something (the helicopter): the clip is relative to it
		var pb := Basis(parent["rotation"])
		eye = parent["position"] + pb * (eye - _g_vec(s["anchor"]))
		basis = pb * basis
	var fov: float = s["fov_rig"]
	if fov < 6.0:
		fov = 50.0   # the rig's angle joint is not animated in this clip
	main.cam.fov = fov
	main.cam.keep_aspect = Camera3D.KEEP_HEIGHT
	main.cam.global_transform = Transform3D(basis, eye)


func _g_vec(v: Vector3) -> Vector3:   # game cm (z up) to Godot metres
	return Vector3(v.x, v.z, -v.y) * 0.01


# ---- fills and times of day ------------------------------------------------------------------------------

func _props_of(name: String) -> Dictionary:
	if not props_cache.has(name):
		props_cache[name] = game.mission_properties(name)
	return props_cache[name]


func _screen(st: Dictionary) -> void:
	var env: Environment = main.env
	# the fills' overlay and added bloom, composited by the plugin
	overlay.color = st.get("cover", Color(0, 0, 0, 0))
	var ambient: float = base["ambient"]
	var sky: float = base["sky"]
	var fog_begin: float = base["fog_begin"]
	var fog_end: float = base["fog_end"]
	var fog_density: float = base["fog_density"]
	var fog_color: Color = base["fog_color"]
	var sat := 1.0
	var mul := Color.WHITE
	# times of day blended in by the script (TimeOfDayParams)
	for tod in st.get("tods", []):
		var p := _props_of(tod["name"])
		var l: float = tod["lerp"]
		if l <= 0.0:
			continue
		if p.has("TimeOfDayParams.AMBIENT_BRIGHTNESS"):
			ambient = lerp(ambient, ambient * float(p["TimeOfDayParams.AMBIENT_BRIGHTNESS"]), l)
		if p.has("TimeOfDayParams.AMBIENT_MULTIPLIER"):
			ambient = lerp(ambient, ambient * float(p["TimeOfDayParams.AMBIENT_MULTIPLIER"]), l)
		if p.has("TimeOfDayParams.SKY_BRIGHTNESS"):
			sky = lerp(sky, sky * float(p["TimeOfDayParams.SKY_BRIGHTNESS"]), l)
		if p.has("TimeOfDayParams.HAZE_DISTANCE"):
			var h: Vector4 = p["TimeOfDayParams.HAZE_DISTANCE"]
			fog_begin = lerp(fog_begin, h.x * 0.01, l)
			fog_end = lerp(fog_end, h.y * 0.01, l)
	# the fills' other settings, by how far each has faded in
	var fills: Dictionary = st.get("fills", {})
	for f in fills:
		var p := _props_of(f)
		var l: float = fills[f]
		if p.has("Fill.FOG_OPACITY"):
			var c: Vector4 = p.get("Fill.FOG_COLOR", Vector4())
			var d: Vector4 = p.get("Fill.FOG_DISTANCE_LIMITS", Vector4(0, 10000, 0, 0))
			fog_color = fog_color.lerp(main._lin(Color(c.x / 255.0, c.y / 255.0, c.z / 255.0)), l)
			fog_begin = lerp(fog_begin, d.x * 0.01, l)
			fog_end = lerp(fog_end, d.y * 0.01, l)
			fog_density = lerp(fog_density, float(p["Fill.FOG_OPACITY"]), l)
		if p.has("Fill.DESAT_LIMITS"):
			var dl: Vector4 = p["Fill.DESAT_LIMITS"]
			sat = min(sat, 1.0 - dl.y * l)
		if p.has("Fill.MULTIPLY_BLOOM_COLOR"):
			var mc: Vector4 = p["Fill.MULTIPLY_BLOOM_COLOR"]
			mul = mul * Color.WHITE.lerp(Color(mc.x / 255.0, mc.y / 255.0, mc.z / 255.0), l * float(p.get("Fill.MULTIPLY_BLOOM", 1.0)))
	env.ambient_light_energy = ambient
	env.background_energy_multiplier = sky
	env.fog_depth_begin = fog_begin
	env.fog_depth_end = max(fog_end, fog_begin + 1.0)
	env.fog_density = clamp(fog_density, 0.0, 1.0)
	env.fog_light_color = fog_color
	env.adjustment_enabled = sat < 0.999
	env.adjustment_saturation = sat
	mul_overlay.color = mul


func _power(on: bool) -> void:
	# a district's power: the street lamps go out
	lamps_on = on
	if main.city_node == null:
		return
	for l in main.city_node.get_children():
		if l is OmniLight3D:
			l.visible = on


# ---- spawned props and script objects ----------------------------------------------------------------

func _spawn(handle: int, model: String) -> void:
	if not prop_models.has(model):
		var n := game.mission_model(model, false)
		prop_models[model] = [n, null] if n.get_child_count() > 0 else [null, null]
	var tpl = prop_models[model][0]
	if tpl == null:
		return
	var inst: Node3D = tpl.duplicate()
	main.add_child(inst)
	props[handle] = {"node": inst, "model": model, "wrecked": false}


func _props() -> void:
	for p in game.mission_props():
		var h := int(p["handle"]) & 0xFFFFF
		if not props.has(h):
			continue
		var e: Dictionary = props[h]
		if p["wrecked"] and not e["wrecked"]:
			e["wrecked"] = true
			var m: String = e["model"]
			if prop_models[m][1] == null:
				prop_models[m][1] = game.mission_model(m, true)
			var w: Node3D = prop_models[m][1].duplicate()
			e["node"].queue_free()
			main.add_child(w)
			e["node"] = w
		# the models are built where their file put them (around the origin): place and turn them
		var n: Node3D = e["node"]
		n.transform = Transform3D(Basis(p["rotation"]), p["position"])


func _show_object(name: String, file: String) -> void:
	if file == "" or objects.has(name):
		if objects.has(name):
			objects[name]["node"].visible = true
		return
	var n := game.mission_file_node(file)
	if n.get_child_count() == 0:
		n.queue_free()
		return
	var pivot: Vector3 = n.get_meta("pivot", Vector3.ZERO)
	var holder := Node3D.new()
	holder.position = pivot
	n.position = -pivot
	holder.add_child(n)
	main.add_child(holder)
	objects[name] = {"node": holder, "pivot": pivot}
	main._log("showing %s (%s)" % [name, file])


func _objects() -> void:
	for o in game.mission_objects():
		var name: String = o["name"]
		if not objects.has(name):
			continue
		var holder: Node3D = objects[name]["node"]
		var s: Vector3 = o["scale"]
		if s.length() < 1e-4:
			holder.visible = false
			continue
		holder.visible = o["shown"]
		holder.scale = s
		holder.position = objects[name]["pivot"] + o["position"]


func _shockwave(handle: int, parent: String) -> void:
	# ShockwaveEffect: a sphere whose scale, alpha and colour follow the script's functions over its time.
	# Each function f(u) = a*u + b*u^2 + c*u^3 of the effect's progress u (0..1) (read from the values the
	# script sets: alpha 2,0,-2 rises and falls to 0; the exact curve form is an assumption).
	var w := game.mission_shockwave(handle)
	if w.is_empty() or not objects.has(parent):
		return
	var mi := MeshInstance3D.new()
	var m := SphereMesh.new()
	m.radius = 1.0
	m.height = 2.0
	mi.mesh = m
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mi.material_override = mat
	var holder: Node3D = objects[parent]["node"]
	mi.position = holder.position
	main.add_child(mi)
	shockwaves.append({"node": mi, "mat": mat, "t": 0.0, "w": w, "base": holder.scale.x * 34.08})


func _f(v: Vector4, u: float) -> float:
	return v.x * u + v.y * u * u + v.z * u * u * u


func _shockwaves(delta: float) -> void:
	for s in shockwaves.duplicate():
		s["t"] += delta
		var w: Dictionary = s["w"]
		var u: float = s["t"] / max(float(w["time"]), 0.01)
		if u >= 1.0:
			s["node"].queue_free()
			shockwaves.erase(s)
			continue
		var r: float = s["base"] * (1.0 + _f(w["scale_fn"], u) * 20.0)
		s["node"].scale = Vector3.ONE * r
		var cols: Array = w["colors"]
		var c: Color = cols[min(int(u * cols.size()), cols.size() - 1)] if cols.size() > 0 else Color.WHITE
		c.a = clamp(_f(w["alpha_fn"], u), 0.0, 1.0) * 0.5
		s["mat"].albedo_color = c


# ---- sound ------------------------------------------------------------------------------------------

func _music(playlist: String) -> void:
	# Playlists are defined in the shared pack's music tables, which are not read yet. A playlist whose name
	# matches music files by its mission's name plays those (jb_intro -> *jbintro*); others are only logged.
	if sounds.has("music"):
		main._fade_out(sounds["music"], 1.0)
		sounds.erase("music")
	if playlist == "" or playlist == "Unknown":
		return
	var key := playlist.replace("playlist_", "").replace("_", "")
	var dir := DirAccess.open(main.game_folder + "/sounds/music")
	if dir == null:
		return
	for f in dir.get_files():
		var plain := f.to_lower().replace("_", "")
		if f.ends_with(".mp3") and "jbintro" in key and "jbintro" in plain and "loop" in plain:
			sounds["music"] = main._play_mp3(main.game_folder + "/sounds/music/" + f, -4.0)
			main._log("music %s -> %s" % [playlist, f])
			return
	main._log("music (not mapped yet): " + playlist)


func _voice(id: String) -> void:
	# a line of dialogue: its voice from the dialogue archive and its text as the subtitle; the script waits
	subtitle.text = texts.get(id, "")
	var len := 2.5
	var arc: String = _dialog_archive()
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
	else:
		main._log("dialogue archive not found: voices are silent")
	await get_tree().create_timer(len + 0.3).timeout
	if subtitle.text == texts.get(id, ""):
		subtitle.text = ""
	game.mission_voice_done()


func _dialog_archive() -> String:
	var f: String = main.game_folder
	for c in [f + "/dialog/archive/english.psarc", f.get_base_dir() + "/dialog/archive/english.psarc",
			f.get_base_dir().get_base_dir() + "/dialog/archive/english.psarc"]:
		if FileAccess.file_exists(c):
			return c
	return ""


func _sound(cue: String) -> void:
	main._log("sound cue (not mapped yet): " + cue)
