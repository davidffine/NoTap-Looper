package com.notap.looper

import android.Manifest
import android.animation.ArgbEvaluator
import android.animation.ValueAnimator
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.*
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.view.Choreographer
import android.view.GestureDetector
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.animation.AccelerateDecelerateInterpolator
import android.view.animation.OvershootInterpolator
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import kotlin.math.abs

// ==========================================
// System Ontology: Typed States
// ==========================================
enum class EngineState {
    IDLE, RECORDING, PROCESSING, LOOPING, OVERDUBBING, CALIBRATING, UNKNOWN;

    companion object {
        fun fromString(state: String?): EngineState {
            return entries.find { it.name == state } ?: UNKNOWN
        }
    }
}

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
        maxRadius = Math.max(w, h) / 1.4f
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

// ==========================================
// 3. The Studio Engine (Main Activity)
// ==========================================
class MainActivity : AppCompatActivity() {

    private var audioEngine: AudioEngine? = null

    // UI Components
    private lateinit var modeText: TextView
    private lateinit var statusText: TextView
    private lateinit var bpmText: TextView
    private lateinit var cometRing: CometRingView
    private lateinit var rippleEmitter: RippleEmitterView
    private lateinit var stompLabel: TextView
    private lateinit var stompContainer: FrameLayout

    private var currentModeIndex = 0
    private val modes = arrayOf("AUTO SILENCE", "TAP & TRIM", "FIXED BPM")
    private var lastKnownState = EngineState.UNKNOWN

    // Hardware Color Palette
    private val palette = object {
        val cyan   = Color.parseColor("#00E5FF")
        val red    = Color.parseColor("#FF1744")
        val green  = Color.parseColor("#00E676")
        val amber  = Color.parseColor("#FF9100")
        val void   = Color.parseColor("#000000")
        val idle   = Color.parseColor("#222222")
    }

    private var pulseAnimator: ValueAnimator? = null
    private val telemetryBuffer = FloatArray(3)

    // Jitter low-pass filter states
    private var currentJitterX = 0f
    private var currentJitterY = 0f

