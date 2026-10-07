package com.holy.game.core

import android.annotation.SuppressLint
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.media.AudioAttributes
import android.media.MediaPlayer
import android.media.SoundPool
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Vibrator
import android.util.Log
import android.view.View
import android.view.ViewGroup
import android.view.ViewParent
import android.view.ViewStub
import android.view.WindowManager
import android.view.animation.AnimationUtils
import android.webkit.WebView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.constraintlayout.widget.ConstraintLayout
import com.google.android.material.progressindicator.CircularProgressIndicator
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.firebase.crashlytics.FirebaseCrashlytics
import com.holy.game.R
import com.holy.game.gui.hud.HudManager
import com.holy.launcher.domain.enums.StorageElements
import com.holy.launcher.storage.NativeStorage
import com.holy.launcher.storage.Storage
import java.io.File
import java.io.IOException
import java.text.DecimalFormat
import java.text.DecimalFormatSymbols
import java.util.Locale
import java.util.Timer
import java.util.TimerTask

class Samp : com.holy.game.core.GTASA() {
    private data class LoadscreenTrack(val resId: Int, val title: String)

    private var mDialogClientSettings: com.holy.game.core.DialogClientSettings? = null
    private var loadscreenMusic: MediaPlayer? = null
    private var loadscreenTrackIndex = 0
    private val loadscreenPlaylist by lazy {
        listOf(
            LoadscreenTrack(R.raw.eagle_rap_nightdrive, "Night Drive"),
            LoadscreenTrack(R.raw.eagle_rap_southblock, "South Block"),
            LoadscreenTrack(R.raw.eagle_rap_citylights, "City Lights"),
            LoadscreenTrack(R.raw.eagle_rap_afterhours, "After Hours"),
            LoadscreenTrack(R.raw.eagle_rap_lowrider, "Low Rider"),
            LoadscreenTrack(R.raw.eagle_rap_midnight, "Midnight Run"),
            LoadscreenTrack(R.raw.eagle_rap_rooftop, "Rooftop Smoke"),
            LoadscreenTrack(R.raw.eagle_rap_downtown, "Downtown Loop"),
            LoadscreenTrack(R.raw.eagle_rap_westcoast, "West Coast Breeze"),
            LoadscreenTrack(R.raw.eagle_rap_streetdreams, "Street Dreams")
        )
    }
    private val loadscreenHandler = Handler(Looper.getMainLooper())
    private val loadscreenMusicTicker = object : Runnable {
        override fun run() {
            val player = loadscreenMusic
            if (player != null) {
                try {
                    val duration = player.duration.coerceAtLeast(1)
                    val current = player.currentPosition.coerceAtLeast(0)
                    val progress = ((current.toLong() * 1000L) / duration.toLong()).toInt().coerceIn(0, 1000)
                    findViewById<LinearProgressIndicator>(R.id.loadscreen_music_progress)?.progress = progress
                    findViewById<TextView>(R.id.loadscreen_music_current)?.text = formatLoadscreenTime(current)
                    findViewById<TextView>(R.id.loadscreen_music_duration)?.text = formatLoadscreenTime(duration)
                } catch (_: Exception) {
                }
                loadscreenHandler.postDelayed(this, 250L)
            }
        }
    }

    private external fun initSAMP(maxFps: Float, directory: String)
    private external fun initServer(ip: String, port: Int)

    private external fun registerCustomSkin(id: Int, skinName: String, parentSkinId: Int)
    private external fun registerCustomVehicle(id: Int, modelName: String, parentVehicleId: Int)
    private external fun registerCustomObject(id: Int, modelName: String, parentObjectId: Int)

    override fun onCreate(bundle: Bundle?) {
        activity = this

        val display = Companion.windowManager.defaultDisplay
        maxFps = display.refreshRate

        val internalDir = File(filesDir, "AudioConfig")
        clearDir(internalDir)
        copyFromAssets(internalDir)

        val host = NativeStorage.getClientProperty("host", this) ?: "127.0.0.1"
        val portStr = NativeStorage.getClientProperty("port", this) ?: "7777"
        val port = portStr.toIntOrNull() ?: 7777

        initServer(host, port)
        initSAMP(maxFps, filesDir.toString())
        // EAGLE graphics engine: re-apply the player's "Grafis" settings before GTA builds its shaders.
        GraphicsNative.applySavedSettings()
        super.onCreate(bundle)
        init()
    }
    
