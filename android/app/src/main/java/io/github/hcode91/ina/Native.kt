package io.github.hcode91.ina

import android.view.Surface

/** Entry points of libina.so (engine/platform/android_jni.cpp). */
object Native {
    init {
        System.loadLibrary("ina")
    }

    /** Opens the imported archives in [dir]. Returns "OK ..." or "ERROR ...". */
    @JvmStatic external fun open(dir: String): String

    /** Moves by packs and/or textures and returns a description of what is shown. */
    @JvmStatic external fun step(packDelta: Int, textureDelta: Int): String

    /** Shows a city sector in full detail, in its place in the city, moving by [delta] sectors. Returns a description. */
    @JvmStatic external fun world(delta: Int): String

    /** Shows the whole city as drawn from far away. Returns a description. */
    @JvmStatic external fun city(): String

    /** Stands a character at the camera target and animates it, stepping variants by [delta]. Returns a description. */
    @JvmStatic external fun character(delta: Int): String

    /** Steps the clip the character plays by [delta]. Returns a description of the clip. */
    @JvmStatic external fun clip(delta: Int): String

    /** Shows the title view: the city around the opening checkpoint. Returns "OK…" or an error. */
    @JvmStatic external fun title(): String

    /** The game's logo: width and height (4 bytes each, little-endian) then RGBA rows; empty when missing. */
    @JvmStatic external fun titleLogo(): ByteArray

    /** Starts play with the player character. Returns "OK…" or "ERROR…". */
    @JvmStatic external fun startGame(): String

    /** The movement stick: x right, y forward, each -1..1. */
    /** What the opening script wants on screen: [black cover 0..1, prompt shown, script running]. */
    @JvmStatic external fun opening(): FloatArray

    @JvmStatic external fun stick(x: Float, y: Float)

    /** Moves the world camera over the ground by a fraction of the screen (right and up positive). */
    @JvmStatic external fun pan(dx: Float, dy: Float)

    /** Describes what the world view is streaming and drawing now; empty outside the world view. */
    @JvmStatic external fun worldStatus(): String

    /** Goes back to the texture viewer. */
    @JvmStatic external fun hideWorld()

    /** Turns the world camera (radians) and scales its distance. */
    @JvmStatic external fun camera(yaw: Float, pitch: Float, zoom: Float)

    @JvmStatic external fun setSurface(surface: Surface?)

    @JvmStatic external fun status(): String
}
