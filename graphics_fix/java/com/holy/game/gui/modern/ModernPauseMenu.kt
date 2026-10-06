package com.holy.game.gui.modern

import android.annotation.SuppressLint
import android.app.Activity
import android.graphics.Color
import android.graphics.Typeface
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.animation.DecelerateInterpolator
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.util.Locale

/**
 * Pause menu in the GTA V / FiveM style.
 *
 * Header with the server and player, a tab bar, a list of rows on the left
 * and a description panel on the right. The GTA SA settings (graphics, audio,
 * controls) are rebuilt here as rows and written straight into the game's
 * MobileSettings through the client. The PETA tab shows the native GTA map
 * under the header and tab bar.
 */
class ModernPauseMenu(private val activity: Activity) {

    private enum class TabId(val title: String) {
        MAP("PETA"),
        GAME("GAME"),
        GRAPHICS("GRAFIS"),
        AUDIO("AUDIO"),
        CONTROLS("KONTROL")
    }

    // ------------------------------------------------------------------ rows

    private abstract class Row(val label: String, val desc: String)

    private class ActionRow(label: String, desc: String, val action: (ActionRow) -> Unit) : Row(label, desc) {
        var descOverride: String? = null
        var isHeader = false
    }

    /** A GTA setting with named values; options[i] is the label of min + i. */
    private class OptionRow(label: String, desc: String, val setting: Int, val options: List<String>) :
        Row(label, desc)

    /**
     * A GTA setting shown as a bar. live = false: sent only when the finger is
     * lifted (Resolution recreates the render target on every change).
     */
    private class SliderRow(label: String, desc: String, val setting: Int, val percent: Boolean,
                            val live: Boolean = true, val unit: String = "") :
        Row(label, desc)

    private class RowViews(
        val row: Row,
        val view: LinearLayout,
        val label: TextView,
        val value: TextView?,
        val arrowLeft: TextView?,
        val arrowRight: TextView?,
        val slider: GtaSliderView?
    )

    private class TabViews(val id: TabId, val view: FrameLayout, val text: TextView, val strip: View)

    // ----------------------------------------------------------------- views

    private val root: View
    private val scrim: View
    private val frame: View
    private val header: View
    private val tabsBar: LinearLayout
    private val listPage: View
    private val mapPage: View
    private val mapSpace: View
    private val rowsScroll: ScrollView
    private val rowsLayout: LinearLayout
    private val sidePlayer: View
    private val sidePlayerName: TextView
    private val sidePlayerStats: TextView
    private val sideTitle: TextView
    private val sideBody: TextView
    private val serverName: TextView
    private val playerName: TextView
    private val playerMoney: TextView

    private val tabs = ArrayList<TabViews>()
    private val rows = ArrayList<RowViews>()
    private var currentTab = TabId.GAME
    private var selectedRow = 0
    private var info = ModernMenu.PlayerInfo("", 0, 0, 0, 0)

    /** GTA settings snapshot: value, min, max, visible per setting. */
    private var settings: IntArray? = null

    private var exitArmedUntil = 0L
    private var tappedValue = false
    private var hiding = false
    private val pendingActions = mutableListOf<() -> Unit>()

    private val density = activity.resources.displayMetrics.density
    private val boldFont: Typeface? = ModernUi.font(activity, "gilroy_bold")
    private val rowFont: Typeface? = ModernUi.font(activity, "gilroy_semibold")

    init {
        // Built in code on purpose: see ModernUi (no new resources).
        val ctx = activity
        val match = ViewGroup.LayoutParams.MATCH_PARENT
        val wrap = ViewGroup.LayoutParams.WRAP_CONTENT

        val rootLayout = FrameLayout(ctx).apply { isMotionEventSplittingEnabled = false }
        root = rootLayout

        scrim = View(ctx).apply {
            background = ModernUi.gradient3(
                Color.argb(0xE6, 0, 0, 0), Color.argb(0xB3, 0, 0, 0), Color.argb(0xD9, 0, 0, 0))
        }
        rootLayout.addView(scrim, FrameLayout.LayoutParams(match, match))

        // ---- map tab: controls over the native map (root does not take touches)
        val mapLayout = FrameLayout(ctx).apply { visibility = View.GONE }
        mapPage = mapLayout
        mapLayout.addView(ModernUi.CrosshairView(ctx),
            FrameLayout.LayoutParams(dpi(36f), dpi(36f), Gravity.CENTER))

        val zoomColumn = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
        }
        val zoomIn = ModernUi.glyphButton(ctx, "+", 44f)
        val zoomOut = ModernUi.glyphButton(ctx, "−", 44f)
        val locate = ModernUi.glyphButton(ctx, "◎", 44f)
        zoomColumn.addView(zoomIn, LinearLayout.LayoutParams(dpi(44f), dpi(44f)))
        zoomColumn.addView(zoomOut, LinearLayout.LayoutParams(dpi(44f), dpi(44f)).apply { topMargin = dpi(6f) })
        zoomColumn.addView(locate, LinearLayout.LayoutParams(dpi(44f), dpi(44f)).apply { topMargin = dpi(16f) })
        mapLayout.addView(zoomColumn, FrameLayout.LayoutParams(wrap, wrap, Gravity.END or Gravity.CENTER_VERTICAL).apply {
            topMargin = dpi(40f)
            marginEnd = dpi(28f)
        })

