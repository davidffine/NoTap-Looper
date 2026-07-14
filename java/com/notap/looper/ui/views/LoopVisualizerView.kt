package com.notap.looper.ui.views

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.SweepGradient
import android.view.View
import com.notap.looper.ui.Design
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

/**
 * The hero instrument — the merged successor of the old VuMeterView (reactive
 * halo), RippleEmitterView (transient rings) and CometRingView (progress arc).
 * One hardware-accelerated canvas layering, back to front:
 *   state-colour bloom · audio-reactive halo · transient ripples ·
 *   loop track · comet progress arc + glowing head · beat ticks · glass orb.
 *
 * STATE LEGIBILITY (for POV/Reels capture): every layer is tinted by the single
 * animated [accent], which the Activity drives per engine state — RECORDING red,
 * LOOPING green, IDLE cyan, etc. The bloom is deliberately large and strong so
 * the whole frame glows the state colour, readable from across a room.
 *
 * PERFORMANCE: onDraw does ZERO object allocation. Every gradient is built once
 * and cached; it is rebuilt only when geometry (onSizeChanged) or the accent
 * colour changes — never per frame. Per-frame motion is applied by mutating
 * reused Matrix objects (setLocalMatrix) and Paint alpha, both allocation-free.
 * (UI-thread GC does not starve the Oboe real-time callback — it lives on a
 * dedicated thread off the JVM heap — but eliminating churn is what keeps the
 * 60 fps steady, which is the actual product requirement.)
 */
class LoopVisualizerView(context: Context) : View(context) {

    // ---- public, pushed from the Activity/ViewModel ----
    var progress: Float = 0f
        set(v) { field = v.coerceIn(0f, 1f); invalidate() }

    var level: Float = 0f
        set(v) { field = v.coerceIn(0f, 1f) }

    /** How hard the orb breathes: 0 idle-calm, 1 recording-urgent. */
    var intensity: Float = 0f

    var beatCount: Int = 0

    // Loop amplitude envelope (per bin) — wrapped around the ring when a loop exists
    private var waveform: FloatArray = FloatArray(0)
    fun setWaveform(w: FloatArray) { waveform = w }

