package io.github.hcode91.ina

import android.app.Activity
import android.content.Intent
import android.graphics.Color
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
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import kotlin.concurrent.thread

/**
 * Development shell. Imports the user's own game files once (copied into app storage), then shows the
 * game's textures decoded by the native engine. No game data ships with the app.
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
    private var personNote = ""
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
        controls.visibility = View.GONE
        root.addView(controls, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL))

        importButton = Button(this).apply {
            text = "Choose game folder"
            setOnClickListener {
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_TREE)
            }
        }
        root.addView(importButton, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.CENTER))
        setContentView(root)

        if (required.all { File(gameDir, it).length() > 0 }) openGame() else showNeedsImport()
        ui.post(object : Runnable {
            override fun run() {
                if (opened) {
                    stats.text = Native.status()
                    if (worldMode) Native.worldStatus().let { if (it.isNotEmpty()) info.text = if (personNote.isEmpty()) it else it + "\n" + personNote }
                }
                ui.postDelayed(this, 500)
            }
        })
    }

    private fun showNeedsImport() {
        importButton.visibility = View.VISIBLE
        controls.visibility = View.GONE
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
                    controls.visibility = View.VISIBLE
                    step(0, 0)
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
