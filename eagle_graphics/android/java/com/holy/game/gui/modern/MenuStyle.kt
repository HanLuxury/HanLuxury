package com.holy.game.gui.modern

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.view.MotionEvent
import android.view.View
import kotlin.math.roundToInt

/**
 * Look of the pause menu: dark tactical panels, condensed titles, a yellow
 * accent, option chips and cut corners (the style of mobile battle royale
 * settings screens).
 *
 * Like ModernUi this adds no resource: colours, drawables and views are made
 * in code and fonts are looked up by name (ModernUi.font).
 */
internal object MenuStyle {

    val ACCENT = Color.rgb(0xF2, 0xC2, 0x30)
    val ACCENT_PRESSED = Color.rgb(0xD1, 0xA3, 0x18)
    val ACCENT_GLOW = Color.argb(0x59, 0xF2, 0xC2, 0x30)
    val ON_ACCENT = Color.rgb(0x16, 0x16, 0x16)

    val TEXT = Color.WHITE
    val TEXT_DIM = Color.rgb(0x9A, 0xA3, 0xAF)
    val TEXT_FAINT = Color.argb(0x80, 0xFF, 0xFF, 0xFF)

    val SCRIM_TOP = Color.argb(0xF2, 0x07, 0x09, 0x0D)
    val SCRIM_MIDDLE = Color.argb(0xD9, 0x10, 0x14, 0x1A)
    val SCRIM_BOTTOM = Color.argb(0xEB, 0x0A, 0x0C, 0x10)
    val PANEL = Color.argb(0xC7, 0x13, 0x17, 0x1E)
    val PANEL_DARK = Color.argb(0xE0, 0x0B, 0x0E, 0x12)
    val ROW = Color.argb(0x99, 0x20, 0x26, 0x2F)
    val ROW_SELECTED = Color.argb(0xE6, 0x2B, 0x32, 0x3D)
    val ROW_PRESSED = Color.argb(0xE6, 0x38, 0x40, 0x4C)
    val CHIP = Color.argb(0xF2, 0x27, 0x2D, 0x37)
    val CHIP_STROKE = Color.argb(0x2E, 0xFF, 0xFF, 0xFF)
    val LINE = Color.argb(0x33, 0xFF, 0xFF, 0xFF)
    val DANGER = Color.rgb(0xFF, 0x5C, 0x5C)
    val MONEY = Color.rgb(0x7E, 0xD9, 0x57)

    const val TITLE_FONT = "bebas_bold"
    const val TAB_FONT = "akrobat_bold"
    const val LABEL_FONT = "gilroy_semibold"
    const val STRONG_FONT = "gilroy_bold"
    const val VALUE_FONT = "din_pro_bold"

    /** Panel / button background with the top-left and bottom-right corners cut off. */
    fun cut(context: Context, fill: Int, cutDp: Float = 6f, stroke: Int = 0, strokeDp: Float = 0f): Drawable =
        CutCornerDrawable(fill, stroke, ModernUi.dp(context, strokeDp), ModernUi.dp(context, cutDp))