    override fun onDestroy() {
        stopLoadscreenMusic()
        super.onDestroy()
    }

    private fun clearDir(dir: File) {
        try {
            if (dir.exists()) {
                dir.deleteRecursively()
            }
        } catch (_: IOException) {}
    }

    private fun copyFromAssets(dir: File) {
        try {
            if (!dir.exists()) dir.mkdirs()
            val assetFiles = assets.list("AudioConfig") ?: return
            assetFiles.forEach { filename ->
                assets.open("AudioConfig/$filename").use { inputStream ->
                    File(dir, filename).outputStream().use { outputStream ->
                        inputStream.copyTo(outputStream)
                    }
                }
            }
        } catch (_: IOException) {}
    }

    fun init() {
        HudManager() // Inisialisasi HudManager (Sekaligus load fragment di dalamnya)
        preloadCef()
        loadCustomDL()
        setupLoadingScreen()
    }

    private fun setupLoadingScreen() {
        val serverName = Storage.getProperty(StorageElements.SERVER_NAME, this)
            ?.takeIf { it.isNotBlank() } ?: "EAGLE ROLEPLAY"
        val nickname = NativeStorage.getClientProperty("name", this)
            ?.takeIf { it.isNotBlank() } ?: "Player"

        runOnUiThread {
            findViewById<TextView>(R.id.loading_status)?.text = "Starting"
            findViewById<TextView>(R.id.static_tip_text)?.text = "Preparing $serverName for $nickname..."
            updateLoadscreenTrackLabels()
            findViewById<CircularProgressIndicator>(R.id.loadscreen_connect_progress)?.progress = 8
            findViewById<TextView>(R.id.loadscreen_percent)?.text = "8%"
            findViewById<LinearProgressIndicator>(R.id.loadscreen_music_progress)?.max = 1000
            findViewById<LinearProgressIndicator>(R.id.loadscreen_music_progress)?.progress = 0
            findViewById<TextView>(R.id.loadscreen_music_prev)?.setOnClickListener { changeLoadscreenTrack(-1) }
            findViewById<TextView>(R.id.loadscreen_music_back)?.setOnClickListener { seekLoadscreenMusic(-5000) }
            findViewById<TextView>(R.id.loadscreen_music_toggle)?.setOnClickListener { toggleLoadscreenMusic() }
            findViewById<TextView>(R.id.loadscreen_music_forward)?.setOnClickListener { seekLoadscreenMusic(5000) }
            findViewById<TextView>(R.id.loadscreen_music_next)?.setOnClickListener { changeLoadscreenTrack(1) }
        }

        startLoadscreenMusic()
        updateLoadingScreen("Preparing game world", 12)
    }

    fun updateLoadingScreen(status: String?, progress: Int) {
        val safeProgress = progress.coerceIn(0, 100)
        runOnUiThread {
            findViewById<TextView>(R.id.loading_status)?.text = status?.takeIf { it.isNotBlank() } ?: "Loading"
            findViewById<CircularProgressIndicator>(R.id.loadscreen_connect_progress)?.progress = safeProgress
            findViewById<TextView>(R.id.loadscreen_percent)?.text = "$safeProgress%"

            val serverName = Storage.getProperty(StorageElements.SERVER_NAME, this)
                ?.takeIf { it.isNotBlank() } ?: "server"
            findViewById<TextView>(R.id.static_tip_text)?.text = when {
                safeProgress < 30 -> "Preparing GTA world and client systems..."
                safeProgress < 60 -> "Connecting to $serverName..."
                safeProgress < 90 -> "Authenticating multiplayer session..."
                else -> "Server connected. Synchronizing player data..."
            }
        }
    }

    private fun startLoadscreenMusic() {
        if (loadscreenMusic != null) return
        playLoadscreenTrack(loadscreenTrackIndex, true)
    }

