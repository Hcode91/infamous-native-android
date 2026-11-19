package io.github.hcode91.ina

import android.app.Activity
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.DocumentsContract
import android.view.Gravity
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import java.nio.ByteBuffer
import kotlin.concurrent.thread

/**
 * Imports the user's own game files once (copied into app storage), then shows a title view of the city
 * with the game's logo and, on a tap, the player character under stick control. A development viewer
 * (textures, sectors, characters) sits behind the Viewer button. No game data ships with the app.
 */
class MainActivity : Activity(), SurfaceHolder.Callback {
    private val wanted = listOf("infamous1.psarc_s", "infamous2.psarc_s", "oid_manifest.txt")
    private val required = wanted.take(2)
    private val ui = Handler(Looper.getMainLooper())
    private lateinit var info: TextView
    private lateinit var stats: TextView
    private lateinit var importButton: Button
    private lateinit var controls: LinearLayout
    private var opened = false
    private var worldMode = false
    private var cityMode = false
    private lateinit var modeButton: Button
    private lateinit var cityButton: Button
    private lateinit var personButton: Button
    private lateinit var moveButton: Button
    private var personNote = ""
    private var gameMode = false          // title view or play: the viewer controls are hidden
    private var playing = false           // the player character is under stick control
    private lateinit var titleLayer: LinearLayout
    private lateinit var titleNote: TextView
    private lateinit var logoView: ImageView
    private lateinit var coverView: View
    private lateinit var gameBar: LinearLayout
    private lateinit var stickView: StickView
    private var stickId = -1              // pointer ids of the finger on the stick and the one turning the camera
    private var lookId = -1
    private var stickX0 = 0f
    private var stickY0 = 0f

