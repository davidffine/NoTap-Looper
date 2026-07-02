package com.notap.looper.ui.views

import android.animation.ArgbEvaluator
import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.SweepGradient
import android.view.View
import android.view.animation.AccelerateDecelerateInterpolator

// ==========================================
// 1. Comet Ring: Optimized Time Representation
// ==========================================
class CometRingView(context: Context) : View(context) {

    var progress: Float = 0f
        set(value) {
            field = value
            updateMatrix()
            invalidate()
        }

    private var _ringColor: Int = Color.parseColor("#333333")
    var ringColor: Int
        get() = _ringColor
        set(value) {
            if (_ringColor != value) animateColorChange(_ringColor, value)
        }

    private val matrix = Matrix()
    private var sweepGradient: SweepGradient? = null
    private val rect = RectF()

    private val backgroundTrackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#0A0A0A")
        style = Paint.Style.STROKE
        strokeWidth = 24f
    }

    private val cometPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 24f
        strokeCap = Paint.Cap.ROUND
    }

    private var colorAnimator: ValueAnimator? = null

    init {
        setLayerType(LAYER_TYPE_SOFTWARE, null)
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        val pad = 40f
        rect.set(pad, pad, w - pad, h - pad)
        rebuildGradient()
    }

    private fun rebuildGradient() {
        val cx = width / 2f
        val cy = height / 2f
        val colors = intArrayOf(Color.TRANSPARENT, _ringColor, _ringColor)
        val positions = floatArrayOf(0f, Math.max(progress * 0.9f, 0.01f), 1f)
        sweepGradient = SweepGradient(cx, cy, colors, positions)
        cometPaint.shader = sweepGradient
        cometPaint.setShadowLayer(35f, 0f, 0f, _ringColor)
        updateMatrix()
    }

    private fun updateMatrix() {
        sweepGradient?.let {
            matrix.setRotate(-90f, width / 2f, height / 2f)
            it.setLocalMatrix(matrix)
        }
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val cx = width / 2f
        val cy = height / 2f
        canvas.drawCircle(cx, cy, rect.width() / 2f, backgroundTrackPaint)

        if (progress > 0.01f) {
            canvas.drawArc(rect, -90f, progress * 360f, false, cometPaint)
        }
    }

    private fun animateColorChange(fromColor: Int, toColor: Int) {
        colorAnimator?.cancel()
        colorAnimator = ValueAnimator.ofObject(ArgbEvaluator(), fromColor, toColor).apply {
            duration = 300
            interpolator = AccelerateDecelerateInterpolator()
            addUpdateListener { animator ->
                _ringColor = animator.animatedValue as Int
                rebuildGradient()
                invalidate()
            }
            start()
        }
    }
}