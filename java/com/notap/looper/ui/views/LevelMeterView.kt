package com.notap.looper.ui.views

import android.content.Context
import android.graphics.Canvas
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.view.View
import com.notap.looper.ui.Design

/**
 * Slim horizontal input-level meter with a decaying peak-hold marker.
 * Doubles as gain feedback — critical for a mic-driven app (tells the player
 * the engine is actually hearing them).
 */
class LevelMeterView(context: Context) : View(context) {

    var level: Float = 0f
        set(v) {
            field = v.coerceIn(0f, 1f)
            if (field > peak) peak = field
            invalidate()
        }
    private var peak = 0f

    private val trackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Design.strokeSoft }
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val peakPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Design.textHi }
    private val track = RectF()
    private val fill = RectF()
    private var grad: Shader? = null

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        grad = LinearGradient(
            0f, 0f, w.toFloat(), 0f,
            intArrayOf(Design.green, Design.green, Design.amber, Design.red),
            floatArrayOf(0f, 0.6f, 0.82f, 1f), Shader.TileMode.CLAMP
        )
        fillPaint.shader = grad
    }

    override fun onDraw(c: Canvas) {
        val r = height / 2f
        track.set(0f, 0f, width.toFloat(), height.toFloat())
        c.drawRoundRect(track, r, r, trackPaint)

        val w = width * level
        if (w > 1f) {
            fill.set(0f, 0f, w, height.toFloat())
            c.drawRoundRect(fill, r, r, fillPaint)
        }
        if (peak > 0.02f) {
            val px = (width * peak).coerceIn(r, width - r)
            c.drawRoundRect(px - Design.dpf(1.5f, context), 0f, px + Design.dpf(1.5f, context),
                height.toFloat(), r, r, peakPaint)
            peak -= 0.012f
            invalidate()
        }
    }
}
