class_name UiLayer
extends Node2D
# One of the game's interface screens (an "Uno" scene of the intro pack): its textured quads hung on an animated
# rig and its text boxes in the game's font, all placed by the scene's own data in a 1280x720 screen (origin in
# the middle, y up). play() runs one of its clips (clip_in, clip_on, clip_off, ...).

var game: InaGame
var scene_name := ""
var data := {}
var parts: Array = []      # [Node2D holder, joint] for elements and text boxes
var anim := ""
var clip := ""
var t := 0.0
var duration := 0.0
var then_clip := ""        # played after the current one ends (clip_in -> clip_on)
var loop := false
var labels := {}           # text box name -> Label

static var font_cache: FontFile


func setup(g: InaGame, name: String) -> bool:
	game = g
	scene_name = name
	data = game.ui_scene(name)
	if data.is_empty() or data["elements"].is_empty() and data["texts"].is_empty():
		return false
	for e in data["elements"]:
		var holder := Node2D.new()
		var poly := Polygon2D.new()
		var r: Rect2 = e["rect"]
		var o: Vector2 = e["offset"]
		# the quad in the joint's space, y up -> Godot's y down
		var pts := PackedVector2Array([Vector2(o.x + r.position.x, -(o.y + r.end.y)), Vector2(o.x + r.end.x, -(o.y + r.end.y)),
			Vector2(o.x + r.end.x, -(o.y + r.position.y)), Vector2(o.x + r.position.x, -(o.y + r.position.y))])
		poly.polygon = pts
		var tex: Texture2D = e["texture"]
		if tex != null:
			poly.texture = tex
			var s := tex.get_size()
			# the interface's textures are stored bottom row first
			poly.uv = PackedVector2Array([Vector2(0, s.y), Vector2(s.x, s.y), Vector2(s.x, 0), Vector2(0, 0)])
		else:
			poly.color = Color.BLACK   # ui_color: a flat colour
		holder.add_child(poly)
		add_child(holder)
		parts.append([holder, int(e["joint"])])
	for tb in data["texts"]:
		var holder := Node2D.new()
		var lab := Label.new()
		var size: Vector2 = tb["size"]
		var off: Vector2 = tb["offset"]
		lab.size = size
		lab.position = Vector2(off.x - size.x * 0.5, -off.y - size.y * 0.5)
		lab.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		lab.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		lab.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		var f := game_font(g)
		if f != null:
			lab.add_theme_font_override("font", f)
		lab.add_theme_font_size_override("font_size", int(tb["font_size"]))
		# TEXT_EFFECT 1 with HALO_WIDTH 2.3: a dark halo round the letters
		lab.add_theme_constant_override("outline_size", 5)
		lab.add_theme_color_override("font_outline_color", Color(0, 0, 0, 0.8))
		holder.add_child(lab)
		add_child(holder)
		parts.append([holder, int(tb["joint"])])
		labels[tb["name"]] = lab
	return true


# The game's main font (font_main_latin of the language pack) as a Godot bitmap font.
static func game_font(g: InaGame) -> FontFile:
	if font_cache != null:
		return font_cache
	var fd := g.ui_font("font_main_latin", "english")
	if fd.is_empty() or fd["texture"] == null:
		return null
	var img: Image = (fd["texture"] as Texture2D).get_image()
	img.decompress()
	img.flip_y()   # stored bottom row first, like the interface's other textures
	var line: int = fd["line"]
	var ff := FontFile.new()
	var size := Vector2i(line, 0)
	ff.fixed_size = line
	ff.set_texture_image(0, size, 0, img)
	var ascent := 80.0   # the space glyph's top sits at 80: the baseline
	ff.set_cache_ascent(0, line, ascent)
	ff.set_cache_descent(0, line, line - ascent)
	for gl in fd["glyphs"]:
		var code: int = gl[0]
		ff.set_glyph_texture_idx(0, size, code, 0)
		ff.set_glyph_uv_rect(0, size, code, Rect2(gl[1], gl[2], gl[3], gl[4]))
		ff.set_glyph_size(0, size, code, Vector2(gl[3], gl[4]))
		ff.set_glyph_offset(0, size, code, Vector2(gl[5], gl[6] - ascent))
		ff.set_glyph_advance(0, line, code, Vector2(gl[7], 0))
	font_cache = ff
	return ff


func set_text(box: String, text: String) -> void:
	if labels.has(box):
		labels[box].text = text


func play(anim_name: String, clip_name: String, next_clip := "", looping := false) -> void:
	anim = anim_name
	clip = clip_name
	then_clip = next_clip
	loop = looping
	t = 0.0
	visible = true
	_pose()


func _process(delta: float) -> void:
	if clip == "":
		return
	t += delta
	if t >= duration:
		if then_clip != "":
			clip = then_clip
			then_clip = ""
			loop = true
			t = 0.0
		elif loop and duration > 0.0:
			t = fmod(t, duration)
		else:
			t = duration
	_pose()


func _pose() -> void:
	var p := game.ui_pose(scene_name, anim, clip, t)
	if p.is_empty():
		return
	duration = p["duration"]
	var j: PackedFloat32Array = p["joints"]
	for part in parts:
		var k: int = part[1] * 6
		if k + 5 >= j.size():
			continue
		var h: Node2D = part[0]
		h.position = Vector2(640.0 + j[k + 3], 360.0 - j[k + 4])
		h.rotation = -j[k + 2]
		h.scale = Vector2(j[k], j[k + 1])
		h.z_index = clampi(int(j[k + 5]), -100, 100)