        val waypoint = ModernUi.text(ctx, "TANDAI / HAPUS TUJUAN", 12f, Color.BLACK, "gilroy_bold", bold = true).apply {
            gravity = Gravity.CENTER
            setPadding(dpi(16f), 0, dpi(16f), 0)
            background = ModernUi.pressable(
                ModernUi.rect(Color.argb(0xF2, 255, 255, 255)), ModernUi.rect(ModernUi.ACCENT))
            isClickable = true
        }
        mapLayout.addView(waypoint, FrameLayout.LayoutParams(wrap, dpi(36f), Gravity.BOTTOM or Gravity.END).apply {
            marginEnd = dpi(28f)
            bottomMargin = dpi(40f)
        })

        val hint = ModernUi.text(ctx,
            "Geser untuk menggeser  \u2022  Cubit untuk zoom  \u2022  Tanda di titik tengah",
            11f, Color.argb(0xE6, 255, 255, 255)).apply {
            background = ModernUi.rect(ModernUi.PANEL)
            setPadding(dpi(10f), dpi(5f), dpi(10f), dpi(5f))
        }
        mapLayout.addView(hint, FrameLayout.LayoutParams(wrap, wrap, Gravity.BOTTOM or Gravity.START).apply {
            marginStart = dpi(28f)
            bottomMargin = dpi(44f)
        })
        rootLayout.addView(mapLayout, FrameLayout.LayoutParams(match, match))

