package com.notap.looper

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

// ==========================================
// מנוע ציור: טבעת התקדמות עם אפקט Glow
// ==========================================
class RingProgressView(context: Context) : View(context) {
    var progress: Float = 0f
        set(value) {
            field = value
            invalidate()
        }

    var ringColor: Int = Color.parseColor("#333333")
        set(value) {
            field = value
            progressPaint.color = value
            // הוספת אפקט זוהר חזותי לטבעת
            progressPaint.setShadowLayer(25f, 0f, 0f, value)
            invalidate()
        }

    init {
        // קריטי: מאפשר רינדור של הצללות והילות (Glow) בקנבס
        setLayerType(LAYER_TYPE_SOFTWARE, null)
    }

    private val backgroundPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#121212")
        style = Paint.Style.STROKE
        strokeWidth = 30f
        strokeCap = Paint.Cap.ROUND
    }

    private val progressPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 30f
        strokeCap = Paint.Cap.ROUND
    }

    private val rect = RectF()

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        val pad = 60f
        rect.set(pad, pad, w - pad, h - pad)
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        canvas.drawArc(rect, 0f, 360f, false, backgroundPaint)
        // הציור מתחיל מ-12 בלילה (-90 מעלות)
        canvas.drawArc(rect, -90f, progress * 360f, false, progressPaint)
    }
}

// ==========================================
// ממשק המשתמש הראשי
// ==========================================
class MainActivity : AppCompatActivity() {

    private var audioEngine: AudioEngine? = null

    private lateinit var modeText: TextView
    private lateinit var statusText: TextView
    private lateinit var bpmText: TextView
    private lateinit var ringView: RingProgressView
    private lateinit var actionLabel: TextView
    private lateinit var actionContainer: FrameLayout
    private lateinit var clearText: TextView

    private var currentModeIndex = 0
    private val modes = arrayOf("MODE: AUTO SILENCE", "MODE: TAP & TRIM", "MODE: FIXED BPM")

    // פלטת צבעי סטודיו מודרנית
    private val colorIdle = Color.parseColor("#00E5FF")     // כחול סייבר
    private val colorRecord = Color.parseColor("#FF1744")   // אדום אזהרה
    private val colorPlay = Color.parseColor("#00E676")     // ירוק זוהר
    private val colorOverdub = Color.parseColor("#FF9100")  // כתום
    private val colorDarkBg = Color.parseColor("#050505")   // שחור פחם לחלל האפליקציה
    private val colorPanel = Color.parseColor("#141414")    // אפור עמוק לפאנלים

