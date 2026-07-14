package com.notap.looper.ui.views

import android.content.Context
import android.graphics.BlurMaskFilter
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.PorterDuffXfermode
import android.graphics.RectF
import android.view.View
import com.notap.looper.ui.Design

/**
 * Onboarding focus scrim. Dims the whole screen, then punches SOFT-EDGED holes
 * that let the hero, the coach card, and the current target control read at full
 * brightness — drawing the eye without arrows or bubbles.
 *
 * PERFORMANCE: this view redraws ONLY when [setHoles] is called (a handful of
 * times per session, on stage change) — never in a render loop. The offscreen
 * saveLayer needed for PorterDuff.CLEAR is therefore paid a few times total, not
 * per frame. It's transparent to touches (see [onTouchEvent] = false) so the lit
 * controls beneath it stay tappable.
 */
class SpotlightView(context: Context) : View(context) {

    private var circleCx = 0f
    private var circleCy = 0f
    private var circleR = 0f
    private var rects: List<RectF> = emptyList()
    private var active = false

    private val scrimPaint = Paint().apply { color = Color.parseColor("#000000") }
    private val scrimAlpha = 115   // ~0.45 — subtle, keeps chrome legible but recessed

    // CLEAR xfermode carves the scrim away; the blur feathers the hole edges so
    // nothing looks like a hard stencil cut.
    private val clearPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        xfermode = PorterDuffXfermode(PorterDuff.Mode.CLEAR)
        maskFilter = BlurMaskFilter(Design.dpf(14f, context), BlurMaskFilter.Blur.NORMAL)
    }

    init {
        // The saveLayer + CLEAR path needs software rendering to composite correctly.
        setLayerType(LAYER_TYPE_SOFTWARE, null)
    }

    /** hero circle (cx,cy,r in this view's coords) + rounded-rect holes for the
     *  coach card and the target control. Empty circle (r<=0) skips the circle. */
    fun setHoles(cx: Float, cy: Float, r: Float, holeRects: List<RectF>) {
        circleCx = cx; circleCy = cy; circleR = r
        rects = holeRects
        active = true
        invalidate()
    }

    fun hide() {
        active = false
        rects = emptyList()
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        if (!active) return
        val layer = canvas.saveLayer(0f, 0f, width.toFloat(), height.toFloat(), null)
        scrimPaint.alpha = scrimAlpha
        canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), scrimPaint)
        if (circleR > 0f) canvas.drawCircle(circleCx, circleCy, circleR, clearPaint)
        val rad = Design.dpf(18f, context)
        for (rc in rects) canvas.drawRoundRect(rc, rad, rad, clearPaint)
        canvas.restoreToCount(layer)
    }

    // Never intercept touches — the illuminated controls underneath must stay live.
    override fun onTouchEvent(event: android.view.MotionEvent) = false
}
