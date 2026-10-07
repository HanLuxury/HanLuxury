package com.holy.game.gui.modern

import android.app.Activity
import android.util.DisplayMetrics
import android.view.View
import android.widget.FrameLayout
import androidx.annotation.Keep
import com.holy.game.core.Samp

/**
 * Bridge between the native client (java_systems/ModernMenu.cpp) and the
 * modern pause menu, map overlay and HUD layout editor.
 *
 * Native -> Java: the @JvmStatic functions below (called on the game thread,
 * every one of them switches to the UI thread).
 * Java -> native: [request]. The client queues the request and runs it on the
 * game thread, so no GTA code is called from the UI thread.
 *
 * Kept by name: the client looks the class and its methods up through JNI.
 */
@Keep
object ModernMenu {

    // Must match CModernMenu::eRequest.
    const val REQUEST_RESUME = 1
    const val REQUEST_OPEN_MAP = 2
    const val REQUEST_CLOSE_MAP = 3
    const val REQUEST_ZOOM_IN = 4
    const val REQUEST_ZOOM_OUT = 5
    const val REQUEST_CENTER_PLAYER = 6
    const val REQUEST_TOGGLE_WAYPOINT = 7
    const val REQUEST_GTA_SETTINGS = 8
    const val REQUEST_LAYOUT_OPEN = 9
    const val REQUEST_LAYOUT_CLOSE = 10
    const val REQUEST_MAP_TO_PAUSE = 11
    const val REQUEST_GTA_CONTROLS = 12
    const val REQUEST_MAP_LEAVE = 13
    const val REQUEST_SET_SETTING = 14
    const val REQUEST_GFX_SAVE = 15
    const val REQUEST_GFX_RESET = 16

    @JvmStatic
    external fun nativeRequest(request: Int)

    @JvmStatic
    external fun nativeRequestArg(request: Int, arg: Int)

    /** 4 ints per GTA setting: value, min, max, visible. */
    @JvmStatic
    external fun nativeGetSettings(): IntArray?

    @JvmStatic
    external fun nativeIsConnected(): Boolean

    /** Writes settings.ini right now (any thread). */
    @JvmStatic
    external fun nativeSaveSettingsNow()

    /**
     * Saves the client settings immediately. Called from Activity.onPause and
     * before exiting: the game thread does not run in the background and
     * Android may kill the process there, losing a pending save.
     */
    @JvmStatic
    fun saveSettingsNow() {
        try {
            nativeSaveSettingsNow()
        } catch (t: Throwable) {
            // Native library not loaded yet (early pause): nothing to save.
        }
    }

    /** Radar rectangle: screen pixels (x1, y1, x2, y2) and GTA 640x480 units. */
    @JvmStatic
    external fun nativeSetRadarRect(
        x1: Float, y1: Float, x2: Float, y2: Float,
        gtaX: Float, gtaY: Float, gtaWidth: Float, gtaHeight: Float
    )

    private var pauseMenu: ModernPauseMenu? = null
    private var layoutEditor: HudLayoutEditor? = null

    internal val activity: Activity
        get() = Samp.activity

    /**
     * Parent of the modern menu and the layout editor: the activity content
     * root, NOT ui_layout. When the GTA menu opens (map tab, GTA button
     * editor) CTimer::StartUserPause calls NvEventQueueActivity.setPauseState
     * which hides the whole ui_layout; the menu must stay visible then.
     */
    internal fun container(): FrameLayout? = activity.findViewById(android.R.id.content)

    internal fun request(request: Int) {
        try {
            nativeRequest(request)
        } catch (t: Throwable) {
            t.printStackTrace()
        }
    }

    private fun ui(block: () -> Unit) {
        activity.runOnUiThread {
            try {
                block()
            } catch (t: Throwable) {
                t.printStackTrace()
            }
        }
    }

    // ------------------------------------------------------------ native -> Java

    @JvmStatic
    fun showPause(name: String, id: Int, score: Int, ping: Int, money: Int) = ui {
        try {
            hideHud()
            val menu = pauseMenu ?: ModernPauseMenu(activity).also { pauseMenu = it }
            menu.show(PlayerInfo(name, id, score, ping, money))
        } catch (t: Throwable) {
            // Never leave the game in the pause state without a menu.
            t.printStackTrace()
            pauseMenu = null
            restoreHud()
            request(REQUEST_RESUME)
        }
    }

    @JvmStatic
    fun hidePause() = ui {
        pauseMenu?.hide(null)
    }

    /** The native map was closed by the game (death, cutscene, ...). */
    @JvmStatic
    fun hideMap() = ui {
        pauseMenu?.hide(null)
    }

    /** The pause was opened by tapping the radar: show the map tab. */
    @JvmStatic
    fun openMapTab() = ui {
        pauseMenu?.openMapTab()
    }

    /** The native map could not open; the pause menu stays. */
    @JvmStatic
    fun onMapFailed() = ui {
        pauseMenu?.onNativeMapClosed()
    }

    /** Every menu is closed and the game runs again. */
    @JvmStatic
    fun onClosed() = ui {
        pauseMenu?.hide(null)
        restoreHud()
    }