    private fun playLoadscreenTrack(index: Int, autoPlay: Boolean) {
        if (loadscreenPlaylist.isEmpty()) return
        val normalized = ((index % loadscreenPlaylist.size) + loadscreenPlaylist.size) % loadscreenPlaylist.size
        loadscreenTrackIndex = normalized
        val track = loadscreenPlaylist[normalized]

        try {
            loadscreenMusic?.setOnCompletionListener(null)
            loadscreenMusic?.release()
        } catch (_: Exception) {
        }
        loadscreenMusic = null

        try {
            loadscreenMusic = MediaPlayer.create(this, track.resId)?.apply {
                isLooping = false
                setVolume(0.52f, 0.52f)
                setOnCompletionListener {
                    runOnUiThread { playLoadscreenTrack(loadscreenTrackIndex + 1, true) }
                }
                if (autoPlay) start()
            }
            updateLoadscreenTrackLabels()
            findViewById<TextView>(R.id.loadscreen_music_toggle)?.text = if (autoPlay) "❚❚" else "▶"
            findViewById<LinearProgressIndicator>(R.id.loadscreen_music_progress)?.progress = 0
            findViewById<TextView>(R.id.loadscreen_music_current)?.text = "00:00"
            loadscreenHandler.removeCallbacks(loadscreenMusicTicker)
            loadscreenHandler.post(loadscreenMusicTicker)
        } catch (e: Exception) {
            Log.w("EagleLoadscreen", "Unable to start loading music", e)
        }
    }

    private fun updateLoadscreenTrackLabels() {
        if (loadscreenPlaylist.isEmpty()) return
        val track = loadscreenPlaylist[loadscreenTrackIndex.coerceIn(0, loadscreenPlaylist.lastIndex)]
        findViewById<TextView>(R.id.loadscreen_music_title)?.text = track.title
        findViewById<TextView>(R.id.loadscreen_music_artist)?.text =
            "Eagle Original • ${loadscreenTrackIndex + 1}/${loadscreenPlaylist.size}"
    }

    private fun changeLoadscreenTrack(direction: Int) {
        val wasPlaying = try { loadscreenMusic?.isPlaying != false } catch (_: Exception) { true }
        playLoadscreenTrack(loadscreenTrackIndex + direction, wasPlaying)
    }

    private fun seekLoadscreenMusic(deltaMs: Int) {
        val player = loadscreenMusic ?: return
        try {
            val target = (player.currentPosition + deltaMs).coerceIn(0, player.duration.coerceAtLeast(1))
            player.seekTo(target)
        } catch (_: Exception) {
        }
    }

    private fun toggleLoadscreenMusic() {
        val player = loadscreenMusic ?: return
        try {
            if (player.isPlaying) {
                player.pause()
                findViewById<TextView>(R.id.loadscreen_music_toggle)?.text = "▶"
            } else {
                player.start()
                findViewById<TextView>(R.id.loadscreen_music_toggle)?.text = "❚❚"
            }
        } catch (_: Exception) {
        }
    }

    private fun formatLoadscreenTime(ms: Int): String {
        val totalSeconds = (ms / 1000).coerceAtLeast(0)
        return String.format(Locale.US, "%02d:%02d", totalSeconds / 60, totalSeconds % 60)
    }

    private fun stopLoadscreenMusic() {
        loadscreenHandler.removeCallbacks(loadscreenMusicTicker)
        try {
            loadscreenMusic?.setOnCompletionListener(null)
            loadscreenMusic?.stop()
        } catch (_: Exception) {
        }
        try {
            loadscreenMusic?.release()
        } catch (_: Exception) {
        }
        loadscreenMusic = null
    }

    private fun loadCustomDL() {
        try {
            Log.d("HolyModloader", "Sistem Custom DL Berjalan!")
        } catch (e: Exception) {
            Log.e("HolyModloader", "Gagal memuat Custom DL: ${e.message}")
        }
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun preloadCef() {
        val webView = WebView(activity).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            settings.allowFileAccess = true
            setBackgroundColor(android.graphics.Color.TRANSPARENT)
            setLayerType(View.LAYER_TYPE_HARDWARE, null)
        }
        webView.loadUrl("about:blank")
        webView.postDelayed({ webView.destroy() }, 500)
    }