        // ---- frame: header, tabs, page, footer
        val frameLayout = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dpi(28f), 0, dpi(28f), dpi(10f))
        }
        frame = frameLayout

        val headerLayout = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.BOTTOM
            setPadding(0, dpi(10f), 0, 0)
        }
        header = headerLayout
        serverName = ModernUi.text(ctx, "", 30f, Color.WHITE, "pricedownbl", bold = true).apply {
            ellipsize = android.text.TextUtils.TruncateAt.END
            setShadowLayer(4f, 0f, 0f, Color.BLACK)
        }
        headerLayout.addView(serverName, LinearLayout.LayoutParams(0, wrap, 1f))

        val headerRight = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.END
        }
        playerName = ModernUi.text(ctx, "", 16f, Color.WHITE, "gilroy_bold", bold = true).apply {
            setShadowLayer(3f, 0f, 0f, Color.BLACK)
        }
        headerRight.addView(playerName, LinearLayout.LayoutParams(wrap, wrap))
        val headerInfo = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        val clock = android.widget.TextClock(ctx).apply {
            format12Hour = "EEEE HH:mm"
            format24Hour = "EEEE HH:mm"
            isAllCaps = true
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
            setTextColor(Color.argb(0xE6, 255, 255, 255))
            rowFont?.let { typeface = it }
            setShadowLayer(3f, 0f, 0f, Color.BLACK)
        }
        headerInfo.addView(clock, LinearLayout.LayoutParams(wrap, wrap))
        playerMoney = ModernUi.text(ctx, "", 11f, Color.rgb(0x72, 0xCC, 0x72), "gilroy_bold", bold = true).apply {
            setShadowLayer(3f, 0f, 0f, Color.BLACK)
        }
        headerInfo.addView(playerMoney, LinearLayout.LayoutParams(wrap, wrap).apply { marginStart = dpi(10f) })
        headerRight.addView(headerInfo, LinearLayout.LayoutParams(wrap, wrap))
        headerLayout.addView(headerRight, LinearLayout.LayoutParams(wrap, wrap))
        frameLayout.addView(headerLayout, LinearLayout.LayoutParams(match, wrap))

        tabsBar = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        frameLayout.addView(tabsBar, LinearLayout.LayoutParams(match, dpi(32f)).apply { topMargin = dpi(6f) })

        val listLayout = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        listPage = listLayout
        rowsScroll = ScrollView(ctx).apply {
            // Visible: the GRAFIS tab is longer than a landscape phone screen.
            isVerticalScrollBarEnabled = true
            overScrollMode = View.OVER_SCROLL_NEVER
            isVerticalFadingEdgeEnabled = true
            setFadingEdgeLength(dpi(12f))
        }
        rowsLayout = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
        rowsScroll.addView(rowsLayout, FrameLayout.LayoutParams(match, wrap))
        listLayout.addView(rowsScroll, LinearLayout.LayoutParams(0, match, 0.6f))

        val side = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            background = ModernUi.rect(ModernUi.PANEL)
            // The panel must not pass touches to the game either.
            isClickable = true
        }
        side.addView(View(ctx).apply { setBackgroundColor(ModernUi.ACCENT) }, LinearLayout.LayoutParams(match, dpi(3f)))
        val sideInner = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dpi(12f), dpi(12f), dpi(12f), dpi(12f))
        }
        val playerBlock = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
        sidePlayer = playerBlock
        sidePlayerName = ModernUi.text(ctx, "", 15f, Color.WHITE, "gilroy_bold", bold = true).apply {
            ellipsize = android.text.TextUtils.TruncateAt.END
        }
        playerBlock.addView(sidePlayerName, LinearLayout.LayoutParams(match, wrap))
        sidePlayerStats = ModernUi.text(ctx, "", 12f, Color.argb(0xCC, 255, 255, 255)).apply {
            maxLines = 4
            setLineSpacing(dp(3f), 1f)
        }
        playerBlock.addView(sidePlayerStats, LinearLayout.LayoutParams(match, wrap).apply { topMargin = dpi(4f) })
        playerBlock.addView(View(ctx).apply { setBackgroundColor(Color.argb(0x33, 255, 255, 255)) },
            LinearLayout.LayoutParams(match, dpi(1f)).apply { topMargin = dpi(10f) })
        sideInner.addView(playerBlock, LinearLayout.LayoutParams(match, wrap).apply { bottomMargin = dpi(10f) })
        sideTitle = ModernUi.text(ctx, "", 14f, Color.WHITE, "gilroy_bold", bold = true).apply { maxLines = 2 }
        sideInner.addView(sideTitle, LinearLayout.LayoutParams(match, wrap))
        sideBody = ModernUi.text(ctx, "", 12f, Color.argb(0xCC, 255, 255, 255)).apply {
            maxLines = 8
            setLineSpacing(dp(2f), 1f)
        }
        sideInner.addView(sideBody, LinearLayout.LayoutParams(match, wrap).apply { topMargin = dpi(6f) })
        side.addView(sideInner, LinearLayout.LayoutParams(match, wrap))
        listLayout.addView(side, LinearLayout.LayoutParams(0, wrap, 0.4f).apply { marginStart = dpi(8f) })
        frameLayout.addView(listLayout, LinearLayout.LayoutParams(match, 0, 1f).apply { topMargin = dpi(6f) })

        mapSpace = View(ctx).apply { visibility = View.GONE }
        frameLayout.addView(mapSpace, LinearLayout.LayoutParams(match, 0, 1f))

        val footer = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.END or Gravity.CENTER_VERTICAL
        }
        val close = ModernUi.text(ctx, "KEMBALI   Tutup menu", 11f, Color.WHITE, "gilroy_bold", bold = true).apply {
            gravity = Gravity.CENTER
            setPadding(dpi(10f), 0, dpi(10f), 0)
            background = ModernUi.pressable(ModernUi.rect(ModernUi.PANEL), ModernUi.rect(ModernUi.ACCENT))
            isClickable = true
        }
        footer.addView(close, LinearLayout.LayoutParams(wrap, dpi(26f)))
        frameLayout.addView(footer, LinearLayout.LayoutParams(match, wrap).apply { topMargin = dpi(6f) })
        rootLayout.addView(frameLayout, FrameLayout.LayoutParams(match, match))

        close.setOnClickListener { closeMenu() }
        locate.setOnClickListener { ModernMenu.request(ModernMenu.REQUEST_CENTER_PLAYER) }
        waypoint.setOnClickListener { ModernMenu.request(ModernMenu.REQUEST_TOGGLE_WAYPOINT) }
        bindRepeat(zoomIn, ModernMenu.REQUEST_ZOOM_IN)
        bindRepeat(zoomOut, ModernMenu.REQUEST_ZOOM_OUT)

        buildTabs()
    }

    private fun dpi(value: Float): Int = (value * density + 0.5f).toInt()

    // ------------------------------------------------------------- show/hide

    fun show(newInfo: ModernMenu.PlayerInfo) {
        val parent = ModernMenu.container() ?: return
        info = newInfo
        bindHeader()

        if (hiding) {
            // Shown again while fading out: finish the pending actions now.
            root.animate().cancel()
            hiding = false
            val actions = pendingActions.toList()
            pendingActions.clear()
            actions.forEach { it() }
        }

        if (root.parent === parent) {
            // Already open: back from the native map, or it could not open.
            ModernMenu.readSettings()?.let { settings = it }
            root.alpha = 1f
            if (currentTab == TabId.MAP) selectTab(TabId.GAME, notifyNative = false)
            return
        }

        (root.parent as? ViewGroup)?.removeView(root)
        parent.addView(root)
        root.bringToFront()
        root.visibility = View.VISIBLE

        settings = ModernMenu.readSettings()
        selectTab(TabId.GAME, notifyNative = false, rebuild = true)

        frame.animate().cancel()
        root.alpha = 0f
        frame.translationY = -dp(12f)
        val interpolator = DecelerateInterpolator(2f)
        root.animate().alpha(1f).setDuration(160).setInterpolator(interpolator).start()
        frame.animate().translationY(0f).setDuration(220).setInterpolator(interpolator).start()
    }

    /** Hides the menu; [then] runs once it is gone. */
    fun hide(then: (() -> Unit)?) {
        if (root.parent == null) {
            then?.invoke()
            return
        }
        if (then != null) pendingActions.add(then)
        if (hiding) return

        hiding = true
        root.animate().alpha(0f).setDuration(140).withEndAction {
            (root.parent as? ViewGroup)?.removeView(root)
            hiding = false
            val actions = pendingActions.toList()
            pendingActions.clear()
            actions.forEach { it() }
        }.start()
    }

    /** Opened by tapping the radar: go straight to the map tab. */
    fun openMapTab() {
        if (root.parent == null || hiding) return
        if (currentTab != TabId.MAP) selectTab(TabId.MAP, notifyNative = true)
    }

    /** The native map closed or failed to open; leave the map tab. */
    fun onNativeMapClosed() {
        if (root.parent != null && currentTab == TabId.MAP) selectTab(TabId.GAME, notifyNative = false)
    }

    private fun closeMenu() {
        if (hiding) return
        if (currentTab == TabId.MAP) ModernMenu.closeFromMap() else ModernMenu.resumeGame()
    }

    private fun bindHeader() {
        val server = readServerName()
        serverName.text = if (server.isNotBlank()) server else "San Andreas Multiplayer"
        playerName.text = info.name
        playerMoney.text = formatMoney(info.money)
        sidePlayerName.text = info.name
        sidePlayerStats.text = String.format(
            Locale.US, "ID %d   •   Skor %d\nPing %d ms   •   Uang %s",
            info.id, info.score, info.ping, formatMoney(info.money)
        )
    }

    // ------------------------------------------------------------------ tabs

    private fun buildTabs() {
        tabsBar.removeAllViews()
        tabs.clear()
        for (id in TabId.values()) {
            val tab = FrameLayout(activity)
            val text = TextView(activity).apply {
                text = id.title
                gravity = Gravity.CENTER
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
                typeface = boldFont ?: Typeface.DEFAULT_BOLD
                letterSpacing = 0.06f
            }
            val strip = View(activity).apply { setBackgroundColor(ACCENT) }
            tab.addView(text, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
            tab.addView(strip, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(3f).toInt(), Gravity.TOP))
            tab.isClickable = true
            tab.setOnClickListener {
                try {
                    if (!hiding) selectTab(id, notifyNative = true)
                } catch (t: Throwable) {
                    t.printStackTrace()
                }
            }

            val lp = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f)
            if (id != TabId.values().last()) lp.marginEnd = dp(2f).toInt()
            tabsBar.addView(tab, lp)
            tabs.add(TabViews(id, tab, text, strip))
        }
    }

    private fun selectTab(id: TabId, notifyNative: Boolean, rebuild: Boolean = false) {
        val previous = currentTab
        currentTab = id

        for (tab in tabs) {
            val selected = tab.id == id
            tab.view.setBackgroundColor(if (selected) Color.WHITE else PANEL)
            tab.text.setTextColor(if (selected) Color.BLACK else Color.WHITE)
            tab.strip.visibility = if (selected) View.VISIBLE else View.INVISIBLE
        }

        if (notifyNative && previous != id) {
            if (id == TabId.MAP) ModernMenu.request(ModernMenu.REQUEST_OPEN_MAP)
            else if (previous == TabId.MAP) ModernMenu.request(ModernMenu.REQUEST_MAP_LEAVE)
        }

        val map = id == TabId.MAP
        // On the map tab the game gets every touch outside the buttons.
        root.isClickable = !map
        scrim.visibility = if (map) View.GONE else View.VISIBLE
        mapPage.visibility = if (map) View.VISIBLE else View.GONE
        listPage.visibility = if (map) View.GONE else View.VISIBLE
        mapSpace.visibility = if (map) View.VISIBLE else View.GONE
        header.background = if (map) {
            ModernUi.gradient(Color.argb(0xD9, 0, 0, 0), Color.TRANSPARENT)
        } else {
            null
        }

        if (!map && (rebuild || previous != id || rows.isEmpty())) {
            selectedRow = 0
            exitArmedUntil = 0L
            buildRows()
            rowsScroll.scrollTo(0, 0)
        }
    }

    // ------------------------------------------------------------------ rows

    private fun rowsFor(tab: TabId): List<Row> = when (tab) {
        TabId.MAP -> emptyList()
        TabId.GAME -> gameRows()
        TabId.GRAPHICS -> settingRows(
            option(GtaSettings.VISUALS, "Kualitas visual",
                "Kualitas efek grafis secara umum (shader, partikel, air). Lebih tinggi lebih bagus, tetapi lebih berat.",
                listOf("Rendah", "Sedang", "Tinggi", "Sangat tinggi")),
            slider(GtaSettings.RESOLUTION, "Resolusi (render scale)",
                "Resolusi render 3D. Diterapkan saat jari dilepas.", live = false),
            slider(GtaSettings.DRAW_DISTANCE, "Jarak pandang",
                "Seberapa jauh bangunan dan object digambar. Diterapkan saat jari dilepas.", live = false),
            option(GtaSettings.SHADOWS, "Bayangan",
                "Kualitas bayangan karakter, kendaraan dan object.",
                listOf("Mati", "Klasik", "Real-time")),
            option(GtaSettings.CAR_REFLECTIONS, "Refleksi kendaraan",
                "Pantulan lingkungan pada bodi kendaraan.",
                listOf("Mati", "Rendah", "Sedang", "Tinggi")),
            option(GtaSettings.TRAFFIC, "Kepadatan populasi",
                "Jumlah pejalan kaki dan kendaraan GTA (server SA-MP bisa membatasi).",
                listOf("Rendah", "Sedang", "Tinggi")),
            slider(GtaSettings.BRIGHTNESS, "Kecerahan", "Kecerahan layar game."),
            option(GtaSettings.FRAME_LIMITER, "Pembatas frame",
                "Batasi frame rate supaya lebih stabil dan hemat baterai.", ON_OFF),
            header("GRAFIS KLIEN", "Sistem grafis client: bayangan matahari, PostFX, langit, jalan basah."),
            option(GtaSettings.GFX_PRESET, "Preset grafis",
                "Mengatur semua pilihan di bawah sekaligus. Berubah ke Kustom bila satu nilai diganti.",
                listOf("Rendah", "Sedang", "Tinggi", "Ultra", "Kustom")),
            option(GtaSettings.GFX_POSTFX, "Post FX",
                "Bloom, tone mapping, warna, kabut dan efek lain setelah dunia digambar. HUD/chat tidak terpengaruh.", ON_OFF),
            option(GtaSettings.GFX_SUN_SHADOWS, "Bayangan matahari",
                "Bayangan realtime dari matahari untuk gedung, pohon, kendaraan, karakter dan object.", ON_OFF),
            option(GtaSettings.GFX_SHADOW_QUALITY, "Resolusi bayangan",
                "Ukuran peta bayangan. Lebih tinggi lebih tajam tetapi lebih berat.",
                listOf("512", "1024", "2048", "4096")),
            slider(GtaSettings.GFX_SHADOW_DISTANCE, "Jarak bayangan",
                "Jangkauan bayangan matahari dari kamera (bayangan panjang).", percent = false, unit = " m"),
            option(GtaSettings.GFX_SOFT_SHADOWS, "Bayangan lembut",
                "Kehalusan tepi bayangan.", listOf("Tajam", "Lembut", "Sangat lembut")),
            option(GtaSettings.GFX_AO, "Ambient occlusion",
                "Bayangan halus di sudut dan celah (SSAO).", ON_OFF),
            option(GtaSettings.GFX_REFLECTIONS, "Refleksi layar",
                "Pantulan screen-space, kuat pada jalan basah.", ON_OFF),
            slider(GtaSettings.GFX_SUN_RAYS, "Sinar matahari",
                "God rays dan lens flare. Terhalang gedung, pohon dan kendaraan."),
            slider(GtaSettings.GFX_BLOOM, "Bloom", "Cahaya pendar dari lampu dan langit terang."),
            slider(GtaSettings.GFX_SKY, "Langit realtime",
                "Warna langit dan cahaya matahari mengikuti jam dan cuaca."),
            slider(GtaSettings.GFX_WET_ROADS, "Jalan basah",
                "Jalan lebih gelap, genangan dan pantulan saat hujan.", percent = false, unit = "%"),
            option(GtaSettings.GFX_WEATHER, "Grafis ikut cuaca",
                "Warna, kabut dan langit berubah realtime sesuai cuaca GTA.", ON_OFF),
            slider(GtaSettings.GFX_FOG, "Kabut", "Kekuatan kabut jarak jauh."),
            option(GtaSettings.GFX_GRASS, "Rumput",
                "Rumput dan tanaman mengikuti kamera. Mati = FPS lebih tinggi.", ON_OFF),
            ActionRow("Simpan pengaturan grafis",
                "Simpan sekarang ke TESTLIT/SAMP/settings.ini (juga tersimpan otomatis).") { saveGraphics(it) },
            ActionRow("Reset grafis ke default", "Kembalikan grafis klien ke preset Sedang.") { resetGraphics() }
        )
        TabId.AUDIO -> settingRows(
            slider(GtaSettings.SFX_VOLUME, "Volume efek suara",
                "Volume suara tembakan, mesin, langkah dan lainnya."),
            slider(GtaSettings.MUSIC_VOLUME, "Volume radio", "Volume radio kendaraan dan musik."),
            option(GtaSettings.AUTOTUNE, "Radio auto-tune",
                "Radio kendaraan langsung memutar stasiun terakhir.", ON_OFF),
            option(GtaSettings.SUBTITLES, "Subtitle", "Tampilkan subtitle dialog.", ON_OFF)
        )
        TabId.CONTROLS -> listOf<Row>(editGtaControlsRow()) + settingRows(
            option(GtaSettings.TARGETING, "Mode bidik",
                "Lock-on: bidikan menempel ke target. Bidik bebas: bidik manual.",
                listOf("Lock-on", "Bidik bebas")),
            option(GtaSettings.STEER_TYPE, "Metode setir", "Cara mengendalikan kendaraan.",
                listOf("Analog", "Digital", "Flick")),
            slider(GtaSettings.STEER_ANALOG_SCALE, "Sensitivitas setir",
                "Sensitivitas setir analog.", percent = false),
            option(GtaSettings.ACCELEROMETER, "Akselerometer",
                "Setir dengan memiringkan HP.", listOf("Mati", "Rendah", "Tinggi")),
            option(GtaSettings.TOUCH_LAYOUT, "Tata letak sentuh",
                "Susunan tombol sentuh bawaan GTA.", listOf("Klasik", "Adaptif")),
            option(GtaSettings.CAM_HEIGHT, "Tinggi kamera",
                "Posisi kamera di belakang karakter.", listOf("Rendah", "Tinggi")),
            option(GtaSettings.INVERT_LOOK, "Balik kamera vertikal",
                "Geser ke atas untuk melihat ke bawah.", ON_OFF),
            option(GtaSettings.VIBRATION, "Getaran", "Getaran saat tertembak atau menabrak.", ON_OFF),
            option(GtaSettings.AUTO_CLIMB, "Panjat otomatis",
                "Karakter memanjat pagar dan dinding rendah otomatis.", ON_OFF)
        )
    }

    private fun editGtaControlsRow(): Row = ActionRow(
        "Edit tombol kontrol GTA",
        "Atur posisi dan ukuran tombol lari, lompat, tembak, masuk mobil, dll. " +
            "Menu ini muncul lagi setelah selesai."
    ) { ModernMenu.openGtaControls() }

    private fun gameRows(): List<Row> = listOf(
        ActionRow("Lanjutkan", "Tutup menu dan kembali ke permainan.") { ModernMenu.resumeGame() },
        editGtaControlsRow(),
        ActionRow("Edit radar & chat",
            "Geser, ubah ukuran dan transparansi radar, chat, uang dan tombol RP.") {
            ModernMenu.openLayoutEditor()
        },
        ActionRow("Pengaturan klien", "FPS, chat dan tampilan klien SA-MP.") { ModernMenu.openClientSettings() },
        ActionRow("Keluar dari game", "Keluar dari game. Ketuk dua kali untuk konfirmasi.") { onExitClicked(it) }
    )

    private fun option(setting: Int, label: String, desc: String, options: List<String>): Row? =
        if (isAvailable(setting)) OptionRow(label, desc, setting, options) else null

    private fun slider(setting: Int, label: String, desc: String, percent: Boolean = true,
                       live: Boolean = true, unit: String = ""): Row? =
        if (isAvailable(setting)) SliderRow(label, desc, setting, percent, live, unit) else null

    /** Section title: not selectable, no action. */
    private fun header(label: String, desc: String): Row = ActionRow(label, desc) {}.also { it.isHeader = true }

    private fun saveGraphics(row: ActionRow) {
        ModernMenu.request(ModernMenu.REQUEST_GFX_SAVE)
        row.descOverride = "Tersimpan."
        refreshRow(row)
        root.postDelayed({ row.descOverride = null; refreshRow(row) }, 1500L)
    }

    private fun resetGraphics() {
        ModernMenu.request(ModernMenu.REQUEST_GFX_RESET)
        refreshSettingsSoon()
    }

    /** Preset/reset change many values natively: read them back after the game thread applied them. */
    private fun refreshSettingsSoon() {
        root.postDelayed({
            if (root.parent == null || hiding) return@postDelayed
            ModernMenu.readSettings()?.let { settings = it }
            rows.forEachIndexed { i, views -> bindRow(views, i == selectedRow) }
        }, 250L)
    }

    private fun settingRows(vararg rows: Row?): List<Row> {
        val list = rows.filterNotNull()
        if (list.isEmpty()) {
            return listOf(ActionRow("Pengaturan tidak tersedia",
                "Client belum mendukung pengaturan dari menu ini.") {})
        }
        return list
    }

    private fun isAvailable(setting: Int): Boolean {
        val s = settings ?: return false
        val base = setting * 4
        if (base + 3 >= s.size) return false
        return s[base + 3] != 0 && s[base + 2] > s[base + 1]
    }

    private fun settingValue(setting: Int) = settings?.getOrNull(setting * 4) ?: 0
    private fun settingMin(setting: Int) = settings?.getOrNull(setting * 4 + 1) ?: 0
    private fun settingMax(setting: Int) = settings?.getOrNull(setting * 4 + 2) ?: 0

    private var lastSendMs = 0L

    /**
     * send = false only updates the shown value (live = false slider while
     * dragging). force = true resends the same value (finger-up), so a request
     * dropped during the drag cannot leave the game on another value.
     */
    private fun setSettingValue(setting: Int, value: Int, send: Boolean = true, force: Boolean = false) {
        val s = settings ?: return
        val v = value.coerceIn(settingMin(setting), settingMax(setting))
        if (s[setting * 4] == v && !force) return
        s[setting * 4] = v
        if (!send) return
        lastSendMs = System.currentTimeMillis()
        ModernMenu.setSetting(setting, v)
        if (setting == GtaSettings.GFX_PRESET) refreshSettingsSoon()
        else if (setting >= GtaSettings.GFX_BASE) refreshPresetSoon()
    }

    /** Changing one client graphics value switches the preset row to Kustom natively. */
    private fun refreshPresetSoon() = refreshSettingsSoon()

    private fun buildRows() {
        rowsLayout.removeAllViews()
        rows.clear()
        for (row in rowsFor(currentTab)) {
            val views = createRow(row)
            // Minimum height, not fixed: text is not clipped with a large system font.
            val lp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            views.view.minimumHeight = dp(ROW_HEIGHT_DP).toInt()
            lp.bottomMargin = dp(2f).toInt()
            rowsLayout.addView(views.view, lp)
            rows.add(views)
        }
        selectedRow = selectedRow.coerceIn(0, (rows.size - 1).coerceAtLeast(0))
        rows.forEachIndexed { i, views -> bindRow(views, i == selectedRow) }
        updateSide()
    }

    private fun createRow(row: Row): RowViews {
        val layout = LinearLayout(activity)
        layout.orientation = LinearLayout.HORIZONTAL
        layout.gravity = Gravity.CENTER_VERTICAL
        layout.setPadding(dp(12f).toInt(), 0, dp(4f).toInt(), 0)
        layout.isClickable = true

        val label = text(row.label, 13f)
        layout.addView(label, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))

        var value: TextView? = null
        var left: TextView? = null
        var right: TextView? = null
        var slider: GtaSliderView? = null

        when (row) {
            is OptionRow -> {
                left = arrow("‹")
                value = text("", 13f).apply {
                    gravity = Gravity.CENTER
                    minWidth = dp(96f).toInt()
                    isClickable = true
                }
                right = arrow("›")
                layout.addView(left, LinearLayout.LayoutParams(dp(30f).toInt(), ViewGroup.LayoutParams.MATCH_PARENT))
                layout.addView(value, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.MATCH_PARENT))
                layout.addView(right, LinearLayout.LayoutParams(dp(30f).toInt(), ViewGroup.LayoutParams.MATCH_PARENT))
            }
            is SliderRow -> {
                slider = GtaSliderView(activity).apply {
                    this.min = settingMin(row.setting)
                    this.max = settingMax(row.setting)
                    this.value = settingValue(row.setting)
                    setPadding(dp(4f).toInt(), 0, dp(4f).toInt(), 0)
                }
                value = text("", 12f).apply { gravity = Gravity.END or Gravity.CENTER_VERTICAL }
                layout.addView(slider, LinearLayout.LayoutParams(dp(120f).toInt(), ViewGroup.LayoutParams.MATCH_PARENT))
                layout.addView(value, LinearLayout.LayoutParams(dp(44f).toInt(), ViewGroup.LayoutParams.WRAP_CONTENT))
            }
            is ActionRow -> {
                right = arrow("›").apply { isClickable = false }
                layout.addView(right, LinearLayout.LayoutParams(dp(30f).toInt(), ViewGroup.LayoutParams.MATCH_PARENT))
            }
        }

        val views = RowViews(row, layout, label, value, left, right, slider)

        layout.setOnClickListener { onRowTapped(views, 1, fromControl = false) }
        if (row is OptionRow) {
            left?.setOnClickListener { onRowTapped(views, -1, fromControl = true) }
            right?.setOnClickListener { onRowTapped(views, 1, fromControl = true) }
            value?.setOnClickListener { tappedValue = true; onRowTapped(views, 1, fromControl = true) }
        }
        if (row is SliderRow) {
            slider?.onValueChanged = { v ->
                select(views)
                // Live rows: at most ~8 requests per second while dragging.
                val throttle = System.currentTimeMillis() - lastSendMs < SLIDER_SEND_INTERVAL_MS
                setSettingValue(row.setting, v, send = row.live && !throttle)
                bindRow(views, true)
            }
            slider?.onValueCommitted = { v ->
                setSettingValue(row.setting, v, force = true)
                bindRow(views, rows.indexOf(views) == selectedRow)
            }
        }
        return views
    }

    /**
     * Action rows run on tap. Setting rows: the arrows and the value change
     * it straight away; a tap on the label selects the row first (like a
     * controller) and changes the value on the next tap.
     */
    private fun onRowTapped(views: RowViews, direction: Int, fromControl: Boolean) {
        try {
            handleRowTap(views, direction, fromControl)
        } catch (t: Throwable) {
            // A UI error must never close the game.
            t.printStackTrace()
        }
    }

    private fun handleRowTap(views: RowViews, direction: Int, fromControl: Boolean) {
        if (hiding) return
        if ((views.row as? ActionRow)?.isHeader == true) return
        val alreadySelected = rows.indexOf(views) == selectedRow
        select(views)
        when (val row = views.row) {
            is ActionRow -> row.action(row)
            is OptionRow -> {
                if (!fromControl && !alreadySelected) return
                val min = settingMin(row.setting)
                val max = settingMax(row.setting)
                var next = settingValue(row.setting) + direction
                if (fromControl && !tappedValue) {
                    // Arrows stop at the ends (Real-time -> Mati by one tap was a trap).
                    next = next.coerceIn(min, max)
                } else {
                    // A tap on the label/value cycles.
                    if (next > max) next = min
                    if (next < min) next = max
                }
                tappedValue = false
                if (next == settingValue(row.setting)) return
                setSettingValue(row.setting, next)
                bindRow(views, true)
            }
            is SliderRow -> Unit  // the bar handles its own touches
        }
    }

    private fun select(views: RowViews) {
        val index = rows.indexOf(views)
        if (index < 0 || index == selectedRow) return
        rows.getOrNull(selectedRow)?.let { bindRow(it, false) }
        selectedRow = index
        bindRow(views, true)
        updateSide()
    }

    private fun bindRow(views: RowViews, selected: Boolean) {
        val fg = if (selected) Color.BLACK else Color.WHITE
        views.view.setBackgroundColor(if (selected) Color.WHITE else ROW)
        views.label.setTextColor(fg)
        views.arrowLeft?.setTextColor(fg)
        views.arrowRight?.setTextColor(
            if (views.row is ActionRow && !selected) Color.argb(120, 255, 255, 255) else fg
        )
        views.value?.setTextColor(fg)
        views.slider?.inverted = selected

        when (val row = views.row) {
            is OptionRow -> {
                val index = settingValue(row.setting) - settingMin(row.setting)
                views.value?.text = row.options.getOrNull(index) ?: settingValue(row.setting).toString()
            }
            is SliderRow -> {
                val v = settingValue(row.setting)
                views.value?.text = if (row.percent) {
                    val range = (settingMax(row.setting) - settingMin(row.setting)).coerceAtLeast(1)
                    "${(v - settingMin(row.setting)) * 100 / range}%"
                } else {
                    "$v${row.unit}"
                }
                if (views.slider?.value != v) views.slider?.value = v
            }
            is ActionRow -> {
                if (row.isHeader) {
                    views.view.setBackgroundColor(Color.TRANSPARENT)
                    views.label.setTextColor(ACCENT)
                    views.arrowRight?.visibility = View.INVISIBLE
                } else if (row.descOverride != null) {
                    views.label.setTextColor(if (selected) DANGER_DARK else DANGER)
                }
            }
        }
    }

    private fun updateSide() {
        sidePlayer.visibility = if (currentTab == TabId.GAME) View.VISIBLE else View.GONE
        val views = rows.getOrNull(selectedRow)
        if (views == null) {
            sideTitle.text = ""
            sideBody.text = ""
            return
        }
        val row = views.row
        sideTitle.text = row.label
        sideBody.text = if (row is ActionRow) row.descOverride ?: row.desc else row.desc
    }

    private fun onExitClicked(row: ActionRow) {
        val now = System.currentTimeMillis()
        if (now < exitArmedUntil) {
            ModernMenu.exitGame()
            return
        }
        exitArmedUntil = now + EXIT_CONFIRM_MS
        row.descOverride = "Ketuk sekali lagi untuk keluar dari game."
        refreshRow(row)
        root.postDelayed({
            if (System.currentTimeMillis() >= exitArmedUntil && row.descOverride != null) {
                row.descOverride = null
                refreshRow(row)
            }
        }, EXIT_CONFIRM_MS)
    }

    private fun refreshRow(row: Row) {
        val index = rows.indexOfFirst { it.row === row }
        if (index < 0) return
        bindRow(rows[index], index == selectedRow)
        updateSide()
    }

    // --------------------------------------------------------------- helpers

    private fun text(value: String, sizeSp: Float) = TextView(activity).apply {
        text = value
        setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp)
        maxLines = 1
        if (rowFont != null) typeface = rowFont
    }

    private fun arrow(symbol: String) = TextView(activity).apply {
        text = symbol
        gravity = Gravity.CENTER
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
        isClickable = true
    }

    /** Zoom buttons: one step per tap, repeated while held. */
    @SuppressLint("ClickableViewAccessibility")
    private fun bindRepeat(button: View, request: Int) {
        val repeat = object : Runnable {
            override fun run() {
                ModernMenu.request(request)
                button.postDelayed(this, REPEAT_INTERVAL_MS)
            }
        }
        button.setOnTouchListener { view, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    view.isPressed = true
                    ModernMenu.request(request)
                    view.postDelayed(repeat, REPEAT_DELAY_MS)
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    view.isPressed = false
                    view.removeCallbacks(repeat)
                }
            }
            true
        }
    }

    private fun readServerName(): String {
        return try {
            com.holy.launcher.storage.Storage.getProperty(
                com.holy.launcher.domain.enums.StorageElements.SERVER_NAME,
                activity
            ) ?: ""
        } catch (_: Throwable) {
            ""
        }
    }

    private fun formatMoney(value: Int): String =
        try {
            String.format(Locale.US, "$%,d", value).replace(",", ".")
        } catch (_: Exception) {
            "$$value"
        }

    private fun dp(value: Float): Float = value * density

    private companion object {
        const val EXIT_CONFIRM_MS = 3000L
        const val REPEAT_DELAY_MS = 350L
        const val REPEAT_INTERVAL_MS = 110L
        const val ROW_HEIGHT_DP = 36f
        const val SLIDER_SEND_INTERVAL_MS = 120L
        val ON_OFF = listOf("Mati", "Nyala")
        val ACCENT = Color.rgb(0x47, 0xA5, 0xE5)
        val PANEL = Color.argb(0xB3, 0, 0, 0)
        val ROW = Color.argb(0x99, 0, 0, 0)
        val DANGER = Color.rgb(0xFF, 0x6B, 0x6B)
        val DANGER_DARK = Color.rgb(0xC0, 0x1E, 0x1E)
    }
}
