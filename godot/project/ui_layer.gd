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
var hide_at_end := false   # clip_off: hidden once it has played
var labels := {}           # text box name -> Label

static var font_cache := {}   # pixel size -> FontFile


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
		var f := game_font(g, int(tb["font_size"]))
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


# The game's main font (font_main_latin of the language pack) as a Godot bitmap font at a pixel size: the
# glyph sheet (drawn at a line height of 100) is scaled down once to that size, so small text stays smooth.
static func game_font(g: InaGame, px: int = 28) -> FontFile:
	if font_cache.has(px):
		return font_cache[px]
	var fd := g.ui_font("font_main_latin", "english")
	if fd.is_empty() or fd["texture"] == null:
		return null
	var img: Image = (fd["texture"] as Texture2D).get_image()
	img.decompress()
	img.clear_mipmaps()
	img.flip_y()   # stored bottom row first, like the interface's other textures
	# the glyphs are a mask in the blue channel (alpha is empty): white letters shaped by that mask
	img.convert(Image.FORMAT_RGBA8)
	var w := img.get_width()
	var h := img.get_height()
	var data := img.get_data()
	for i in range(0, w * h * 4, 4):
		var m := data[i + 2]
		data[i] = 255
		data[i + 1] = 255
		data[i + 2] = 255
		data[i + 3] = m
	img = Image.create_from_data(w, h, false, Image.FORMAT_RGBA8, data)
	var line: int = fd["line"]
	var k := float(px) / float(line)
	img.resize(maxi(1, int(round(w * k))), maxi(1, int(round(h * k))), Image.INTERPOLATE_LANCZOS)
	var ff := FontFile.new()
	var size := Vector2i(px, 0)
	ff.fixed_size = px
	ff.set_texture_image(0, size, 0, img)
	var ascent := 80.0 * k   # the space glyph's top sits at 80: the baseline
	ff.set_cache_ascent(0, px, ascent)
	ff.set_cache_descent(0, px, px - ascent)
	for gl in fd["glyphs"]:
		var code: int = gl[0]
		var r := Rect2(gl[1] * k, gl[2] * k, gl[3] * k, gl[4] * k)
		ff.set_glyph_texture_idx(0, size, code, 0)
		ff.set_glyph_uv_rect(0, size, code, r)
		ff.set_glyph_size(0, size, code, r.size)
		ff.set_glyph_offset(0, size, code, Vector2(gl[5] * k, gl[6] * k - ascent))
		ff.set_glyph_advance(0, px, code, Vector2(gl[7] * k, 0))
	font_cache[px] = ff
	return ff


func set_text(box: String, text: String) -> void:
	if labels.has(box):
		labels[box].text = text


func play(anim_name: String, clip_name: String, next_clip := "", looping := false) -> void:
	anim = anim_name
	clip = clip_name
	then_clip = next_clip
	loop = looping
	hide_at_end = false
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
			if hide_at_end:
				visible = false
				clip = ""
				return
	_pose()


# Plays the screen's way out (its clip_off), then hides it.
func leave() -> void:
	if not visible:
		return
	var a: Array = data["anims"].get(anim, [])
	if a.has("clip_off"):
		play(anim, "clip_off")
		hide_at_end = true
	else:
		visible = false
		clip = ""


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
