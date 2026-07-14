package com.notap.looper.ui.views

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.Shader
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import com.notap.looper.ui.Design
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Horizontal value slider. A rounded track with a glowing accent fill and a
 * grabbable thumb. Absolute positioning: the thumb jumps to (and follows) the
 * finger's x — the natural gesture for a fader, unlike a rotary knob's relative
 * turn. Reports 0..1 via [onChange].
 */
class SliderView(context: Context) : View(context) {

    var value: Float = 0f
        private set
    var onChange: ((Float) -> Unit)? = null
    var accent: Int = Design.green
        set(v) { field = v; invalidate() }

    fun setValueSilently(v: Float) { value = v.coerceIn(0f, 1f); invalidate() }

    /** Free-tier boundary as a fraction 0..1 (<0 = off). Draws an amber tick +
     *  a dimmed "locked" zone beyond it, and arms a detent haptic at the line. */
    var freeMarker: Float = -1f
        set(v) { field = v; invalidate() }

    /** Smoothly ease to a value (e.g. snap-back to the free limit) without firing
     *  onChange or detent haptics — used by the host after the paywall is dismissed. */
    fun animateTo(target: Float) {
        snapAnim?.cancel()
        snapAnim = android.animation.ValueAnimator.ofFloat(value, target.coerceIn(0f, 1f)).apply {
            duration = 220
            interpolator = android.view.animation.DecelerateInterpolator()
            addUpdateListener { value = it.animatedValue as Float; invalidate() }
            start()
        }
    }

    private var snapAnim: android.animation.ValueAnimator? = null
    private var lastMarkerSide = 0   // -1 below / +1 above the marker (detent edge-detect)

    private val trackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeCap = Paint.Cap.ROUND; color = Design.strokeSoft
    }
    private val markerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { strokeCap = Paint.Cap.ROUND }
    private val lockPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeCap = Paint.Cap.ROUND
    }
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeCap = Paint.Cap.ROUND
    }
    private val glowPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeCap = Paint.Cap.ROUND
    }
    private val thumbPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val thumbRimPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }

    private var lastHapticStep = -1

    // Inset so the round thumb never clips at the ends.
    private fun inset() = Design.dpf(11, context)

    override fun onDraw(c: Canvas) {
        val left = inset()
        val right = width - inset()
        val cy = height / 2f
        val span = right - left

        trackPaint.strokeWidth = Design.dpf(4f, context)
        c.drawLine(left, cy, right, cy, trackPaint)

        val hasMarker = freeMarker in 0f..1f
        val markerX = left + span * freeMarker

        // "locked" zone beyond the free-tier line — amber-tinted so >30% reads
        // as premium territory before the user ever drags into it.
        if (hasMarker) {
            lockPaint.color = Design.alpha(Design.amber, 0.22f)
            lockPaint.strokeWidth = Design.dpf(4f, context)
            c.drawLine(markerX, cy, right, cy, lockPaint)
        }

        val thumbX = left + span * value

        if (value > 0.002f) {
            glowPaint.color = Design.alpha(accent, 0.35f)
            glowPaint.strokeWidth = Design.dpf(9f, context)
            c.drawLine(left, cy, thumbX, cy, glowPaint)
            fillPaint.color = accent
            fillPaint.strokeWidth = Design.dpf(4f, context)
            c.drawLine(left, cy, thumbX, cy, fillPaint)
        }

        // free-tier tick (drawn above the fill so it stays visible)
        if (hasMarker) {
            markerPaint.color = Design.amber
            markerPaint.strokeWidth = Design.dpf(2f, context)
            val half = Design.dpf(7f, context)
            c.drawLine(markerX, cy - half, markerX, cy + half, markerPaint)
        }

        // thumb — glass body with an accent rim
        val thumbR = Design.dpf(10f, context)
        thumbPaint.shader = RadialGradient(
            thumbX, cy - thumbR * 0.35f, thumbR * 1.6f,
            intArrayOf(Design.surfaceHi, Design.surface),
            floatArrayOf(0f, 1f), Shader.TileMode.CLAMP
        )
        c.drawCircle(thumbX, cy, thumbR, thumbPaint)
        thumbPaint.shader = null
        thumbRimPaint.strokeWidth = Design.dpf(2f, context)
        thumbRimPaint.color = if (value > 0.002f) accent else Design.textLo
        c.drawCircle(thumbX, cy, thumbR, thumbRimPaint)
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.action) {
            MotionEvent.ACTION_DOWN -> {
                snapAnim?.cancel()                 // user grabbed it mid snap-back
                lastMarkerSide = 0                 // re-arm detect for this gesture
                parent?.requestDisallowInterceptTouchEvent(true)
                updateFromX(e.x)
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                updateFromX(e.x)
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                parent?.requestDisallowInterceptTouchEvent(false)
                return true
            }
        }
        return super.onTouchEvent(e)
    }

    private fun updateFromX(x: Float) {
        val left = inset()
        val span = (width - inset()) - left
        if (span <= 0f) return
        value = ((x - left) / span).coerceIn(0f, 1f)

        val step = (value * 20).roundToInt()
        if (step != lastHapticStep) {
            lastHapticStep = step
            performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
        }
        // Detent at the free-tier line — a firmer haptic when crossing it, so the
        // boundary is felt as a physical notch, not just seen.
        if (freeMarker in 0f..1f) {
            val side = if (value > freeMarker) 1 else -1
            if (lastMarkerSide != 0 && side != lastMarkerSide) {
                performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
            }
            lastMarkerSide = side
        }
        onChange?.invoke(value)
        invalidate()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        // Fill the width given; fixed comfortable touch height.
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val h = min(Design.dp(40, context), MeasureSpec.getSize(heightMeasureSpec)
            .let { if (MeasureSpec.getMode(heightMeasureSpec) == MeasureSpec.UNSPECIFIED) Int.MAX_VALUE else it })
        setMeasuredDimension(w, if (h <= 0) Design.dp(40, context) else h)
    }
}
