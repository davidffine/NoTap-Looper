package com.notap.looper.ui.views

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.view.View

// ==========================================
// 2. Ripple Emitter: Acoustic String Physics
// ==========================================
class RippleEmitterView(context: Context) : View(context) {
    private class Ripple(var radius: Float, var alpha: Float)
    private val ripples = mutableListOf<Ripple>()
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private var maxRadius = 0f
    private var initialRadius = 0f

    fun fireRipple(color: Int) {
        paint.color = color
        ripples.add(Ripple(initialRadius, 1f))
        invalidate()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        // הכפלנו את הרדיוס המקסימלי במקום לחלק אותו, כדי שהגל לא ידעך לפני שיגיע לקצה העליון
        maxRadius = Math.max(w, h) * 1.5f
        initialRadius = Math.min(w, h) / 3f
    }

    override fun onDraw(canvas: Canvas) {
        if (ripples.isEmpty()) return
        val cx = width / 2f
        val cy = height / 2f
        var needsNextFrame = false
        val iterator = ripples.iterator()

        while (iterator.hasNext()) {
            val r = iterator.next()
            paint.alpha = (r.alpha * 60).toInt() // Peak opacity ~23%
            canvas.drawCircle(cx, cy, r.radius, paint)

            // Non-linear acoustic physics (ADSR mimic)
            r.radius += (maxRadius - r.radius) * 0.18f // Easing out expansion
            r.alpha *= 0.88f                           // Exponential decay

            if (r.alpha <= 0.01f) {
                iterator.remove()
            } else {
                needsNextFrame = true
            }
        }
        if (needsNextFrame) invalidate()
    }
}