    /** Left-to-right fade, used behind the selected tab. */
    fun fadeRight(start: Int): GradientDrawable =
        GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, intArrayOf(start, Color.TRANSPARENT))

    class CutCornerDrawable(
        private val fill: Int,
        private val stroke: Int,
        private val strokeWidth: Float,
        private val cut: Float
    ) : Drawable() {

        private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.FILL
            color = fill
        }
        private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE
            color = stroke
            this.strokeWidth = this@CutCornerDrawable.strokeWidth
        }
        private val path = Path()
        private val strokePath = Path()

        override fun onBoundsChange(bounds: Rect) {
            super.onBoundsChange(bounds)
            build(path, RectF(bounds))
            val inset = strokeWidth / 2f
            build(strokePath, RectF(bounds.left + inset, bounds.top + inset, bounds.right - inset, bounds.bottom - inset))
        }

        private fun build(p: Path, r: RectF) {
            val c = minOf(cut, r.width() / 2f, r.height() / 2f)
            p.reset()
            p.moveTo(r.left + c, r.top)
            p.lineTo(r.right, r.top)
            p.lineTo(r.right, r.bottom - c)
            p.lineTo(r.right - c, r.bottom)
            p.lineTo(r.left, r.bottom)
            p.lineTo(r.left, r.top + c)
            p.close()
        }

        override fun draw(canvas: Canvas) {
            if (Color.alpha(fill) != 0) canvas.drawPath(path, fillPaint)
            if (strokeWidth > 0f && Color.alpha(stroke) != 0) canvas.drawPath(strokePath, strokePaint)
        }

        override fun setAlpha(alpha: Int) {
            fillPaint.alpha = Color.alpha(fill) * alpha / 255
            strokePaint.alpha = Color.alpha(stroke) * alpha / 255
            invalidateSelf()
        }

        override fun setColorFilter(colorFilter: ColorFilter?) {
            fillPaint.colorFilter = colorFilter
            strokePaint.colorFilter = colorFilter
            invalidateSelf()
        }

        @Deprecated("Deprecated in Java")
        override fun getOpacity(): Int = PixelFormat.TRANSLUCENT
    }

    /**
     * Slider with a yellow fill and a round thumb. Values are floats snapped
     * to [step]; drag or tap anywhere on it.
     */
    class SliderView(context: Context) : View(context) {

        var min = 0f
        var max = 1f
        var step = 0.01f
        var value = 0f
            set(v) {
                field = snap(v)
                invalidate()
            }

        /** True while the finger is on the bar (the row must not overwrite the value). */
        var dragging = false
            private set

        var onValueChanged: ((Float) -> Unit)? = null
        var onValueCommitted: ((Float) -> Unit)? = null

        private val density = context.resources.displayMetrics.density
        private val trackHeight = 4f * density
        private val thumbRadius = 8f * density
        private val trackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(0x47, 0xFF, 0xFF, 0xFF) }
        private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = ACCENT }
        private val thumbPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE }
        private val ringPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE
            color = ACCENT
            strokeWidth = 2.5f * density
        }
        private val rect = RectF()

        private fun snap(v: Float): Float {
            if (max <= min) return min
            val c = v.coerceIn(min, max)
            if (step <= 0f) return c
            val n = ((c - min) / step).roundToInt()
            return (min + n * step).coerceIn(min, max)
        }

        private fun left() = paddingLeft + thumbRadius
        private fun right() = width - paddingRight - thumbRadius

        override fun onDraw(canvas: Canvas) {
            super.onDraw(canvas)
            val l = left()
            val r = right()
            if (r <= l) return
            val cy = height / 2f
            val fraction = if (max > min) (value - min) / (max - min) else 0f
            val x = l + (r - l) * fraction
            val radius = trackHeight / 2f
            rect.set(l, cy - radius, r, cy + radius)
            canvas.drawRoundRect(rect, radius, radius, trackPaint)
            rect.set(l, cy - radius, x, cy + radius)
            canvas.drawRoundRect(rect, radius, radius, fillPaint)
            val thumb = if (dragging) thumbRadius * 1.15f else thumbRadius
            canvas.drawCircle(x, cy, thumb, thumbPaint)
            canvas.drawCircle(x, cy, thumb - ringPaint.strokeWidth / 2f, ringPaint)
        }

        @SuppressLint("ClickableViewAccessibility")
        override fun onTouchEvent(event: MotionEvent): Boolean {
            if (!isEnabled) return false
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    parent?.requestDisallowInterceptTouchEvent(true)
                    dragging = true
                    update(event.x)
                    invalidate()
                }
                MotionEvent.ACTION_MOVE -> update(event.x)
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    parent?.requestDisallowInterceptTouchEvent(false)
                    dragging = false
                    invalidate()
                    onValueCommitted?.invoke(value)
                }
            }
            return true
        }

        private fun update(x: Float) {
            val l = left()
            val r = right()
            if (r <= l) return
            val fraction = ((x - l) / (r - l)).coerceIn(0f, 1f)
            val newValue = snap(min + fraction * (max - min))
            if (newValue != value) {
                value = newValue
                onValueChanged?.invoke(value)
            }
        }
    }
}
