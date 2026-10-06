package com.holy.game.gui.modern

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import kotlin.math.abs
import kotlin.math.roundToInt

/** Flat GTA V style value bar. Drag or tap anywhere on it to set the value. */
class GtaSliderView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    var min = 0
    var max = 100
    var value = 0
        set(v) {
            field = v.coerceIn(min, max)
            invalidate()
        }

    /** Selected row: dark bar on the white row background. */
    var inverted = false
        set(v) {
            field = v
            invalidate()
        }

    /** Called for every new value while dragging. */
    var onValueChanged: ((Int) -> Unit)? = null

    /** Called once when the finger is lifted. */
    var onValueCommitted: ((Int) -> Unit)? = null

    private val density = context.resources.displayMetrics.density
    private val barHeight = 8f * density
    private val trackPaint = Paint()
    private val fillPaint = Paint()

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        trackPaint.color = if (inverted) Color.argb(60, 0, 0, 0) else Color.argb(80, 255, 255, 255)
        fillPaint.color = if (inverted) Color.BLACK else Color.WHITE

        val top = (height - barHeight) / 2f
        val left = paddingLeft.toFloat()
        val right = (width - paddingRight).toFloat()
        canvas.drawRect(left, top, right, top + barHeight, trackPaint)

        val range = (max - min).coerceAtLeast(1)
        val fraction = (value - min).toFloat() / range
        canvas.drawRect(left, top, left + (right - left) * fraction, top + barHeight, fillPaint)
    }

    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
    private var downX = 0f
    private var downY = 0f
    private var dragging = false

    /**
     * The bar only takes the gesture after a horizontal move: a vertical swipe
     * that starts on it scrolls the list instead of changing the value.
     * A plain tap sets the value on finger-up.
     */
    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = event.x
                downY = event.y
                dragging = false
            }
            MotionEvent.ACTION_MOVE -> {
                if (!dragging) {
                    val dx = abs(event.x - downX)
                    val dy = abs(event.y - downY)
                    if (dy > touchSlop && dy > dx) return false
                    if (dx <= touchSlop) return true
                    dragging = true
                    parent?.requestDisallowInterceptTouchEvent(true)
                }
                updateFromTouch(event.x)
            }
            MotionEvent.ACTION_UP -> {
                if (!dragging) updateFromTouch(event.x)
                parent?.requestDisallowInterceptTouchEvent(false)
                dragging = false
                onValueCommitted?.invoke(value)
            }
            MotionEvent.ACTION_CANCEL -> {
                parent?.requestDisallowInterceptTouchEvent(false)
                if (dragging) onValueCommitted?.invoke(value)
                dragging = false
            }
        }
        return true
    }

    private fun updateFromTouch(x: Float) {
        val left = paddingLeft.toFloat()
        val right = (width - paddingRight).toFloat()
        if (right <= left) return
        val fraction = ((x - left) / (right - left)).coerceIn(0f, 1f)
        val newValue = min + (fraction * (max - min)).roundToInt()
        if (newValue != value) {
            value = newValue
            onValueChanged?.invoke(value)
        }
    }
}
