package com.notap.looper.ui.views

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RadialGradient
import android.graphics.Shader
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.animation.OvershootInterpolator
import com.notap.looper.ui.Design

/**
 * Icon-only action button that morphs between UPLOAD (arrow up) and
 * DOWNLOAD (arrow down). The arrow rotates 180° across the morph while a
 * fixed tray stays put; in download mode it glows and gently bobs to signal
 * "a loop is ready to grab."
 */
class IconMorphButton(context: Context) : View(context) {

    var onTap: (() -> Unit)? = null

    private var download = false
    private var morph = 0f          // 0 = upload, 1 = download
    private var pulse = 0f          // continuous phase for the glow
    private var morphAnim: ValueAnimator? = null
    private var pulseAnim: ValueAnimator? = null

    private val bgPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val borderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val glowPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val icon = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeCap = Paint.Cap.ROUND; strokeJoin = Paint.Join.ROUND
    }
    private val arrowPath = Path()
    private val trayPath = Path()   // reused each frame — no per-onDraw allocation

    fun setDownload(value: Boolean) {
        if (value == download) return
        download = value
        morphAnim?.cancel()
        morphAnim = ValueAnimator.ofFloat(morph, if (value) 1f else 0f).apply {
            duration = 420
            interpolator = OvershootInterpolator(2.0f)
            addUpdateListener { morph = it.animatedValue as Float; invalidate() }
            start()
        }
        if (value) startPulse() else stopPulse()
        if (value) performHapticFeedback(HapticFeedbackConstants.CONTEXT_CLICK)
    }

    private fun startPulse() {
        if (pulseAnim?.isRunning == true) return
        pulseAnim = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = 1500
            repeatCount = ValueAnimator.INFINITE
            addUpdateListener { pulse = it.animatedValue as Float; invalidate() }
            start()
        }
    }

    private fun stopPulse() { pulseAnim?.cancel(); pulseAnim = null; pulse = 0f; invalidate() }

    override fun onDetachedFromWindow() { morphAnim?.cancel(); pulseAnim?.cancel(); super.onDetachedFromWindow() }

    override fun onDraw(c: Canvas) {
        val cx = width / 2f; val cy = height / 2f
        val r = minOf(width, height) / 2f - Design.dpf(1, context)
        val accent = Design.mix(Design.cyan, Design.green, morph)

        // pulsing glow (download only)
        if (morph > 0.02f) {
            val p = 0.5f - 0.5f * kotlin.math.cos(pulse * 2f * Math.PI).toFloat()
            glowPaint.shader = RadialGradient(
                cx, cy, r * (1.15f + 0.15f * p),
                intArrayOf(Design.alpha(Design.green, (0.10f + 0.22f * p) * morph), Color.TRANSPARENT),
                floatArrayOf(0f, 1f), Shader.TileMode.CLAMP
            )
            c.drawCircle(cx, cy, r * 1.3f, glowPaint)
        }

        // glass body
        bgPaint.color = Design.surface
        c.drawCircle(cx, cy, r, bgPaint)
        borderPaint.strokeWidth = Design.dpf(1, context)
        borderPaint.color = Design.mix(Design.stroke, Design.green, morph * 0.8f)
        c.drawCircle(cx, cy, r, borderPaint)

        icon.color = accent
        icon.strokeWidth = Design.dpf(2, context)

        val u = Design.dpf(1, context)
        val bob = morph * kotlin.math.sin(pulse * 2f * Math.PI).toFloat() * 1.2f * u   // gentle bob in download

        // fixed tray (open-top inbox) at the bottom — reuse trayPath (no alloc)
        val trayHalf = 6f * u
        val trayY = cy + 7f * u
        trayPath.reset()
        trayPath.moveTo(cx - trayHalf, trayY - 3f * u)
        trayPath.lineTo(cx - trayHalf, trayY)
        trayPath.lineTo(cx + trayHalf, trayY)
        trayPath.lineTo(cx + trayHalf, trayY - 3f * u)
        c.drawPath(trayPath, icon)

        // arrow — drawn pointing up, rotated by morph*180 to point down
        val shaftTop = cy - 8f * u
        val shaftBottom = cy + 2f * u
        val headHalf = 4f * u
        arrowPath.reset()
        arrowPath.moveTo(cx, shaftBottom)
        arrowPath.lineTo(cx, shaftTop)
        arrowPath.moveTo(cx - headHalf, shaftTop + headHalf)
        arrowPath.lineTo(cx, shaftTop)
        arrowPath.lineTo(cx + headHalf, shaftTop + headHalf)

        c.save()
        c.translate(0f, bob)
        c.rotate(morph * 180f, cx, (shaftTop + shaftBottom) / 2f)
        c.drawPath(arrowPath, icon)
        c.restore()
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.action) {
            MotionEvent.ACTION_DOWN ->
                animate().scaleX(0.85f).scaleY(0.85f).alpha(0.7f).setDuration(50).start()
            MotionEvent.ACTION_UP -> {
                animate().scaleX(1f).scaleY(1f).alpha(1f).setDuration(150).start()
                performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                onTap?.invoke()
            }
            MotionEvent.ACTION_CANCEL ->
                animate().scaleX(1f).scaleY(1f).alpha(1f).setDuration(150).start()
        }
        return true
    }
}