    fun countAllChildren(viewParent: ViewParent?): Int {
        if (viewParent == null || viewParent !is ViewGroup) return 0
        val childCount = viewParent.childCount
        var totalCount = childCount
        for (i in 0 until childCount) {
            val childView = viewParent.getChildAt(i)
            if (childView is ViewGroup) {
                totalCount += countAllChildren(childView)
            }
        }
        return totalCount
    }

    fun hideLoadingScreen() {
        updateLoadingScreen("Connected", 100)
        val task: TimerTask = object : TimerTask() {
            override fun run() {
                activity.runOnUiThread {
                    val loadscreenMainLayout = activity.findViewById<ConstraintLayout>(R.id.loadscreen_main_layout)
                    if (loadscreenMainLayout != null) {
                        loadscreenMainLayout.animate()
                            .alpha(0f)
                            .setDuration(350L)
                            .withEndAction {
                                stopLoadscreenMusic()
                                val parentContainer = loadscreenMainLayout.parent as? ViewGroup
                                parentContainer?.removeView(loadscreenMainLayout)
                            }
                            .start()
                    } else {
                        stopLoadscreenMusic()
                    }
                }
            }
        }
        val timer = Timer("Timer")
        timer.schedule(task, 900L)
    }

    fun exitGame() {
        stopLoadscreenMusic()
        FirebaseCrashlytics.getInstance().deleteUnsentReports()
        FirebaseCrashlytics.getInstance().setCrashlyticsCollectionEnabled(false)
        finishAndRemoveTask()
        System.exit(0)
    }

    fun goVibrate(milliseconds: Int) {
        if (vibrator.hasVibrator()) {
            vibrator.vibrate(milliseconds.toLong())
        }
    }

    private fun openUrl(url: String) {
        runOnUiThread {
            val address = Uri.parse(url)
            val openlink = Intent(Intent.ACTION_VIEW, address)
            startActivity(openlink)
        }
    }

    private fun copyTextToBuffer(string: String) {
        runOnUiThread {
            val clipboardManager = this.getSystemService(CLIPBOARD_SERVICE) as ClipboardManager
            val clipData = ClipData.newPlainText("text", string)
            clipboardManager.setPrimaryClip(clipData)
            Toast.makeText(this, "Скопированно в буфер обмена ", Toast.LENGTH_SHORT).show()
        }
    }

    companion object {
        @JvmStatic lateinit var activity: AppCompatActivity
        @JvmStatic val vibrator by lazy { activity.getSystemService(VIBRATOR_SERVICE) as Vibrator }
        @JvmStatic var maxFps = 0f
        val windowManager by lazy { activity.getSystemService(WINDOW_SERVICE) as WindowManager }
        val formatter = DecimalFormatSymbols(Locale.getDefault()).run {
            groupingSeparator = '.'
            DecimalFormat("###,###.###", this)
        }
        const val INVALID_PLAYER_ID = 65535
        val soundPool = SoundPool.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
            .build()
        @JvmStatic val clickAnim by lazy { AnimationUtils.loadAnimation(activity, R.anim.button_click) }

        external fun sendCommand(command: String?)
        external fun playUrlSound(url: String?)

        fun deInflatedViewStud(inflated: View?, id: Int, layout: Int) {
            activity.runOnUiThread {
                if (inflated == null) return@runOnUiThread
                val parentContainer = inflated.parent as? ViewGroup ?: return@runOnUiThread
                val index = parentContainer.indexOfChild(inflated)
                if (index == -1) return@runOnUiThread
                parentContainer.removeViewAt(index)
                val viewStub = ViewStub(activity)
                viewStub.layoutResource = layout
                viewStub.id = id
                parentContainer.addView(viewStub, index)
            }
        }

        val isTablet: Boolean
            get() = activity.resources.getBoolean(R.bool.is_tablet)
    }

    fun showClientSettings() {
        runOnUiThread {
            if (mDialogClientSettings != null) mDialogClientSettings = null
            mDialogClientSettings = com.holy.game.core.DialogClientSettings()
            mDialogClientSettings?.show(supportFragmentManager, "test")
        }
    }
}