    /** Connected to the server: apply the saved HUD layout. */
    @JvmStatic
    fun onGameReady() = ui {
        val root = container() ?: return@ui
        root.post {
            try {
                HudLayoutStore.applyAll(activity)
                if (HudLayoutStore.isRadarMoved(activity)) {
                    resyncRadar()
                    // HudManager sends the radar rectangle once when the HUD
                    // is created; send ours again in case that came later.
                    root.postDelayed({ resyncRadar() }, RADAR_RESYNC_DELAY_MS)
                }
            } catch (t: Throwable) {
                t.printStackTrace()
            }
        }
    }

    /** Android back button while the layout editor is open. */
    @JvmStatic
    fun onBackPressed() = ui {
        layoutEditor?.cancel()
    }

    // ------------------------------------------------------ pause menu actions

    internal fun resumeGame() {
        pauseMenu?.hide { request(REQUEST_RESUME) }
    }

    /** Close button while the map tab is open: straight back to the game. */
    internal fun closeFromMap() {
        pauseMenu?.hide { request(REQUEST_CLOSE_MAP) }
    }

    /** Writes one GTA setting (MobileSettings) on the game thread. */
    internal fun setSetting(id: Int, value: Int) {
        try {
            nativeRequestArg(REQUEST_SET_SETTING, (id shl 16) or (value and 0xFFFF))
        } catch (t: Throwable) {
            t.printStackTrace()
        }
    }

    internal fun readSettings(): IntArray? {
        return try {
            nativeGetSettings()?.takeIf { it.size >= GtaSettings.COUNT * 4 }
        } catch (t: Throwable) {
            null
        }
    }

    internal fun openLayoutEditor() {
        pauseMenu?.hide(null)
        restoreHud()
        request(REQUEST_LAYOUT_OPEN)
        try {
            layoutEditor?.cancel()
            layoutEditor = HudLayoutEditor(activity) {
                layoutEditor = null
                request(REQUEST_LAYOUT_CLOSE)
            }.also { it.show() }
        } catch (t: Throwable) {
            t.printStackTrace()
            layoutEditor = null
            request(REQUEST_LAYOUT_CLOSE)
        }
    }

    /**
     * Opens the GTA touch button editor (sprint, jump, fire, enter car, ...)
     * directly. The client shows the pause menu again when it is closed.
     */
    internal fun openGtaControls() {
        pauseMenu?.hide(null)
        request(REQUEST_GTA_CONTROLS)
    }

    internal fun openClientSettings() {
        pauseMenu?.hide { request(REQUEST_RESUME) }
        (activity as? Samp)?.showClientSettings()
    }

    internal fun exitGame() {
        // Save settings first: exitGame() ends the process right away.
        // settings.ini now on this thread; gta_sa.set on the game thread.
        saveSettingsNow()
        request(REQUEST_GFX_SAVE)
        activity.window.decorView.postDelayed({ (activity as? Samp)?.exitGame() }, 250L)
    }

    // ------------------------------------------------------------------ HUD

    private var hudHidden = false
    private var hudVisibilityBefore = View.VISIBLE

    /**
     * Hides the Java HUD (radar frame, weapon, money, chat, RP buttons) while
     * a menu is open. Only the root of hud.xml is touched; nothing else in
     * the project changes its visibility.
     */
    internal fun hideHud() {
        val hud = ModernUi.findView<View>(activity, "hudView") ?: return
        if (!hudHidden) {
            hudVisibilityBefore = hud.visibility
            hudHidden = true
        }
        hud.visibility = View.INVISIBLE
    }

    internal fun restoreHud() {
        if (!hudHidden) return
        hudHidden = false
        ModernUi.findView<View>(activity, "hudView")?.visibility = hudVisibilityBefore
    }

    /**
     * Sends the radar rectangle to the game after the radar was moved or
     * resized. Same maths as HudManager / ConvertViewCoordsToGta.
     */
    internal fun resyncRadar() {
        val radar = ModernUi.findView<View>(activity, "hud_bg") ?: return
        radar.post {
            try {
                if (radar.width <= 0 || radar.height <= 0) return@post

                val location = IntArray(2)
                radar.getLocationOnScreen(location)
                var scale = 1f
                var v: View? = radar
                while (v != null) {
                    scale *= v.scaleX
                    v = v.parent as? View
                }
                val x = location[0].toFloat()
                val y = location[1].toFloat()
                val w = radar.width * scale
                val h = radar.height * scale

                val metrics = DisplayMetrics()
                @Suppress("DEPRECATION")
                activity.windowManager.defaultDisplay.getRealMetrics(metrics)
                val sw = metrics.widthPixels.toFloat()
                val sh = metrics.heightPixels.toFloat()
                if (sw <= 0f || sh <= 0f) return@post

                val gtaX = 640f * ((x + w / 2f) / sw)
                val gtaY = 480f * ((y + h / 2.2f) / sh)
                val gtaW = 640f * (w / 2f / sw)
                val gtaH = 480f * (h / 3.333f / sh)
                nativeSetRadarRect(x, y, x + w, y + h, gtaX, gtaY, gtaW, gtaH)
            } catch (t: Throwable) {
                // Old client without this native: the radar keeps its place.
                t.printStackTrace()
            }
        }
    }

    private const val RADAR_RESYNC_DELAY_MS = 3000L

    data class PlayerInfo(val name: String, val id: Int, val score: Int, val ping: Int, val money: Int)
}
