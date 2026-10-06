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
import com.holy.game.core.GraphicsNative
import java.util.Locale
import kotlin.math.roundToInt

/**
 * Pause menu in the style of mobile battle royale settings screens.
 *
 * Top bar with the server, player (ID, score, ping, money), clock and close
 * button; a tab rail on the left with a big CONTINUE button; setting rows with
 * option chips and sliders on the right; a hint bar for the selected row.
 *
 * GTA SA settings (graphics, audio, controls) are written straight into the
 * game's MobileSettings through the client, like before. The EAGLE graphics
 * engine (sun shadows, shaderUniform.ini) is controlled through
 * GraphicsNative: GRAFIS has the main switches, EFEK the live shader values.
 * The PETA tab shows the native GTA map under the top bar and the rail.
 *
 * Built in code on purpose: see ModernUi (no new resources).
 */
class ModernPauseMenu(private val activity: Activity) {

    private enum class TabId(val title: String, val glyph: String, val subtitle: String) {
        MAP("PETA", "◎", "Peta San Andreas"),
        GAME("GAME", "▶", "Permainan"),
        GRAPHICS("GRAFIS", "◆", "GTA SA  •  EAGLE"),
        EFFECTS("EFEK", "✦", "shaderUniform.ini  •  langsung aktif"),
        AUDIO("AUDIO", "♪", "Suara dan radio"),
        CONTROLS("KONTROL", "✥", "Sentuh, setir dan kamera")
    }

    // ------------------------------------------------------------------ rows

    private abstract class Row(val label: String, val desc: String) {
        open val selectable = true
    }

    private class SectionRow(title: String) : Row(title, "") {
        override val selectable = false
    }

    private class ActionRow(
        label: String,
        desc: String,
        val danger: Boolean = false,
        val action: (ActionRow) -> Unit
    ) : Row(label, desc) {
        var descOverride: String? = null
        var descProvider: (() -> String)? = null
    }

    /** Named values; [get] / [set] use the index into [options]. */
    private class ChoiceRow(
        label: String,
        desc: String,
        val options: List<String>,
        val get: () -> Int,
        val set: (Int) -> Unit
    ) : Row(label, desc)

    /** A value on a bar; [set] gets committed = true when the finger is lifted. */
    private class RangeRow(
        label: String,
        desc: String,
        val min: Float,
        val max: Float,
        val step: Float,
        val format: (Float) -> String,
        val get: () -> Float,
        val set: (Float, Boolean) -> Unit
    ) : Row(label, desc)

    private class RowViews(
        val row: Row,
        val view: LinearLayout,
        val bar: View?,
        val label: TextView,
        val chips: List<TextView>,
        val value: TextView?,
        val arrowLeft: TextView?,
        val arrowRight: TextView?,
        val slider: MenuStyle.SliderView?
    )

    private class TabViews(val id: TabId, val view: LinearLayout, val glyph: TextView, val text: TextView, val bar: View)

    // ----------------------------------------------------------------- views

    private val root: View
    private val scrim: View
    private val frame: View
    private val topBar: View
    private val railTabs: LinearLayout
    private val content: View
    private val pageTitle: TextView
    private val pageSubtitle: TextView
    private val mapPage: View
    private val rowsScroll: ScrollView
    private val rowsLayout: LinearLayout
    private val hintTitle: TextView
    private val hintBody: TextView
    private val serverName: TextView
    private val playerName: TextView
    private val playerStats: TextView
    private val playerMoney: TextView

    private val tabs = ArrayList<TabViews>()
    private val rows = ArrayList<RowViews>()
    private var currentTab = TabId.GAME
    private var selectedRow = -1
    private var info = ModernMenu.PlayerInfo("", 0, 0, 0, 0)

    /** GTA settings snapshot: value, min, max, visible per setting. */
    private var settings: IntArray? = null

    /** shaderUniform.ini values shown in EFEK ("class\u0000name" -> value). */
    private val uniformValues = HashMap<String, Float>()
    private var eagleOk: Boolean? = null

    private var exitArmedUntil = 0L
    private var hiding = false
    private val pendingActions = mutableListOf<() -> Unit>()

    private val density = activity.resources.displayMetrics.density