    private val mainHandler = Handler(Looper.getMainLooper())
    private val telemetryRunnable = object : Runnable {
        override fun run() {
            updateTelemetry()
            mainHandler.postDelayed(this, 16)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setupPolishedUI()

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.RECORD_AUDIO), 1)
        } else {
            bootEngine()
        }
    }

    private fun Number.dpToPx(): Int = (this.toFloat() * resources.displayMetrics.density).toInt()

    private fun createRoundedDrawable(bgColor: Int, radiusDp: Float, strokeColor: Int? = null, strokeWidthDp: Float = 2f): GradientDrawable {
        return GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = radiusDp.dpToPx().toFloat()
            setColor(bgColor)
            if (strokeColor != null) {
                setStroke(strokeWidthDp.dpToPx(), strokeColor)
            }
        }
    }

    private fun setupPolishedUI() {
        val rootLayout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setBackgroundColor(colorDarkBg)
            setPadding(24.dpToPx(), 48.dpToPx(), 24.dpToPx(), 48.dpToPx())
        }

        // בורר מצבים
        modeText = TextView(this).apply {
            text = modes[currentModeIndex]
            textSize = 13f
            typeface = Typeface.MONOSPACE
            letterSpacing = 0.1f
            setTextColor(colorIdle)
            background = createRoundedDrawable(colorPanel, 8f, colorIdle, 1f)
            setPadding(24.dpToPx(), 12.dpToPx(), 24.dpToPx(), 12.dpToPx())
            setOnClickListener { cycleDetectionMode() }
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply { setMargins(0, 0, 0, 48.dpToPx()) }
        }

        // צג סטטוס מרכזי
        statusText = TextView(this).apply {
            text = "BOOTING..."
            textSize = 26f
            typeface = Typeface.DEFAULT_BOLD
            letterSpacing = 0.1f
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply { setMargins(0, 0, 0, 4.dpToPx()) }
        }

        // צג BPM דיגיטלי
        bpmText = TextView(this).apply {
            text = "BPM: --"
            textSize = 18f
            typeface = Typeface.MONOSPACE
            setTextColor(Color.parseColor("#555555"))
            gravity = Gravity.CENTER
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply { setMargins(0, 0, 0, 48.dpToPx()) }
        }

        // מארז הפדאל
        val pedalContainer = FrameLayout(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                340.dpToPx(),
                340.dpToPx()
            ).apply { setMargins(0, 0, 0, 64.dpToPx()) }
        }

        ringView = RingProgressView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            )
        }

        // כפתור אקטיבי - שימוש ב-FrameLayout כדי לעקוף את המגבלות של אנדרואיד
        actionContainer = FrameLayout(this).apply {
            val btnSize = 220.dpToPx()
            layoutParams = FrameLayout.LayoutParams(btnSize, btnSize).apply {
                gravity = Gravity.CENTER
            }
            background = createRoundedDrawable(colorPanel, 200f) // עיגול פנימי
            setOnClickListener { handleActionClick() }
        }

        actionLabel = TextView(this).apply {
            text = "INIT"
            textSize = 20f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            )
        }

        actionContainer.addView(actionLabel)
        pedalContainer.addView(ringView)
        pedalContainer.addView(actionContainer)

        // כפתור איפוס מינימליסטי
        clearText = TextView(this).apply {
            text = "CLEAR MEMORY"
            textSize = 15f
            typeface = Typeface.DEFAULT_BOLD
            letterSpacing = 0.1f
            setTextColor(Color.parseColor("#FF5252"))
            background = createRoundedDrawable(Color.TRANSPARENT, 8f, Color.parseColor("#4DFF5252"), 1.5f)
            setPadding(32.dpToPx(), 16.dpToPx(), 32.dpToPx(), 16.dpToPx())
            setOnClickListener { audioEngine?.clearLoop() }
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
        }

        rootLayout.addView(modeText)
        rootLayout.addView(statusText)
        rootLayout.addView(bpmText)
        rootLayout.addView(pedalContainer)
        rootLayout.addView(clearText)

        setContentView(rootLayout)
    }

    private fun cycleDetectionMode() {
        currentModeIndex = (currentModeIndex + 1) % modes.size
        modeText.text = modes[currentModeIndex]
        audioEngine?.setDetectionMode(currentModeIndex)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 1 && grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            bootEngine()
        } else {
            statusText.text = "HARDWARE LOCKED"
            statusText.setTextColor(colorRecord)
        }
    }

    private fun bootEngine() {
        try {
            audioEngine = AudioEngine()
            audioEngine?.startEngine()
            audioEngine?.setDetectionMode(currentModeIndex)
            mainHandler.post(telemetryRunnable)
        } catch (e: Exception) {
            statusText.text = "KERNEL PANIC"
            statusText.setTextColor(colorRecord)
        }
    }

    private fun updateTelemetry() {
        audioEngine?.let { engine ->
            val state = engine.getCurrentState()
            val bpm = engine.getEstimatedBPM()

            ringView.progress = engine.getLoopPosition()
            statusText.text = state
            bpmText.text = if (bpm > 0f) "BPM: ${String.format("%.1f", bpm)}" else "BPM: --"

            when (state) {
                "LOOPING" -> {
                    actionContainer.isEnabled = true
                    actionLabel.text = "OVERDUB"
                    actionContainer.background = createRoundedDrawable(Color.parseColor("#092610"), 200f, colorPlay, 2f)
                    ringView.ringColor = colorPlay
                }
                "OVERDUBBING" -> {
                    actionContainer.isEnabled = true
                    actionLabel.text = "LOCK LOOP"
                    actionContainer.background = createRoundedDrawable(Color.parseColor("#331B00"), 200f, colorOverdub, 2f)
                    ringView.ringColor = colorOverdub
                }
                "RECORDING" -> {
                    actionContainer.isEnabled = false
                    actionLabel.text = "RECORDING"
                    actionContainer.background = createRoundedDrawable(Color.parseColor("#330505"), 200f, colorRecord, 2f)
                    ringView.ringColor = colorRecord
                }
                "PROCESSING" -> {
                    actionContainer.isEnabled = false
                    actionLabel.text = "QUANTIZING"
                    actionContainer.background = createRoundedDrawable(colorPanel, 200f)
                    ringView.ringColor = Color.WHITE
                }
                "CALIBRATING" -> {
                    actionContainer.isEnabled = false
                    actionLabel.text = "LEARNING..."
                    actionContainer.background = createRoundedDrawable(colorPanel, 200f)
                    ringView.ringColor = Color.parseColor("#333333")
                }
                else -> { // IDLE
                    if (currentModeIndex == 1) { // Tap & Trim
                        actionContainer.isEnabled = true
                        actionLabel.text = "TAP START"
                        actionContainer.background = createRoundedDrawable(Color.parseColor("#00252A"), 200f, colorIdle, 2f)
                        ringView.ringColor = colorIdle
                    } else { // Auto Silence / BPM
                        actionContainer.isEnabled = false
                        actionLabel.text = "PLAY TO\nSTART"
                        actionContainer.background = createRoundedDrawable(colorPanel, 200f)
                        ringView.ringColor = Color.parseColor("#333333")
                    }
                }
            }
        }
    }

    private fun handleActionClick() {
        audioEngine?.let { engine ->
            val state = engine.getCurrentState()
            if (state == "LOOPING") {
                engine.executeOverdub()
            } else if (state == "OVERDUBBING") {
                engine.executeLoop()
            } else if (currentModeIndex == 1 && state == "IDLE") {
                // מחכה למימוש ב-C++
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        mainHandler.removeCallbacks(telemetryRunnable)
        audioEngine?.stopEngine()
    }
}