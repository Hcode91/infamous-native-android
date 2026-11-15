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

    /** Shows the static geometry of a city sector, moving by [delta] sectors. Returns a description. */
    @JvmStatic external fun world(delta: Int): String

    /** Goes back to the texture viewer. */
    @JvmStatic external fun hideWorld()

    /** Turns the world camera (radians) and scales its distance. */
    @JvmStatic external fun camera(yaw: Float, pitch: Float, zoom: Float)

    @JvmStatic external fun setSurface(surface: Surface?)

    @JvmStatic external fun status(): String
}