    private var accent: Int = Design.cyan
    private var accentAnim: ValueAnimator? = null
    fun setAccent(color: Int, animated: Boolean = true) {
        if (color == accent) return
        if (!animated) { accent = color; rebuildAccentShaders(); return }
        val from = accent
        accentAnim?.cancel()
        accentAnim = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = 380
            // Accent shaders are rebuilt on each cross-fade frame — bounded to
            // ~23 frames per transition (a handful of times per session), NOT the
            // steady-state hot path. Steady-state onDraw stays allocation-free.
            addUpdateListener { accent = Design.mix(from, color, it.animatedValue as Float); rebuildAccentShaders() }
            start()
        }
    }

    // ---- reactive halo bars (shimmer that grows with level) ----
    private val barCount = 76
    private val barLen = FloatArray(barCount)

    // ---- transient ripples: fixed pool, no per-fire / per-frame allocation ----
    private class Ripple { var r = 0f; var a = 0f }
    private val ripplePool = Array(6) { Ripple() }
    private var rippleCursor = 0
    fun fireRipple() {
        val slot = ripplePool.firstOrNull { it.a <= 0.03f } ?: ripplePool[rippleCursor].also {
            rippleCursor = (rippleCursor + 1) % ripplePool.size
        }
        slot.r = 0f; slot.a = 1f
    }

    // ---- paints ----
    private val glowPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val haloPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { strokeCap = Paint.Cap.ROUND }
    private val ripplePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val trackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; color = Design.strokeSoft
    }
    private val cometPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeCap = Paint.Cap.ROUND
    }
    private val tickPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeCap = Paint.Cap.ROUND
    }
    private val headPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val orbRimPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val orbBodyPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val orbBloomPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val orbGlossPaint = Paint(Paint.ANTI_ALIAS_FLAG)

    // ---- cached shaders (built off the hot path) ----
    private var glowShader: RadialGradient? = null
    private var cometShader: SweepGradient? = null
    private var headShader: RadialGradient? = null
    private var orbBloomShader: RadialGradient? = null   // accent-dependent
    // static (geometry-only) shaders
    private var orbBodyShader: RadialGradient? = null
    private var orbGlossShader: RadialGradient? = null

    // ---- reused matrices for per-frame motion (allocation-free) ----
    private val glowMatrix = Matrix()
    private val headMatrix = Matrix()

    private val ringRect = RectF()

    private var cx = 0f; private var cy = 0f; private var radius = 0f
    private var rRing = 0f
    private var glowBaseR = 1f
    private var headR = 1f
    private var orbBaseR = 1f
    private var startNs = System.nanoTime()

    private var driver: ValueAnimator? = null

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        cx = w / 2f; cy = h / 2f
        radius = min(w, h) / 2f - Design.dpf(8, context)
        rRing = radius * 0.80f
        glowBaseR = rRing * 1.35f
        headR = Design.dpf(15, context)
        orbBaseR = rRing * 0.66f

        trackPaint.strokeWidth = Design.dpf(5, context)
        cometPaint.strokeWidth = Design.dpf(6, context)
        tickPaint.strokeWidth = Design.dpf(2, context)
        orbRimPaint.strokeWidth = Design.dpf(1.5f, context)
        haloPaint.strokeWidth = Design.dpf(2.5f, context)
        ringRect.set(cx - rRing, cy - rRing, cx + rRing, cy + rRing)

        rebuildStaticShaders()
        rebuildAccentShaders()
    }

    // Geometry-only shaders — independent of accent/level; built once per layout.
    private fun rebuildStaticShaders() {
        if (radius <= 1f) return
        orbBodyShader = RadialGradient(
            cx, cy - orbBaseR * 0.35f, orbBaseR * 1.5f,
            intArrayOf(Design.surfaceHi, Design.surface, Design.orbBase),
            floatArrayOf(0f, 0.5f, 1f), Shader.TileMode.CLAMP
        )
        orbBodyPaint.shader = orbBodyShader
        orbGlossShader = RadialGradient(
            cx, cy - orbBaseR * 0.5f, orbBaseR * 0.9f,
            intArrayOf(Design.alpha(Color.WHITE, 0.10f), Color.TRANSPARENT),
            floatArrayOf(0f, 0.7f), Shader.TileMode.CLAMP
        )
        orbGlossPaint.shader = orbGlossShader
    }

    // Accent-tinted shaders — rebuilt on state colour change. Colours are baked
    // at full strength; per-frame intensity is applied via Paint.alpha, and the
    // size/position via a reused Matrix — so onDraw never rebuilds these.
    private fun rebuildAccentShaders() {
        if (radius <= 1f) return

        glowShader = RadialGradient(
            cx, cy, glowBaseR,
            intArrayOf(Design.alpha(accent, 1f), Design.alpha(accent, 0.45f), Color.TRANSPARENT),
            floatArrayOf(0f, 0.5f, 1f), Shader.TileMode.CLAMP
        )
        glowPaint.shader = glowShader

        cometShader = SweepGradient(
            cx, cy,
            intArrayOf(Color.TRANSPARENT, Design.alpha(accent, 0.18f), accent, accent),
            floatArrayOf(0f, 0.5f, 0.97f, 1f)
        ).also {
            // rotate so the fade-in tail begins at the 12-o'clock arc start
            glowMatrix.setRotate(-90f, cx, cy)   // borrow glowMatrix transiently (off hot path)
            it.setLocalMatrix(glowMatrix)
        }
        cometPaint.shader = cometShader

        headShader = RadialGradient(
            0f, 0f, headR,
            intArrayOf(Color.WHITE, accent, Color.TRANSPARENT),
            floatArrayOf(0f, 0.35f, 1f), Shader.TileMode.CLAMP
        )
        headPaint.shader = headShader

        orbBloomShader = RadialGradient(
            cx, cy, orbBaseR,
            intArrayOf(Design.alpha(accent, 1f), Color.TRANSPARENT),
            floatArrayOf(0f, 1f), Shader.TileMode.CLAMP
        )
        orbBloomPaint.shader = orbBloomShader
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        startNs = System.nanoTime()
        driver = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = 1000
            repeatCount = ValueAnimator.INFINITE
            addUpdateListener { invalidate() }
            start()
        }
    }

    override fun onDetachedFromWindow() {
        driver?.cancel(); driver = null
        accentAnim?.cancel()
        super.onDetachedFromWindow()
    }

    override fun onDraw(canvas: Canvas) {
        if (radius <= 1f) return   // not laid out yet — gradients need positive radius
        val t = (System.nanoTime() - startNs) / 1_000_000_000f
        val breath = 0.5f + 0.5f * sin(t * (1.1f + intensity * 3.2f).toDouble()).toFloat()

        drawGlow(canvas, breath)
        if (waveform.size >= 2) drawWaveform(canvas) else drawHalo(canvas, t)
        drawRipples(canvas)
        canvas.drawCircle(cx, cy, rRing, trackPaint)
        drawComet(canvas)
        drawTicks(canvas)
        drawHead(canvas)
        drawOrb(canvas, breath)
    }

    // Screen-filling state-colour bloom. Boosted vs. the arm's-length original so
    // the frame visibly glows the state colour on video. Size pulses with
    // breath+level via a matrix scale; overall strength via paint alpha — no alloc.
    private fun drawGlow(c: Canvas, breath: Float) {
        val glowR = rRing * (1.35f + 0.12f * breath + 0.30f * level)
        val a = (0.14f + 0.42f * level + 0.07f * breath * intensity).coerceIn(0f, 1f)
        val s = glowR / glowBaseR
        glowMatrix.setScale(s, s, cx, cy)
        glowShader?.setLocalMatrix(glowMatrix)
        glowPaint.alpha = (a * 255f).toInt()
        c.drawCircle(cx, cy, glowR, glowPaint)
    }

    private fun drawHalo(c: Canvas, t: Float) {
        val inner = rRing * 1.03f
        val maxLen = radius * 0.16f
        for (i in 0 until barCount) {
            val phase = t * 1.6f + i * 0.42f
            val target = maxLen * level * (0.30f + 0.70f * abs(sin(phase.toDouble()).toFloat()))
            barLen[i] += (target - barLen[i]) * 0.25f
            if (barLen[i] < 0.4f) continue
            val ang = (i.toFloat() / barCount) * 2f * PI.toFloat() - PI.toFloat() / 2f + t * 0.18f
            val ca = cos(ang.toDouble()).toFloat(); val sa = sin(ang.toDouble()).toFloat()
            haloPaint.color = Design.alpha(accent, 0.18f + 0.55f * (barLen[i] / maxLen))
            c.drawLine(cx + ca * inner, cy + sa * inner,
                       cx + ca * (inner + barLen[i]), cy + sa * (inner + barLen[i]), haloPaint)
        }
    }

    private fun drawWaveform(c: Canvas) {
        val bins = waveform.size
        var mx = 1e-4f
        for (v in waveform) if (v > mx) mx = v
        val inner = rRing * 1.03f
        val maxLen = radius * 0.17f
        haloPaint.strokeWidth = Design.dpf(2f, context)
        for (i in 0 until bins) {
            val amp = (waveform[i] / mx).coerceIn(0f, 1f)
            val len = maxLen * (0.05f + 0.95f * amp)
            val ang = (i.toFloat() / bins) * 2f * PI.toFloat() - PI.toFloat() / 2f
            val ca = cos(ang.toDouble()).toFloat(); val sa = sin(ang.toDouble()).toFloat()
            val played = (i.toFloat() / bins) <= progress
            haloPaint.color = Design.alpha(accent, if (played) 0.35f + 0.5f * amp else 0.14f + 0.32f * amp)
            c.drawLine(cx + ca * inner, cy + sa * inner,
                       cx + ca * (inner + len), cy + sa * (inner + len), haloPaint)
        }
    }

    private fun drawRipples(c: Canvas) {
        for (rp in ripplePool) {
            if (rp.a <= 0.03f) continue
            ripplePaint.color = Design.alpha(accent, rp.a * 0.5f)
            ripplePaint.strokeWidth = Design.dpf(2, context) * rp.a
            c.drawCircle(cx, cy, rRing * (0.7f + rp.r), ripplePaint)
            rp.r += (1.9f - rp.r) * 0.06f
            rp.a *= 0.93f
        }
    }

    private fun drawComet(c: Canvas) {
        if (progress <= 0.001f) return
        c.drawArc(ringRect, -90f, progress * 360f, false, cometPaint)
    }

    private fun drawTicks(c: Canvas) {
        if (beatCount < 2 || beatCount > 64) return
        tickPaint.color = Design.alpha(Design.textLo, 0.7f)
        val inner = rRing - Design.dpf(4, context)
        val outer = rRing + Design.dpf(4, context)
        for (b in 0 until beatCount) {
            val ang = (b.toFloat() / beatCount) * 2f * PI.toFloat() - PI.toFloat() / 2f
            val ca = cos(ang.toDouble()).toFloat(); val sa = sin(ang.toDouble()).toFloat()
            c.drawLine(cx + ca * inner, cy + sa * inner, cx + ca * outer, cy + sa * outer, tickPaint)
        }
    }

    private fun drawHead(c: Canvas) {
        if (progress <= 0.001f) return
        val ang = progress * 2f * PI.toFloat() - PI.toFloat() / 2f
        val hx = cx + cos(ang.toDouble()).toFloat() * rRing
        val hy = cy + sin(ang.toDouble()).toFloat() * rRing
        headMatrix.setTranslate(hx, hy)
        headShader?.setLocalMatrix(headMatrix)
        c.drawCircle(hx, hy, headR, headPaint)
        headPaint.color = Color.WHITE          // primitive set, no alloc
        c.drawCircle(hx, hy, Design.dpf(3.5f, context), headPaint)
    }

    private fun drawOrb(c: Canvas, breath: Float) {
        val rOrb = orbBaseR * (1f + (0.012f + 0.03f * intensity) * (breath - 0.5f) * 2f)

        orbRimPaint.color = Design.alpha(accent, 0.6f)
        c.drawCircle(cx, cy, rOrb + Design.dpf(2, context), orbRimPaint)

        c.drawCircle(cx, cy, rOrb, orbBodyPaint)           // static spherical body

        val bloom = (0.06f + 0.20f * level + 0.14f * intensity).coerceIn(0f, 1f)
        orbBloomPaint.alpha = (bloom * 255f).toInt()       // accent bloom, alpha-modulated
        c.drawCircle(cx, cy, rOrb, orbBloomPaint)

        c.drawCircle(cx, cy - orbBaseR * 0.35f, orbBaseR * 0.8f, orbGlossPaint)  // static top gloss
    }

    /** Radius of the tappable orb in px (for hit-testing / press feedback). */
    fun orbRadius(): Float = radius * 0.80f * 0.66f
}