    // Hardware Vsync callback
    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            pollAndRender()
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setupStudioUI()

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.RECORD_AUDIO), 1)
        } else {
            bootEngine()
        }
    }

    private fun Number.dpToPx(): Int = (this.toFloat() * resources.displayMetrics.density).toInt()

    private fun setupStudioUI() {
        val rootLayout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setBackgroundColor(palette.void)
            setPadding(0, 48.dpToPx(), 0, 0)
        }

        val lcdScreen = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            background = GradientDrawable().apply {
                shape = GradientDrawable.RECTANGLE
                cornerRadius = 16f.dpToPx().toFloat()
                colors = intArrayOf(Color.parseColor("#15181E"), Color.parseColor("#0A0B0E"))
                setStroke(2.dpToPx(), Color.parseColor("#22252A"))
            }
            layoutParams = LinearLayout.LayoutParams(
                320.dpToPx(),
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply { setMargins(0, 0, 0, 72.dpToPx()) }
            setPadding(0, 24.dpToPx(), 0, 24.dpToPx())
            elevation = 12f
        }

        modeText = TextView(this).apply {
            text = "MODE: ${modes[currentModeIndex]}"
            textSize = 12f
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            letterSpacing = 0.2f
            setTextColor(Color.parseColor("#707885"))
        }

        statusText = TextView(this).apply {
            text = "BOOTING"
            textSize = 28f
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            letterSpacing = 0.05f
            setTextColor(Color.WHITE)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { setMargins(0, 8.dpToPx(), 0, 4.dpToPx()) }
        }

        bpmText = TextView(this).apply {
            text = "BPM: --"
            textSize = 14f
            typeface = Typeface.MONOSPACE
            letterSpacing = 0.1f
            setTextColor(Color.parseColor("#505560"))
        }

        lcdScreen.addView(modeText)
        lcdScreen.addView(statusText)
        lcdScreen.addView(bpmText)

        val gestureDetector = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            override fun onFling(e1: MotionEvent?, e2: MotionEvent, velocityX: Float, velocityY: Float): Boolean {
                if (abs(velocityX) > 200) {
                    cycleDetectionMode(if (velocityX > 0) 1 else -1)
                    lcdScreen.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                    return true
                }
                return false
            }
        })
        lcdScreen.setOnTouchListener { _, event -> gestureDetector.onTouchEvent(event); true }

        val pedalContainer = FrameLayout(this).apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
        }

        rippleEmitter = RippleEmitterView(this).apply {
            layoutParams = FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        }

        cometRing = CometRingView(this).apply {
            layoutParams = FrameLayout.LayoutParams(340.dpToPx(), 340.dpToPx()).apply {
                gravity = Gravity.CENTER
            }
        }

        stompContainer = FrameLayout(this).apply {
            val btnSize = 190.dpToPx()
            layoutParams = FrameLayout.LayoutParams(btnSize, btnSize).apply {
                gravity = Gravity.CENTER
            }
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                colors = intArrayOf(Color.parseColor("#1E1E1E"), Color.parseColor("#090909"))
                gradientType = GradientDrawable.RADIAL_GRADIENT
                gradientRadius = btnSize.toFloat()
                setStroke(4.dpToPx(), Color.parseColor("#151515"))
            }
            elevation = 20f
        }

        stompLabel = TextView(this).apply {
            text = "INIT"
            textSize = 18f
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            letterSpacing = 0.15f
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
            layoutParams = FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        }

        pedalContainer.setOnTouchListener { _, event ->
            when (event.action) {
                MotionEvent.ACTION_DOWN -> {
                    pedalContainer.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                    stopPulsing()
                    stompContainer.animate().scaleX(0.92f).scaleY(0.92f)
                        .setDuration(60).setInterpolator(OvershootInterpolator(1.5f)).start()
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    stompContainer.animate().scaleX(1f).scaleY(1f)
                        .setDuration(120).setInterpolator(OvershootInterpolator(1.5f)).start()
                    if (event.action == MotionEvent.ACTION_UP) handleActionClick()
                }
            }
            true
        }

        stompContainer.addView(stompLabel)
        pedalContainer.addView(rippleEmitter)
        pedalContainer.addView(cometRing)
        pedalContainer.addView(stompContainer)

        rootLayout.addView(lcdScreen)
        rootLayout.addView(pedalContainer)

        setContentView(rootLayout)
    }

    private fun cycleDetectionMode(direction: Int = 1) {
        currentModeIndex = (currentModeIndex + direction + modes.size) % modes.size
        modeText.text = "MODE: ${modes[currentModeIndex]}"
        audioEngine?.setDetectionMode(currentModeIndex)
    }

    private fun bootEngine() {
        try {
            audioEngine = AudioEngine()
            audioEngine?.startEngine()
            audioEngine?.setDetectionMode(currentModeIndex)
            // Hook to hardware Vsync
            Choreographer.getInstance().postFrameCallback(frameCallback)
        } catch (e: Exception) {
            statusText.text = "ERR: KERNEL"
            statusText.setTextColor(palette.red)
        }
    }

    private fun pollAndRender() {
        val engine = audioEngine ?: return

        // 1. Poll underlying DSP reality
        engine.pollTelemetry(telemetryBuffer)
        val rms = telemetryBuffer[0]
        val noiseStdDev = telemetryBuffer[1]
        val transientHit = telemetryBuffer[2] > 0f

        val rawState = engine.getCurrentState()
        val engineState = EngineState.fromString(rawState)
        val bpm = engine.getEstimatedBPM()

        // 2. Render physical dynamics
        updateVisualDynamics(rms, noiseStdDev, transientHit)

        // 3. Render logic state
        cometRing.progress = engine.getLoopPosition()

        if (engineState != lastKnownState) {
            transitionToState(engineState, rawState)
            lastKnownState = engineState
        }

        bpmText.text = if (bpm > 0f) "BPM: ${String.format("%.1f", bpm)}" else "BPM: --"
    }

    private fun transitionToState(state: EngineState, rawString: String) {
        statusText.text = rawString

        // Minimalism rule
        val isActive = state in listOf(EngineState.RECORDING, EngineState.LOOPING, EngineState.OVERDUBBING)
        val uiAlpha = if (isActive) 0.1f else 1.0f
        modeText.animate().alpha(uiAlpha).setDuration(400).start()
        bpmText.animate().alpha(uiAlpha).setDuration(400).start()

        when (state) {
            EngineState.LOOPING -> {
                stompLabel.text = "OVERDUB"
                statusText.setTextColor(palette.green)
                cometRing.ringColor = palette.green
                stopPulsing()
            }
            EngineState.OVERDUBBING -> {
                stompLabel.text = "LOCK"
                statusText.setTextColor(palette.amber)
                cometRing.ringColor = palette.amber
                stopPulsing()
                stompContainer.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_PRESS)
            }
            EngineState.RECORDING -> {
                stompLabel.text = if (currentModeIndex == 1) "TAP STOP" else "REC"
                statusText.setTextColor(palette.red)
                cometRing.ringColor = palette.red
                startPulsing()
                stompContainer.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_PRESS)
            }
            EngineState.PROCESSING -> {
                stompLabel.text = "SYNC"
                statusText.setTextColor(Color.WHITE)
                cometRing.ringColor = Color.WHITE
                stopPulsing()
            }
            EngineState.CALIBRATING -> {
                stompLabel.text = "WAIT"
                statusText.setTextColor(Color.parseColor("#555555"))
                cometRing.ringColor = palette.idle
                stopPulsing()
            }
            EngineState.IDLE, EngineState.UNKNOWN -> {
                if (currentModeIndex == 1) {
                    stompLabel.text = "TAP START"
                    statusText.setTextColor(palette.cyan)
                    cometRing.ringColor = palette.cyan
                } else {
                    stompLabel.text = "PLAY"
                    statusText.setTextColor(Color.WHITE)
                    cometRing.ringColor = palette.idle
                }
                stopPulsing()
            }
        }
    }

    private fun updateVisualDynamics(rms: Float, noiseStdDev: Float, transientHit: Boolean) {
        val targetAlpha = 0.3f + (rms * 6f).coerceIn(0f, 0.7f)
        cometRing.alpha = targetAlpha

        val jitterIntensity = noiseStdDev * 300f
        val targetJitterX = (Math.random().toFloat() - 0.5f) * jitterIntensity
        val targetJitterY = (Math.random().toFloat() - 0.5f) * jitterIntensity

        currentJitterX += (targetJitterX - currentJitterX) * 0.15f
        currentJitterY += (targetJitterY - currentJitterY) * 0.15f

        cometRing.translationX = currentJitterX
        cometRing.translationY = currentJitterY

        if (transientHit) {
            rippleEmitter.fireRipple(Color.WHITE)
        }
    }

    private fun startPulsing() {
        if (pulseAnimator == null || !pulseAnimator!!.isRunning) {
            pulseAnimator = ValueAnimator.ofFloat(1f, 1.04f).apply {
                duration = 600
                repeatCount = ValueAnimator.INFINITE
                repeatMode = ValueAnimator.REVERSE
                interpolator = AccelerateDecelerateInterpolator()
                addUpdateListener { animator ->
                    val scale = animator.animatedValue as Float
                    cometRing.scaleX = scale
                    cometRing.scaleY = scale
                }
                start()
            }
        }
    }

    private fun stopPulsing() {
        pulseAnimator?.cancel()
        cometRing.animate().scaleX(1f).scaleY(1f).setDuration(200).start()
    }

    private fun handleActionClick() {
        audioEngine?.let { engine ->
            when (EngineState.fromString(engine.getCurrentState())) {
                EngineState.LOOPING -> engine.executeOverdub()
                EngineState.OVERDUBBING -> engine.executeLoop()
                EngineState.IDLE -> if (currentModeIndex == 1) engine.executeRecordStart()
                EngineState.RECORDING -> if (currentModeIndex == 1) engine.executeRecordStop()
                else -> {}
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        pulseAnimator?.cancel()
        Choreographer.getInstance().removeFrameCallback(frameCallback)
        audioEngine?.stopEngine()
    }
}