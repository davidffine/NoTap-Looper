package com.notap.looper.ui.views

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.animation.OvershootInterpolator
import com.notap.looper.ui.Design

/**
 * Modern pill segmented control with a spring-animated selection thumb.
 * Short tap selects; long-press invokes [onInfo] for the descriptive sheet.
 */
class SegmentedControlView(context: Context) : View(context) {

    private val labels = arrayOf("AUTO", "TAP", "SYNC")
    var selected = 0
        private set

    var accent = Design.cyan
        set(v) { field = v; invalidate() }

    var onSelect: ((Int) -> Unit)? = null
    var onInfo: ((Int) -> Unit)? = null

    private val trackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Design.surface }
    private val borderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = Design.dpf(1, context); color = Design.stroke
    }
    private val thumbPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val thumbBorder = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = Design.dpf(1.5f, context)
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        typeface = Typeface.create("sans-serif-medium", Typeface.BOLD)
        letterSpacing = 0.12f
    }

    private val track = RectF()
    private val thumb = RectF()
    private var thumbPos = 0f          // animated index position
    private var anim: ValueAnimator? = null
    private var downX = 0f
    private var longFired = false

    init { textPaint.textSize = Design.dpf(13, context) }

    fun setSelectedSilently(index: Int) {
        selected = index.coerceIn(0, labels.size - 1)
        thumbPos = selected.toFloat()
        invalidate()
    }

    override fun onMeasure(w: Int, h: Int) {
        setMeasuredDimension(
            MeasureSpec.getSize(w),
            MeasureSpec.makeMeasureSpec(Design.dp(52, context), MeasureSpec.EXACTLY)
        )
    }

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        track.set(0f, 0f, w.toFloat(), h.toFloat())
    }

    override fun onDraw(c: Canvas) {
        val r = height / 2f
        c.drawRoundRect(track, r, r, trackPaint)
        c.drawRoundRect(track, r, r, borderPaint)

        val segW = width.toFloat() / labels.size
        val pad = Design.dpf(4, context)
        val left = thumbPos * segW + pad
        thumb.set(left, pad, left + segW - pad * 2, height - pad)
        val tr = thumb.height() / 2f
        thumbPaint.color = Design.alpha(accent, 0.16f)
        c.drawRoundRect(thumb, tr, tr, thumbPaint)
        thumbBorder.color = Design.alpha(accent, 0.9f)
        c.drawRoundRect(thumb, tr, tr, thumbBorder)

        for (i in labels.indices) {
            val frac = 1f - (kotlin.math.abs(thumbPos - i)).coerceIn(0f, 1f)
            textPaint.color = Design.mix(Design.textLo, accent, frac).let {
                if (frac > 0.5f) Design.mix(Design.textHi, accent, (frac - 0.5f) * 2f) else it
            }
            val cx = i * segW + segW / 2f
            val cy = height / 2f - (textPaint.descent() + textPaint.ascent()) / 2f
            c.drawText(labels[i], cx, cy, textPaint)
        }
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.action) {
            MotionEvent.ACTION_DOWN -> {
                downX = e.x; longFired = false
                postDelayed(longRunnable, 420)
                return true
            }
            MotionEvent.ACTION_UP -> {
                removeCallbacks(longRunnable)
                if (!longFired) {
                    val idx = (e.x / (width.toFloat() / labels.size)).toInt().coerceIn(0, labels.size - 1)
                    select(idx, true)
                }
                return true
            }
            MotionEvent.ACTION_CANCEL -> { removeCallbacks(longRunnable); return true }
        }
        return super.onTouchEvent(e)
    }

    private val longRunnable = Runnable {
        longFired = true
        val idx = (downX / (width.toFloat() / labels.size)).toInt().coerceIn(0, labels.size - 1)
        performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
        onInfo?.invoke(idx)
    }

    fun select(index: Int, notify: Boolean) {
        val idx = index.coerceIn(0, labels.size - 1)
        if (idx == selected) { if (notify) onSelect?.invoke(idx); return }
        selected = idx
        performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
        anim?.cancel()
        anim = ValueAnimator.ofFloat(thumbPos, idx.toFloat()).apply {
            duration = 340
            interpolator = OvershootInterpolator(1.6f)
            addUpdateListener { thumbPos = it.animatedValue as Float; invalidate() }
            start()
        }
        if (notify) onSelect?.invoke(idx)
    }
}