    init {
        val ctx = activity
        val match = ViewGroup.LayoutParams.MATCH_PARENT
        val wrap = ViewGroup.LayoutParams.WRAP_CONTENT

        val rootLayout = FrameLayout(ctx).apply { isMotionEventSplittingEnabled = false }
        root = rootLayout

        scrim = View(ctx).apply {
            background = ModernUi.gradient3(MenuStyle.SCRIM_TOP, MenuStyle.SCRIM_MIDDLE, MenuStyle.SCRIM_BOTTOM)
        }
        rootLayout.addView(scrim, FrameLayout.LayoutParams(match, match))

        // ---- map tab: controls over the native map (root does not take touches)
        val mapLayout = FrameLayout(ctx).apply { visibility = View.GONE }
        mapPage = mapLayout
        mapLayout.addView(ModernUi.CrosshairView(ctx), FrameLayout.LayoutParams(dpi(36f), dpi(36f), Gravity.CENTER))
        val zoomColumn = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
        }
        val zoomIn = squareButton("+")
        val zoomOut = squareButton("−")
        val locate = squareButton("◎")
        zoomColumn.addView(zoomIn, LinearLayout.LayoutParams(dpi(46f), dpi(46f)))
        zoomColumn.addView(zoomOut, LinearLayout.LayoutParams(dpi(46f), dpi(46f)).apply { topMargin = dpi(6f) })
        zoomColumn.addView(locate, LinearLayout.LayoutParams(dpi(46f), dpi(46f)).apply { topMargin = dpi(16f) })
        mapLayout.addView(zoomColumn, FrameLayout.LayoutParams(wrap, wrap, Gravity.END or Gravity.CENTER_VERTICAL).apply {
            topMargin = dpi(40f)
            marginEnd = dpi(24f)
        })
        val waypoint = label("TANDAI / HAPUS TUJUAN", 13f, MenuStyle.ON_ACCENT, MenuStyle.TAB_FONT).apply {
            gravity = Gravity.CENTER
            setPadding(dpi(18f), 0, dpi(18f), 0)
            background = ModernUi.pressable(
                MenuStyle.cut(ctx, MenuStyle.ACCENT, 8f), MenuStyle.cut(ctx, MenuStyle.ACCENT_PRESSED, 8f))
            isClickable = true
        }
        mapLayout.addView(waypoint, FrameLayout.LayoutParams(wrap, dpi(40f), Gravity.BOTTOM or Gravity.END).apply {
            marginEnd = dpi(24f)
            bottomMargin = dpi(28f)
        })
        val hint = label("Geser untuk menggeser  •  Cubit untuk zoom  •  Tanda di titik tengah", 11f,
            Color.argb(0xE6, 255, 255, 255), MenuStyle.LABEL_FONT).apply {
            background = MenuStyle.cut(ctx, MenuStyle.PANEL_DARK, 6f)
            setPadding(dpi(12f), dpi(6f), dpi(12f), dpi(6f))
        }
        mapLayout.addView(hint, FrameLayout.LayoutParams(wrap, wrap, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply {
            bottomMargin = dpi(34f)
        })
        rootLayout.addView(mapLayout, FrameLayout.LayoutParams(match, match))

        // ---- frame: top bar, then rail + content
        val frameLayout = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dpi(20f), dpi(8f), dpi(20f), dpi(12f))
        }
        frame = frameLayout

        // top bar
        val bar = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        topBar = bar
        bar.addView(View(ctx).apply { background = MenuStyle.cut(ctx, MenuStyle.ACCENT, 3f) },
            LinearLayout.LayoutParams(dpi(6f), dpi(30f)))
        serverName = label("", 28f, Color.WHITE, MenuStyle.TITLE_FONT).apply {
            ellipsize = android.text.TextUtils.TruncateAt.END
            letterSpacing = 0.04f
            setShadowLayer(4f, 0f, 0f, Color.BLACK)
        }
        bar.addView(serverName, LinearLayout.LayoutParams(0, wrap, 1f).apply { marginStart = dpi(10f) })

        val playerBox = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.END
        }
        playerName = label("", 14f, Color.WHITE, MenuStyle.STRONG_FONT)
        playerBox.addView(playerName, LinearLayout.LayoutParams(wrap, wrap))
        val playerLine = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        val clock = android.widget.TextClock(ctx).apply {
            format12Hour = "EEEE HH:mm"
            format24Hour = "EEEE HH:mm"
            isAllCaps = true
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
            setTextColor(MenuStyle.TEXT_DIM)
            ModernUi.font(ctx, MenuStyle.LABEL_FONT)?.let { typeface = it }
        }
        playerLine.addView(clock, LinearLayout.LayoutParams(wrap, wrap))
        playerStats = label("", 11f, MenuStyle.TEXT_DIM, MenuStyle.LABEL_FONT)
        playerLine.addView(playerStats, LinearLayout.LayoutParams(wrap, wrap).apply { marginStart = dpi(10f) })
        playerMoney = label("", 11f, MenuStyle.MONEY, MenuStyle.STRONG_FONT)
        playerLine.addView(playerMoney, LinearLayout.LayoutParams(wrap, wrap).apply { marginStart = dpi(10f) })
        playerBox.addView(playerLine, LinearLayout.LayoutParams(wrap, wrap))
        bar.addView(playerBox, LinearLayout.LayoutParams(wrap, wrap).apply { marginEnd = dpi(12f) })

        val close = label("✕", 18f, Color.WHITE, null).apply {
            gravity = Gravity.CENTER
            background = ModernUi.pressable(
                MenuStyle.cut(ctx, MenuStyle.PANEL_DARK, 6f, MenuStyle.LINE, 1f),
                MenuStyle.cut(ctx, MenuStyle.ACCENT, 6f))
            isClickable = true
        }
        bar.addView(close, LinearLayout.LayoutParams(dpi(40f), dpi(40f)))
        frameLayout.addView(bar, LinearLayout.LayoutParams(match, dpi(46f)))

        // thin line under the top bar with a yellow segment
        val underline = FrameLayout(ctx)
        underline.addView(View(ctx).apply { setBackgroundColor(MenuStyle.LINE) }, FrameLayout.LayoutParams(match, dpi(1f), Gravity.CENTER_VERTICAL))
        underline.addView(View(ctx).apply { setBackgroundColor(MenuStyle.ACCENT) }, FrameLayout.LayoutParams(dpi(120f), dpi(2f), Gravity.CENTER_VERTICAL))
        frameLayout.addView(underline, LinearLayout.LayoutParams(match, dpi(3f)).apply { topMargin = dpi(4f) })

        // body
        val body = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }

        // rail: tabs, continue
        val rail = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
        railTabs = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
        val railScroll = ScrollView(ctx).apply {
            isVerticalScrollBarEnabled = false
            overScrollMode = View.OVER_SCROLL_NEVER
        }
        railScroll.addView(railTabs, FrameLayout.LayoutParams(match, wrap))
        rail.addView(railScroll, LinearLayout.LayoutParams(match, 0, 1f))

        val resume = label("LANJUTKAN", 17f, MenuStyle.ON_ACCENT, MenuStyle.TITLE_FONT).apply {
            gravity = Gravity.CENTER
            letterSpacing = 0.08f
            background = ModernUi.pressable(
                MenuStyle.cut(ctx, MenuStyle.ACCENT, 10f), MenuStyle.cut(ctx, MenuStyle.ACCENT_PRESSED, 10f))
            isClickable = true
        }
        rail.addView(resume, LinearLayout.LayoutParams(match, dpi(42f)).apply { topMargin = dpi(8f) })
        body.addView(rail, LinearLayout.LayoutParams(dpi(RAIL_WIDTH_DP), match))

        // content panel
        val contentLayout = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            background = MenuStyle.cut(ctx, MenuStyle.PANEL, 12f, MenuStyle.LINE, 1f)
            setPadding(dpi(14f), dpi(8f), dpi(14f), dpi(10f))
            // The panel must not pass touches to the game.
            isClickable = true
        }
        content = contentLayout
        val titleLine = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.BOTTOM
        }
        pageTitle = label("", 24f, Color.WHITE, MenuStyle.TITLE_FONT).apply { letterSpacing = 0.05f }
        titleLine.addView(pageTitle, LinearLayout.LayoutParams(wrap, wrap))
        pageSubtitle = label("", 11f, MenuStyle.TEXT_DIM, MenuStyle.LABEL_FONT).apply {
            setPadding(dpi(10f), 0, 0, dpi(5f))
        }
        titleLine.addView(pageSubtitle, LinearLayout.LayoutParams(0, wrap, 1f))
        contentLayout.addView(titleLine, LinearLayout.LayoutParams(match, wrap))

        rowsScroll = ScrollView(ctx).apply {
            isVerticalScrollBarEnabled = false
            overScrollMode = View.OVER_SCROLL_NEVER
            isVerticalFadingEdgeEnabled = true
            setFadingEdgeLength(dpi(14f))
        }
        rowsLayout = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
        rowsScroll.addView(rowsLayout, FrameLayout.LayoutParams(match, wrap))
        contentLayout.addView(rowsScroll, LinearLayout.LayoutParams(match, 0, 1f).apply { topMargin = dpi(4f) })

        // hint bar: what the selected row does
        val hintBar = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = MenuStyle.cut(ctx, MenuStyle.PANEL_DARK, 6f)
            setPadding(dpi(10f), dpi(6f), dpi(10f), dpi(6f))
        }
        val badge = label("i", 12f, MenuStyle.ON_ACCENT, MenuStyle.STRONG_FONT).apply {
            gravity = Gravity.CENTER
            background = android.graphics.drawable.GradientDrawable().apply {
                shape = android.graphics.drawable.GradientDrawable.OVAL
                setColor(MenuStyle.ACCENT)
            }
        }
        hintBar.addView(badge, LinearLayout.LayoutParams(dpi(18f), dpi(18f)))
        val hintText = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
        hintTitle = label("", 12f, MenuStyle.ACCENT, MenuStyle.STRONG_FONT)
        hintText.addView(hintTitle, LinearLayout.LayoutParams(match, wrap))
        hintBody = label("", 11f, Color.argb(0xD9, 255, 255, 255), MenuStyle.LABEL_FONT).apply {
            maxLines = 2
            ellipsize = android.text.TextUtils.TruncateAt.END
        }
        hintText.addView(hintBody, LinearLayout.LayoutParams(match, wrap))
        hintBar.addView(hintText, LinearLayout.LayoutParams(0, wrap, 1f).apply { marginStart = dpi(10f) })
        contentLayout.addView(hintBar, LinearLayout.LayoutParams(match, wrap).apply { topMargin = dpi(8f) })

        body.addView(contentLayout, LinearLayout.LayoutParams(0, match, 1f).apply { marginStart = dpi(12f) })
        frameLayout.addView(body, LinearLayout.LayoutParams(match, 0, 1f).apply { topMargin = dpi(10f) })
        rootLayout.addView(frameLayout, FrameLayout.LayoutParams(match, match))

        close.setOnClickListener { closeMenu() }
        resume.setOnClickListener { closeMenu() }
        locate.setOnClickListener { ModernMenu.request(ModernMenu.REQUEST_CENTER_PLAYER) }
        waypoint.setOnClickListener { ModernMenu.request(ModernMenu.REQUEST_TOGGLE_WAYPOINT) }
        bindRepeat(zoomIn, ModernMenu.REQUEST_ZOOM_IN)
        bindRepeat(zoomOut, ModernMenu.REQUEST_ZOOM_OUT)

        buildTabs()
    }

    private fun dp(value: Float): Float = value * density
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
            root.alpha = 1f
            if (currentTab == TabId.MAP) selectTab(TabId.GAME, notifyNative = false)
            return
        }

        (root.parent as? ViewGroup)?.removeView(root)
        parent.addView(root)
        root.bringToFront()
        root.visibility = View.VISIBLE

        settings = ModernMenu.readSettings()
        eagleOk = null
        selectTab(TabId.GAME, notifyNative = false, rebuild = true)

        frame.animate().cancel()
        root.alpha = 0f
        frame.translationX = -dp(16f)
        val interpolator = DecelerateInterpolator(2f)
        root.animate().alpha(1f).setDuration(160).setInterpolator(interpolator).start()
        frame.animate().translationX(0f).setDuration(220).setInterpolator(interpolator).start()
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
        serverName.text = (if (server.isNotBlank()) server else "San Andreas Multiplayer").uppercase(Locale.ROOT)
        playerName.text = info.name
        playerMoney.text = formatMoney(info.money)
        playerStats.text = String.format(Locale.US, "ID %d  •  Skor %d  •  Ping %d ms", info.id, info.score, info.ping)
    }

    // ------------------------------------------------------------------ tabs

    private fun buildTabs() {
        railTabs.removeAllViews()
        tabs.clear()
        for (id in TabId.values()) {
            val tab = LinearLayout(activity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                isClickable = true
            }
            val bar = View(activity).apply { setBackgroundColor(MenuStyle.ACCENT) }
            tab.addView(bar, LinearLayout.LayoutParams(dpi(4f), ViewGroup.LayoutParams.MATCH_PARENT))
            val glyph = label(id.glyph, 15f, Color.WHITE, null).apply { gravity = Gravity.CENTER }
            tab.addView(glyph, LinearLayout.LayoutParams(dpi(34f), ViewGroup.LayoutParams.MATCH_PARENT))
            val text = label(id.title, 16f, Color.WHITE, MenuStyle.TAB_FONT).apply { letterSpacing = 0.06f }
            tab.addView(text, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            tab.setOnClickListener {
                try {
                    if (!hiding) selectTab(id, notifyNative = true)
                } catch (t: Throwable) {
                    t.printStackTrace()
                }
            }
            railTabs.addView(tab, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dpi(TAB_HEIGHT_DP)).apply {
                bottomMargin = dpi(2f)
            })
            tabs.add(TabViews(id, tab, glyph, text, bar))
        }
    }

    private fun selectTab(id: TabId, notifyNative: Boolean, rebuild: Boolean = false) {
        val previous = currentTab
        currentTab = id

        for (tab in tabs) {
            val selected = tab.id == id
            tab.view.background = if (selected) MenuStyle.fadeRight(MenuStyle.ACCENT_GLOW) else null
            tab.bar.visibility = if (selected) View.VISIBLE else View.INVISIBLE
            tab.text.setTextColor(if (selected) MenuStyle.ACCENT else Color.argb(0xD9, 255, 255, 255))
            tab.glyph.setTextColor(if (selected) MenuStyle.ACCENT else MenuStyle.TEXT_DIM)
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
        content.visibility = if (map) View.INVISIBLE else View.VISIBLE
        topBar.background = if (map) ModernUi.gradient(Color.argb(0xD9, 0, 0, 0), Color.TRANSPARENT) else null

        pageTitle.text = id.title
        pageSubtitle.text = id.subtitle

        if (!map && (rebuild || previous != id || rows.isEmpty())) {
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
            SectionRow("GTA SAN ANDREAS"),
            option(GtaSettings.VISUALS, "Kualitas visual",
                "Kualitas efek grafis secara umum. Lebih tinggi lebih bagus, tetapi lebih berat.",
                listOf("Rendah", "Sedang", "Tinggi", "Sgt tinggi")),
            slider(GtaSettings.RESOLUTION, "Resolusi",
                "Resolusi render. Berlaku penuh setelah game dibuka ulang."),
            slider(GtaSettings.DRAW_DISTANCE, "Jarak pandang",
                "Seberapa jauh bangunan dan object digambar."),
            option(GtaSettings.SHADOWS, "Bayangan GTA",
                "Bayangan bawaan GTA. Saat bayangan matahari EAGLE aktif, bayangan bawaan disembunyikan.",
                listOf("Mati", "Klasik", "Real-time")),
            option(GtaSettings.CAR_REFLECTIONS, "Refleksi kendaraan",
                "Pantulan lingkungan pada bodi kendaraan.",
                listOf("Mati", "Rendah", "Sedang", "Tinggi")),
            slider(GtaSettings.BRIGHTNESS, "Kecerahan", "Kecerahan layar game."),
            option(GtaSettings.FRAME_LIMITER, "Pembatas frame",
                "Batasi frame rate supaya lebih stabil dan hemat baterai.", ON_OFF),
            option(GtaSettings.SUBTITLES, "Subtitle", "Tampilkan subtitle dialog.", ON_OFF)
        ) + eagleRows()
        TabId.EFFECTS -> effectRows()
        TabId.AUDIO -> settingRows(
            slider(GtaSettings.SFX_VOLUME, "Volume efek suara",
                "Volume suara tembakan, mesin, langkah dan lainnya."),
            slider(GtaSettings.MUSIC_VOLUME, "Volume radio", "Volume radio kendaraan dan musik."),
            option(GtaSettings.AUTOTUNE, "Radio auto-tune",
                "Radio kendaraan langsung memutar stasiun terakhir.", ON_OFF)
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
        ActionRow("Pengaturan grafis", "Kualitas GTA, bayangan matahari EAGLE dan efek shader.") {
            selectTab(TabId.GRAPHICS, notifyNative = false)
        },
        ActionRow("Pengaturan klien", "FPS, chat dan tampilan klien SA-MP.") { ModernMenu.openClientSettings() },
        ActionRow("Keluar dari game", "Keluar dari game. Ketuk dua kali untuk konfirmasi.", danger = true) {
            onExitClicked(it)
        }
    )

    // ---- GTA MobileSettings

    private fun option(setting: Int, label: String, desc: String, options: List<String>): Row? {
        if (!isAvailable(setting)) return null
        val count = settingMax(setting) - settingMin(setting) + 1
        val names = List(count) { i -> options.getOrNull(i) ?: (settingMin(setting) + i).toString() }
        return ChoiceRow(label, desc, names,
            get = { settingValue(setting) - settingMin(setting) },
            set = { i -> setSettingValue(setting, settingMin(setting) + i) })
    }

    private fun slider(setting: Int, label: String, desc: String, percent: Boolean = true): Row? {
        if (!isAvailable(setting)) return null
        val min = settingMin(setting).toFloat()
        val max = settingMax(setting).toFloat()
        return RangeRow(label, desc, min, max, 1f,
            format = { v ->
                if (percent) "${((v - min) * 100f / (max - min).coerceAtLeast(1f)).roundToInt()}%"
                else v.roundToInt().toString()
            },
            get = { settingValue(setting).toFloat() },
            set = { v, _ -> setSettingValue(setting, v.roundToInt()) })
    }

    private fun settingRows(vararg rows: Row?): List<Row> {
        val list = rows.filterNotNull()
        if (list.none { it.selectable }) {
            return list.filter { it !is SectionRow } + ActionRow("Pengaturan tidak tersedia",
                "Client belum mendukung pengaturan dari menu ini.") {}
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

    private fun setSettingValue(setting: Int, value: Int) {
        val s = settings ?: return
        val v = value.coerceIn(settingMin(setting), settingMax(setting))
        if (s[setting * 4] == v) return
        s[setting * 4] = v
        ModernMenu.setSetting(setting, v)
    }

    // ---- EAGLE graphics engine (GraphicsNative, libmultiplayer.so)

    /** False when libmultiplayer.so has no EAGLE engine (old build): rows say so instead of crashing. */
    private fun eagleAvailable(): Boolean {
        eagleOk?.let { return it }
        val ok = try {
            GraphicsNative.nativeGetStatus()
            true
        } catch (t: Throwable) {
            false
        }
        eagleOk = ok
        return ok
    }

    private inline fun <T> eagle(fallback: T, block: () -> T): T =
        try {
            block()
        } catch (t: Throwable) {
            eagleOk = false
            fallback
        }

    private fun saveEagle() = GraphicsNative.saveUserSettings()

    private fun eagleMissingRows(): List<Row> = listOf(
        ActionRow("EAGLE tidak tersedia",
            "libmultiplayer.so ini belum berisi EAGLE graphics engine. Pasang build terbaru.") {}
    )

    private fun eagleRows(): List<Row> {
        val rows = ArrayList<Row>()
        rows.add(SectionRow("BAYANGAN MATAHARI  •  EAGLE"))
        if (!eagleAvailable()) return rows + eagleMissingRows()
        rows.add(ChoiceRow("Grafis EAGLE",
            "Engine grafis EAGLE: bayangan matahari real-time dari gedung, pohon, kendaraan dan karakter.",
            ON_OFF,
            get = { eagle(0) { if (GraphicsNative.nativeIsGraphicsEnabled()) 1 else 0 } },
            set = { i ->
                eagle(Unit) { GraphicsNative.nativeSetGraphicsEnabled(i == 1) }
                saveEagle()
            }))
        rows.add(ChoiceRow("Bayangan matahari",
            "Bayangan mengikuti arah matahari GTA (pagi, siang, sore). Mati otomatis di malam hari dan dalam ruangan.",
            ON_OFF,
            get = { eagle(0) { if (GraphicsNative.nativeIsShadowEnabled()) 1 else 0 } },
            set = { i ->
                eagle(Unit) { GraphicsNative.nativeSetShadowEnabled(i == 1) }
                saveEagle()
            }))
        rows.add(ChoiceRow("Kualitas bayangan",
            "Resolusi, jumlah cascade dan kehalusan tepi. Ultra paling tajam tetapi paling berat. Mengatur ulang jarak.",
            listOf("Rendah", "Sedang", "Tinggi", "Ultra"),
            get = { eagle(2) { GraphicsNative.clampQuality(GraphicsNative.nativeGetGraphicsQuality()) } },
            set = { i ->
                eagle(Unit) { GraphicsNative.nativeSetGraphicsQuality(i) }
                saveEagle()
                refreshAllRows() // the preset also changed the distance
            }))
        rows.add(RangeRow("Jarak bayangan",
            "Seberapa jauh dari kamera bayangan digambar. Makin jauh makin berat.",
            DISTANCE_MIN, DISTANCE_MAX, DISTANCE_STEP,
            format = { v -> "${v.roundToInt()} m" },
            get = { eagle(160f) { GraphicsNative.nativeGetShadowDistance() }.coerceIn(DISTANCE_MIN, DISTANCE_MAX) },
            set = { v, committed ->
                if (committed) {
                    eagle(Unit) { GraphicsNative.nativeSetShadowDistance(v) }
                    saveEagle()
                }
            }))
        rows.add(ActionRow("Efek shader lanjutan",
            "Kekuatan dan kelembutan bayangan per gedung, kendaraan dan karakter (tab EFEK).") {
            selectTab(TabId.EFFECTS, notifyNative = false)
        })
        return rows
    }

    private fun effectRows(): List<Row> {
        if (!eagleAvailable()) return eagleMissingRows()
        val rows = ArrayList<Row>()
        uniformValues.clear()
        val list = eagle("") { GraphicsNative.nativeGetShaderUniforms() ?: "" }
        var lastClass: String? = null
        for (line in list.split('\n')) {
            val f = line.split('\t')
            if (f.size < 8) continue
            val cls = f[0]
            val name = f[1]
            val type = f[2]
            val value = f[3].toFloatOrNull() ?: continue
            val min = f[4].toFloatOrNull() ?: continue
            val max = f[5].toFloatOrNull() ?: continue
            val step = f[6].toFloatOrNull() ?: 0.01f
            if (f[7] != "1") continue // not used by any glShader file: no effect in game
            val key = uniformKey(cls, name)
            uniformValues[key] = value
            if (cls != lastClass) {
                rows.add(SectionRow(uniformSection(cls)))
                lastClass = cls
            }
            val label = uniformLabel(cls, name)
            val desc = "${uniformDesc(cls, name)}  [$cls / $name]"
            if (type == "bool") {
                rows.add(ChoiceRow(label, desc, ON_OFF,
                    get = { if ((uniformValues[key] ?: 0f) > 0.5f) 1 else 0 },
                    set = { i -> setUniform(cls, name, i.toFloat(), committed = true) }))
            } else {
                val integer = type == "int"
                rows.add(RangeRow(label, desc, min, max, if (integer) maxOf(1f, step) else step,
                    format = { v -> if (integer) v.roundToInt().toString() else String.format(Locale.US, "%.2f", v) },
                    get = { uniformValues[key] ?: value },
                    set = { v, committed -> setUniform(cls, name, v, committed) }))
            }
        }
        if (rows.isEmpty()) {
            rows.add(ActionRow("shaderUniform.ini kosong",
                "Salin TESTLIT/graphics ke /storage/emulated/0/TESTLIT/graphics lalu buka menu lagi.") {})
        }

        rows.add(SectionRow("DEBUG"))
        rows.add(debugRow("Warna cascade", "showCascade",
            "Warnai area bayangan per cascade: merah dekat, hijau, biru jauh. Untuk mengecek jarak & kualitas."))
        rows.add(debugRow("Tampilkan shadow map", "showShadowMap",
            "Gambar shadow map di kiri bawah layar (siluet gedung, pohon, mobil, karakter)."))

        rows.add(SectionRow("BERKAS"))
        rows.add(ActionRow("Muat ulang Config.ini",
            "Baca ulang Config.ini, Advanced.ini, shaderUniform.ini dan eagle_timecyc.dat dari HP. " +
                "Pilihan di menu ini kembali ke isi file.") { row ->
            eagle(Unit) { GraphicsNative.nativeReloadConfig() }
            GraphicsNative.clearUserSettings()
            row.descOverride = "Dimuat ulang. Nilai diambil dari file."
            refreshRow(row)
            // The files are read by the game thread on its next frame.
            rowsLayout.postDelayed({ if (currentTab == TabId.EFFECTS && root.parent != null) buildRows() }, RELOAD_DELAY_MS)
        })
        rows.add(ActionRow("Reset efek ke bawaan",
            "Semua nilai efek kembali ke nilai bawaan shader dan disimpan ke shaderUniform.ini.") { _ ->
            eagle(Unit) {
                GraphicsNative.nativeResetShaderUniforms()
                GraphicsNative.nativeSaveShaderUniforms()
            }
            buildRows()
        })
        rows.add(ActionRow("Status engine", "") {}.apply {
            descProvider = { eagle("tidak tersedia") { GraphicsNative.nativeGetStatus() ?: "" } }
        })
        return rows
    }

    private fun debugRow(label: String, flag: String, desc: String): Row = ChoiceRow(label, desc, ON_OFF,
        get = { eagle(0) { if (GraphicsNative.nativeGetDebugFlag(flag)) 1 else 0 } },
        set = { i -> eagle(Unit) { GraphicsNative.nativeSetDebugFlag(flag, i == 1) } })

    private fun uniformKey(cls: String, name: String) = cls + "\u0000" + name

    /** Live value while dragging; written to shaderUniform.ini when the finger is lifted. */
    private fun setUniform(cls: String, name: String, value: Float, committed: Boolean) {
        uniformValues[uniformKey(cls, name)] = value
        eagle(Unit) {
            GraphicsNative.nativeSetShaderUniform(cls, name, value)
            if (committed) GraphicsNative.nativeSaveShaderUniforms()
        }
    }

    private fun uniformSection(cls: String): String = when (cls.lowercase(Locale.ROOT)) {
        "shadow" -> "BAYANGAN  •  UMUM"
        "building" -> "GEDUNG & DUNIA"
        "vehicle" -> "KENDARAAN"
        "character" -> "KARAKTER"
        else -> cls.uppercase(Locale.ROOT)
    }

    private fun uniformLabel(cls: String, name: String): String =
        UNIFORM_TEXT["${cls.lowercase(Locale.ROOT)}.${name.lowercase(Locale.ROOT)}"]?.first ?: "$cls  •  $name"

    private fun uniformDesc(cls: String, name: String): String =
        UNIFORM_TEXT["${cls.lowercase(Locale.ROOT)}.${name.lowercase(Locale.ROOT)}"]?.second
            ?: "Nilai shader dari shaderUniform.ini."

    // ---- row views

    private fun buildRows() {
        rowsLayout.removeAllViews()
        rows.clear()
        for (row in rowsFor(currentTab)) {
            val views = createRow(row)
            val height = if (row is SectionRow) ViewGroup.LayoutParams.WRAP_CONTENT else dpi(ROW_HEIGHT_DP)
            val lp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, height)
            lp.bottomMargin = dpi(3f)
            rowsLayout.addView(views.view, lp)
            rows.add(views)
        }
        selectedRow = rows.indexOfFirst { it.row.selectable }
        rows.forEachIndexed { i, views -> bindRow(views, i == selectedRow) }
        updateHint()
    }

    private fun createRow(row: Row): RowViews {
        val layout = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }

        if (row is SectionRow) {
            layout.setPadding(0, dpi(10f), 0, dpi(2f))
            layout.addView(View(activity).apply { setBackgroundColor(MenuStyle.ACCENT) },
                LinearLayout.LayoutParams(dpi(3f), dpi(14f)))
            val title = label(row.label, 15f, Color.WHITE, MenuStyle.TITLE_FONT).apply { letterSpacing = 0.08f }
            layout.addView(title, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                marginStart = dpi(8f)
            })
            layout.addView(View(activity).apply { setBackgroundColor(MenuStyle.LINE) },
                LinearLayout.LayoutParams(0, dpi(1f), 1f).apply { marginStart = dpi(10f) })
            return RowViews(row, layout, null, title, emptyList(), null, null, null, null)
        }

        layout.isClickable = true
        val bar = View(activity).apply { setBackgroundColor(MenuStyle.ACCENT) }
        layout.addView(bar, LinearLayout.LayoutParams(dpi(3f), ViewGroup.LayoutParams.MATCH_PARENT))
        val label = label(row.label, 14f, Color.WHITE, MenuStyle.LABEL_FONT).apply {
            ellipsize = android.text.TextUtils.TruncateAt.END
            setPadding(dpi(10f), 0, dpi(6f), 0)
        }
        layout.addView(label, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))

        val chips = ArrayList<TextView>()
        var value: TextView? = null
        var left: TextView? = null
        var right: TextView? = null
        var slider: MenuStyle.SliderView? = null

        when (row) {
            is ChoiceRow -> {
                if (row.options.size <= MAX_CHIPS) {
                    row.options.forEachIndexed { i, name ->
                        val chip = label(name, 12f, Color.WHITE, MenuStyle.STRONG_FONT).apply {
                            gravity = Gravity.CENTER
                            setPadding(dpi(8f), 0, dpi(8f), 0)
                            minWidth = dpi(CHIP_MIN_WIDTH_DP)
                            isClickable = true
                        }
                        layout.addView(chip, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dpi(30f)).apply {
                            marginStart = dpi(5f)
                        })
                        chips.add(chip)
                        chip.tag = i
                    }
                } else {
                    // Long lists: ‹ value › stepper.
                    left = arrow("‹")
                    value = label("", 13f, Color.WHITE, MenuStyle.STRONG_FONT).apply {
                        gravity = Gravity.CENTER
                        minWidth = dpi(96f)
                        isClickable = true
                    }
                    right = arrow("›")
                    layout.addView(left, LinearLayout.LayoutParams(dpi(30f), ViewGroup.LayoutParams.MATCH_PARENT))
                    layout.addView(value, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.MATCH_PARENT))
                    layout.addView(right, LinearLayout.LayoutParams(dpi(30f), ViewGroup.LayoutParams.MATCH_PARENT))
                }
                layout.setPadding(0, 0, dpi(6f), 0)
            }
            is RangeRow -> {
                val sliderView = MenuStyle.SliderView(activity)
                sliderView.min = row.min
                sliderView.max = row.max
                sliderView.step = row.step
                sliderView.value = row.get()
                slider = sliderView
                layout.addView(sliderView, LinearLayout.LayoutParams(dpi(SLIDER_WIDTH_DP), ViewGroup.LayoutParams.MATCH_PARENT))
                value = label("", 12f, MenuStyle.ACCENT, MenuStyle.VALUE_FONT).apply {
                    gravity = Gravity.CENTER
                    background = MenuStyle.cut(activity, MenuStyle.PANEL_DARK, 4f, MenuStyle.CHIP_STROKE, 1f)
                }
                layout.addView(value, LinearLayout.LayoutParams(dpi(58f), dpi(28f)).apply {
                    marginStart = dpi(8f)
                    marginEnd = dpi(8f)
                })
            }
            is ActionRow -> {
                right = arrow("›")
                right.isClickable = false
                layout.addView(right, LinearLayout.LayoutParams(dpi(34f), ViewGroup.LayoutParams.MATCH_PARENT))
            }
        }

        val views = RowViews(row, layout, bar, label, chips, value, left, right, slider)

        layout.setOnClickListener { onRowTapped(views) { handleRowTap(views) } }
        if (row is ChoiceRow) {
            for (chip in chips) chip.setOnClickListener { onRowTapped(views) { choose(views, chip.tag as Int) } }
            left?.setOnClickListener { onRowTapped(views) { step(views, -1) } }
            right?.setOnClickListener { onRowTapped(views) { step(views, 1) } }
            value?.setOnClickListener { onRowTapped(views) { step(views, 1) } }
        }
        if (row is RangeRow) {
            slider?.onValueChanged = { v ->
                onRowTapped(views) {
                    select(views)
                    row.set(v, false)
                    views.value?.text = row.format(v)
                }
            }
            slider?.onValueCommitted = { v ->
                onRowTapped(views) {
                    row.set(v, true)
                    bindRow(views, rows.indexOf(views) == selectedRow)
                }
            }
        }
        return views
    }

    /** A UI error must never close the game. */
    private inline fun onRowTapped(views: RowViews, block: () -> Unit) {
        if (hiding || rows.indexOf(views) < 0) return
        try {
            block()
        } catch (t: Throwable) {
            t.printStackTrace()
        }
    }

    /** Tap on the row itself: actions run, chips and stepper move to the next value. */
    private fun handleRowTap(views: RowViews) {
        val alreadySelected = rows.indexOf(views) == selectedRow
        select(views)
        when (val row = views.row) {
            is ActionRow -> row.action(row)
            is ChoiceRow -> if (alreadySelected) step(views, 1)
            else -> Unit // sliders handle their own touches
        }
    }

    private fun choose(views: RowViews, index: Int) {
        val row = views.row as? ChoiceRow ?: return
        select(views)
        if (index != row.get()) row.set(index.coerceIn(0, row.options.size - 1))
        bindRow(views, true)
    }

    private fun step(views: RowViews, direction: Int) {
        val row = views.row as? ChoiceRow ?: return
        val count = row.options.size
        if (count <= 0) return
        choose(views, ((row.get() + direction) % count + count) % count)
    }

    private fun select(views: RowViews) {
        val index = rows.indexOf(views)
        if (index < 0 || index == selectedRow || !views.row.selectable) return
        rows.getOrNull(selectedRow)?.let { bindRow(it, false) }
        selectedRow = index
        bindRow(views, true)
        updateHint()
    }

    private fun bindRow(views: RowViews, selected: Boolean) {
        val row = views.row
        if (row is SectionRow) return

        val base = if (selected) MenuStyle.ROW_SELECTED else MenuStyle.ROW
        views.view.background = ModernUi.pressable(
            MenuStyle.cut(activity, base, 6f), MenuStyle.cut(activity, MenuStyle.ROW_PRESSED, 6f))
        views.bar?.visibility = if (selected) View.VISIBLE else View.INVISIBLE
        val danger = row is ActionRow && row.danger
        views.label.setTextColor(
            when {
                danger -> MenuStyle.DANGER
                selected -> Color.WHITE
                else -> Color.argb(0xE6, 255, 255, 255)
            })

        when (row) {
            is ChoiceRow -> {
                val index = row.get()
                views.chips.forEachIndexed { i, chip ->
                    val on = i == index
                    chip.background = if (on) MenuStyle.cut(activity, MenuStyle.ACCENT, 5f)
                    else MenuStyle.cut(activity, MenuStyle.CHIP, 5f, MenuStyle.CHIP_STROKE, 1f)
                    chip.setTextColor(if (on) MenuStyle.ON_ACCENT else Color.argb(0xD9, 255, 255, 255))
                }
                views.value?.text = row.options.getOrNull(index) ?: ""
                views.value?.setTextColor(MenuStyle.ACCENT)
                views.arrowLeft?.setTextColor(if (selected) MenuStyle.ACCENT else Color.WHITE)
                views.arrowRight?.setTextColor(if (selected) MenuStyle.ACCENT else Color.WHITE)
            }
            is RangeRow -> {
                val s = views.slider
                if (s != null && !s.dragging) s.value = row.get()
                views.value?.text = row.format(s?.value ?: row.get())
            }
            is ActionRow -> {
                views.arrowRight?.setTextColor(if (selected) MenuStyle.ACCENT else MenuStyle.TEXT_FAINT)
            }
        }
    }

    private fun refreshAllRows() {
        rows.forEachIndexed { i, views -> bindRow(views, i == selectedRow) }
        updateHint()
    }

    private fun updateHint() {
        val views = rows.getOrNull(selectedRow)
        if (views == null) {
            hintTitle.text = currentTab.title
            hintBody.text = currentTab.subtitle
            return
        }
        val row = views.row
        hintTitle.text = row.label.uppercase(Locale.ROOT)
        hintBody.text = if (row is ActionRow) row.descOverride ?: row.descProvider?.invoke() ?: row.desc else row.desc
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
        updateHint()
    }

    // --------------------------------------------------------------- helpers

    private fun label(value: String, sizeSp: Float, color: Int, font: String?): TextView =
        TextView(activity).apply {
            text = value
            setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp)
            setTextColor(color)
            maxLines = 1
            includeFontPadding = false
            val tf = font?.let { ModernUi.font(activity, it) }
            typeface = tf ?: Typeface.DEFAULT_BOLD
        }

    private fun arrow(symbol: String) = label(symbol, 22f, Color.WHITE, null).apply {
        gravity = Gravity.CENTER
        isClickable = true
    }

    private fun squareButton(glyph: String) = label(glyph, 20f, Color.WHITE, null).apply {
        gravity = Gravity.CENTER
        background = ModernUi.pressable(
            MenuStyle.cut(activity, MenuStyle.PANEL_DARK, 6f, MenuStyle.LINE, 1f),
            MenuStyle.cut(activity, MenuStyle.ACCENT, 6f))
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

    private companion object {
        const val EXIT_CONFIRM_MS = 3000L
        const val REPEAT_DELAY_MS = 350L
        const val REPEAT_INTERVAL_MS = 110L
        const val RELOAD_DELAY_MS = 450L
        const val ROW_HEIGHT_DP = 42f
        const val TAB_HEIGHT_DP = 36f
        const val RAIL_WIDTH_DP = 168f
        const val SLIDER_WIDTH_DP = 190f
        const val CHIP_MIN_WIDTH_DP = 62f
        const val MAX_CHIPS = 5
        const val DISTANCE_MIN = 40f
        const val DISTANCE_MAX = 300f
        const val DISTANCE_STEP = 10f
        val ON_OFF = listOf("Mati", "Nyala")

        /** Labels for the shaderUniform.ini values of the EAGLE glShader files. */
        val UNIFORM_TEXT = mapOf(
            "shadow.strength" to ("Kekuatan bayangan" to "Kegelapan semua bayangan matahari. 0 = tanpa bayangan."),
            "shadow.softness" to ("Kelembutan tepi" to "Lebar tepi lembut bayangan (PCF). Lebih tinggi = lebih halus."),
            "building.shadowstrength" to ("Bayangan gedung & jalan" to "Kegelapan bayangan pada gedung, jalan, pohon dan objek."),
            "building.sunboost" to ("Sinar matahari gedung" to "Tambahan terang di bagian gedung yang terkena matahari."),
            "vehicle.shadowstrength" to ("Bayangan kendaraan" to "Kegelapan bayangan yang jatuh pada kendaraan."),
            "vehicle.sunboost" to ("Sinar matahari kendaraan" to "Tambahan terang di bodi kendaraan yang terkena matahari."),
            "vehicle.specularinshadow" to ("Kilap di bayangan" to "Sisa kilap cat kendaraan saat berada di bayangan."),
            "character.shadowstrength" to ("Bayangan karakter" to "Kegelapan bayangan pada ped dan pemain."),
            "character.sunboost" to ("Sinar matahari karakter" to "Tambahan terang pada karakter yang terkena matahari."),
            "character.minlight" to ("Cahaya minimum karakter" to "Wajah dan badan tetap terlihat walau di bawah bayangan gedung.")
        )
    }
}