    /** Draws the movement stick where the left thumb went down. */
    private class StickView(c: android.content.Context) : View(c) {
        var cx = 0f; var cy = 0f; var dx = 0f; var dy = 0f; var shown = false
        private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 4f; color = Color.argb(140, 255, 255, 255) }
        private val knob = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(120, 255, 255, 255) }
        var radius = 120f
        override fun onDraw(canvas: Canvas) {
            if (!shown) return
            canvas.drawCircle(cx, cy, radius, ring)
            canvas.drawCircle(cx + dx * radius, cy + dy * radius, radius * 0.4f, knob)
        }
    }
    private var lastX = 0f
    private var lastY = 0f
    private var midX = 0f
    private var midY = 0f

    private val gameDir: File get() = File(filesDir, "game")

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = FrameLayout(this)
        root.setBackgroundColor(Color.BLACK)
        val surface = SurfaceView(this)
        surface.holder.addCallback(this)
        root.addView(surface, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))

        info = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 13f
            setShadowLayer(4f, 0f, 0f, Color.BLACK)
            setPadding(24, 16, 24, 16)
        }
        root.addView(info, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.TOP or Gravity.START))
        stats = TextView(this).apply {
            setTextColor(Color.rgb(160, 220, 160))
            textSize = 12f
            setShadowLayer(4f, 0f, 0f, Color.BLACK)
            setPadding(24, 16, 24, 16)
        }
        root.addView(stats, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.TOP or Gravity.END))

        // World view gestures: one finger turns the camera, two fingers pinch to zoom and drag to move.
        val pinch = ScaleGestureDetector(this, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(d: ScaleGestureDetector): Boolean {
                if (worldMode && d.scaleFactor > 0f) Native.camera(0f, 0f, 1f / d.scaleFactor)
                return true
            }
        })
        surface.setOnTouchListener { v, e ->
            if (playing) { gameTouch(v, e); return@setOnTouchListener true }
            if (gameMode) return@setOnTouchListener true
            pinch.onTouchEvent(e)
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> { lastX = e.x; lastY = e.y }
                MotionEvent.ACTION_POINTER_DOWN -> if (e.pointerCount == 2) {
                    midX = (e.getX(0) + e.getX(1)) / 2f; midY = (e.getY(0) + e.getY(1)) / 2f
                }
                MotionEvent.ACTION_MOVE -> if (worldMode && e.pointerCount == 1 && !pinch.isInProgress) {
                    Native.camera(-(e.x - lastX) * 0.006f, (e.y - lastY) * 0.006f, 1f)
                    lastX = e.x; lastY = e.y
                } else if (worldMode && e.pointerCount >= 2) {
                    // the midpoint of two fingers moves the camera over the ground
                    val mx = (e.getX(0) + e.getX(1)) / 2f; val my = (e.getY(0) + e.getY(1)) / 2f
                    Native.pan((mx - midX) / v.width, -(my - midY) / v.height)
                    midX = mx; midY = my
                }
                MotionEvent.ACTION_POINTER_UP -> {
                    // carry on from the finger that stays down, so the view does not jump
                    val keep = if (e.actionIndex == 0) 1 else 0
                    if (keep < e.pointerCount) { lastX = e.getX(keep); lastY = e.getY(keep) }
                }
                MotionEvent.ACTION_UP -> v.performClick()
            }
            true
        }

        controls = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        fun add(label: String, packs: Int, textures: Int) {
            controls.addView(Button(this).apply {
                text = label
                setOnClickListener {
                    if (worldMode) {
                        if (packs != 0) { cityMode = false; cityButton.text = "City"; showWorld(packs) }
                    } else step(packs, textures)
                }
            })
        }
        add("◀ Pack", -1, 0)
        add("◀ Texture", 0, -1)
        add("Texture ▶", 0, 1)
        add("Pack ▶", 1, 0)
        modeButton = Button(this).apply {
            text = "World"
            setOnClickListener {
                if (!opened) return@setOnClickListener
                worldMode = !worldMode
                cityMode = false
                cityButton.text = "City"
                cityButton.visibility = if (worldMode) View.VISIBLE else View.GONE
                personButton.visibility = if (worldMode) View.VISIBLE else View.GONE
                moveButton.visibility = if (worldMode) View.VISIBLE else View.GONE
                personNote = ""
                text = if (worldMode) "Textures" else "World"
                if (worldMode) showWorld(0) else { Native.hideWorld(); step(0, 0) }
            }
        }
        controls.addView(modeButton)
        // In the world view: the whole city from far away, or back to the current sector.
        cityButton = Button(this).apply {
            text = "City"
            visibility = View.GONE
            setOnClickListener {
                if (!opened || !worldMode) return@setOnClickListener
                cityMode = !cityMode
                text = if (cityMode) "Sector" else "City"
                if (cityMode) showCity() else showWorld(0)
            }
        }
        controls.addView(cityButton)
        // In the world view: stands the next character variant where the camera is looking.
        personButton = Button(this).apply {
            text = "Person"
            visibility = View.GONE
            setOnClickListener {
                if (!opened || !worldMode) return@setOnClickListener
                thread {
                    val text = Native.character(1)
                    ui.post { personNote = text }
                }
            }
        }
        controls.addView(personButton)
        // Steps through the animation clips of the character's pack.
        moveButton = Button(this).apply {
            text = "Move"
            visibility = View.GONE
            setOnClickListener {
                if (!opened || !worldMode) return@setOnClickListener
                val text = Native.clip(1)
                personNote = personNote.substringBeforeLast("\n", personNote) + "\n" + text
            }
        }
        controls.addView(moveButton)
        controls.addView(Button(this).apply {
            text = "Game"
            setOnClickListener { if (opened) showTitle() }
        })
        controls.visibility = View.GONE
        root.addView(controls, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL))

        // Title view: the game's logo over the city, a tap starts play.
        stickView = StickView(this)
        stickView.radius = 60f * resources.displayMetrics.density
        root.addView(stickView, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        // The colour cover for the screen fills the opening script switches on (black, the blast's flash).
        coverView = View(this).apply {
            setBackgroundColor(Color.BLACK)
            alpha = 0f
            isClickable = false
        }
        root.addView(coverView, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        logoView = ImageView(this).apply { adjustViewBounds = true }
        titleNote = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 20f
            gravity = Gravity.CENTER
            setShadowLayer(6f, 0f, 0f, Color.BLACK)
            setPadding(0, 24, 0, 24)
        }
        titleLayer = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            visibility = View.GONE
            addView(logoView, LinearLayout.LayoutParams((resources.displayMetrics.widthPixels * 0.6f).toInt(), WRAP_CONTENT))
            addView(titleNote, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
            setOnClickListener { startPlay() }
        }
        root.addView(titleLayer, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        gameBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            visibility = View.GONE
            addView(Button(this@MainActivity).apply {
                text = "Title"
                setOnClickListener { showTitle() }
            })
            addView(Button(this@MainActivity).apply {
                text = "Viewer"
                setOnClickListener { showViewer() }
            })
        }
        root.addView(gameBar, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.BOTTOM or Gravity.END))

        importButton = Button(this).apply {
            text = "Choose game folder"
            setOnClickListener {
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_TREE)
            }
        }
        root.addView(importButton, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.CENTER))
        setContentView(root)

        if (required.all { File(gameDir, it).length() > 0 }) openGame() else showNeedsImport()
        // While the opening script runs it decides when the cover, the logo and the prompt show.
        ui.post(object : Runnable {
            override fun run() {
                if (opened && gameMode) {
                    val o = Native.opening()
                    coverView.setBackgroundColor(Color.rgb((o[3] * 255f).toInt().coerceIn(0, 255), (o[4] * 255f).toInt().coerceIn(0, 255), (o[5] * 255f).toInt().coerceIn(0, 255)))
                    coverView.alpha = o[0]
                    if (o[2] > 0f && titleLayer.visibility == View.VISIBLE && titleNote.text.toString() == "Tap to start") {
                        val show = if (o[1] > 0f) View.VISIBLE else View.INVISIBLE
                        logoView.visibility = show
                        titleNote.visibility = show
                    } else {
                        logoView.visibility = View.VISIBLE
                        titleNote.visibility = View.VISIBLE
                    }
                } else if (coverView.alpha != 0f) {
                    coverView.alpha = 0f
                }
                ui.postDelayed(this, 33)
            }
        })
        ui.post(object : Runnable {
            override fun run() {
                if (opened) {
                    stats.text = Native.status()
                    if (gameMode) info.text = Native.worldStatus()
                    else if (worldMode) Native.worldStatus().let { if (it.isNotEmpty()) info.text = if (personNote.isEmpty()) it else it + "\n" + personNote }
                }
                ui.postDelayed(this, 500)
            }
        })
    }

    /** The title view: the city turning slowly behind the game's logo. */
    private fun showTitle() {
        gameMode = true
        playing = false
        worldMode = false
        cityMode = false
        Native.stick(0f, 0f)
        stickView.shown = false
        stickView.invalidate()
        controls.visibility = View.GONE
        gameBar.visibility = View.VISIBLE
        titleLayer.visibility = View.VISIBLE
        titleNote.text = "Loading…"
        thread {
            val result = Native.title()
            val logo = Native.titleLogo()
            ui.post {
                if (logo.size > 8) {
                    val b = ByteBuffer.wrap(logo).order(java.nio.ByteOrder.LITTLE_ENDIAN)
                    val w = b.getInt(0); val h = b.getInt(4)
                    if (w > 0 && h > 0 && logo.size >= 8 + w * h * 4) {
                        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
                        bmp.copyPixelsFromBuffer(ByteBuffer.wrap(logo, 8, w * h * 4))
                        logoView.setImageBitmap(bmp)
                    }
                }
                titleNote.text = if (result.startsWith("OK")) "Tap to start" else result
            }
        }
    }

    private fun startPlay() {
        if (!gameMode || playing || titleNote.text.toString() != "Tap to start" || titleNote.visibility != View.VISIBLE) return
        titleNote.text = "Starting…"
        thread {
            val result = Native.startGame()
            ui.post {
                if (result.startsWith("OK")) {
                    playing = true
                    stickId = -1; lookId = -1
                    titleLayer.visibility = View.GONE
                } else {
                    titleNote.text = result
                }
            }
        }
    }

    /** The development viewer: textures, sectors and characters. */
    private fun showViewer() {
        gameMode = false
        playing = false
        Native.stick(0f, 0f)
        stickView.shown = false
        stickView.invalidate()
        titleLayer.visibility = View.GONE
        gameBar.visibility = View.GONE
        controls.visibility = View.VISIBLE
        worldMode = false
        modeButton.text = "World"
        cityButton.visibility = View.GONE
        personButton.visibility = View.GONE
        moveButton.visibility = View.GONE
        Native.hideWorld()
        step(0, 0)
    }

    /** Play controls: a finger that lands on the left half is the movement stick, any other turns the camera. */
    private fun gameTouch(v: View, e: MotionEvent) {
        val i = e.actionIndex
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val id = e.getPointerId(i)
                if (e.getX(i) < v.width / 2f && stickId < 0) {
                    stickId = id; stickX0 = e.getX(i); stickY0 = e.getY(i)
                    stickView.cx = stickX0; stickView.cy = stickY0; stickView.dx = 0f; stickView.dy = 0f; stickView.shown = true
                } else if (lookId < 0) {
                    lookId = id; lastX = e.getX(i); lastY = e.getY(i)
                }
            }
            MotionEvent.ACTION_MOVE -> for (k in 0 until e.pointerCount) {
                val id = e.getPointerId(k)
                if (id == stickId) {
                    var dx = (e.getX(k) - stickX0) / stickView.radius
                    var dy = (e.getY(k) - stickY0) / stickView.radius
                    val len = Math.sqrt((dx * dx + dy * dy).toDouble()).toFloat()
                    if (len > 1f) { dx /= len; dy /= len }
                    stickView.dx = dx; stickView.dy = dy
                    Native.stick(dx, -dy)
                } else if (id == lookId) {
                    Native.camera(-(e.getX(k) - lastX) * 0.006f, (e.getY(k) - lastY) * 0.006f, 1f)
                    lastX = e.getX(k); lastY = e.getY(k)
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> {
                val id = e.getPointerId(i)
                if (id == stickId || e.actionMasked != MotionEvent.ACTION_POINTER_UP) {
                    stickId = -1; stickView.shown = false
                    Native.stick(0f, 0f)
                }
                if (id == lookId || e.actionMasked != MotionEvent.ACTION_POINTER_UP) lookId = -1
                if (e.actionMasked == MotionEvent.ACTION_UP) v.performClick()
            }
        }
        stickView.invalidate()
    }

    private fun showNeedsImport() {
        importButton.visibility = View.VISIBLE
        controls.visibility = View.GONE
        titleLayer.visibility = View.GONE
        gameBar.visibility = View.GONE
        gameMode = false
        info.text = "No game data yet.\nChoose the folder that holds your own copy of the game (the one containing " +
            "PS3_GAME, or any folder above the two infamous*.psarc_s files). The files are copied into this app once."
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val tree = data?.data
        if (requestCode != REQUEST_TREE || resultCode != RESULT_OK || tree == null) return
        importButton.visibility = View.GONE
        info.text = "Looking for the game files…"
        thread { importFrom(tree) }
    }

    /** Finds the wanted files anywhere under [tree] and copies them into app storage. */
    private fun importFrom(tree: Uri) {
        try {
            val found = HashMap<String, Pair<Uri, Long>>()
            val pending = ArrayDeque<String>()
            pending.add(DocumentsContract.getTreeDocumentId(tree))
            var visited = 0
            while (pending.isNotEmpty() && found.size < wanted.size && visited < 5000) {
                val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, pending.removeFirst())
                visited++
                contentResolver.query(
                    children,
                    arrayOf(
                        DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                        DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                        DocumentsContract.Document.COLUMN_MIME_TYPE,
                        DocumentsContract.Document.COLUMN_SIZE
                    ), null, null, null
                )?.use { c ->
                    while (c.moveToNext()) {
                        val id = c.getString(0)
                        val name = c.getString(1) ?: continue
                        if (c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR) {
                            pending.add(id)
                        } else if (name in wanted && name !in found) {
                            found[name] = Pair(DocumentsContract.buildDocumentUriUsingTree(tree, id), c.getLong(3))
                        }
                    }
                }
            }
            val missing = required.filter { it !in found }
            if (missing.isNotEmpty()) {
                ui.post {
                    showNeedsImport()
                    info.text = "Could not find " + missing.joinToString(" and ") + " under that folder.\n" +
                        "They are normally in PS3_GAME/USRDIR/cache/psarc/install1 and install2."
                }
                return
            }
            gameDir.mkdirs()
            val total = found.values.sumOf { it.second }.coerceAtLeast(1)
            var done = 0L
            var lastShown = 0L
            val buf = ByteArray(1 shl 20)
            for ((name, src) in found) {
                val tmp = File(gameDir, "$name.part")
                contentResolver.openInputStream(src.first)!!.use { input ->
                    tmp.outputStream().use { out ->
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            out.write(buf, 0, n)
                            done += n
                            if (done - lastShown > (8 shl 20)) {
                                lastShown = done
                                val pct = done * 100 / total
                                ui.post { info.text = "Importing game files… $pct%  ($name)" }
                            }
                        }
                    }
                }
                if (!tmp.renameTo(File(gameDir, name))) throw java.io.IOException("cannot store $name")
            }
            ui.post { openGame() }
        } catch (e: Exception) {
            ui.post {
                showNeedsImport()
                info.text = "Import failed: ${e.message}"
            }
        }
    }

    private fun openGame() {
        info.text = "Opening archives…"
        thread {
            val result = Native.open(gameDir.absolutePath)
            ui.post {
                if (result.startsWith("OK")) {
                    opened = true
                    importButton.visibility = View.GONE
                    showTitle()
                } else {
                    showNeedsImport()
                    info.text = "The imported files could not be opened: $result\nChoose the game folder again to re-import."
                }
            }
        }
    }

    private fun showWorld(delta: Int) {
        if (!opened) return
        info.text = "Loading sector…"
        thread {
            val text = Native.world(delta)
            ui.post { info.text = text }
        }
    }

    private fun showCity() {
        if (!opened) return
        info.text = "Loading the city…"
        thread {
            val text = Native.city()
            ui.post { info.text = text }
        }
    }

    private fun step(packs: Int, textures: Int) {
        if (!opened) return
        thread {
            val text = Native.step(packs, textures)
            ui.post { info.text = text }
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {}

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Native.setSurface(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Native.setSurface(null)
    }

    private companion object {
        const val REQUEST_TREE = 1
    }
}
