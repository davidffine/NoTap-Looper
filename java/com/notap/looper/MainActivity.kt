package com.notap.looper

import android.Manifest
import android.animation.ValueAnimator
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.view.View
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.ViewGroup
import android.view.animation.AccelerateDecelerateInterpolator
import android.view.animation.OvershootInterpolator
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.viewModels
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import com.notap.looper.ui.state.LooperViewModel
import com.notap.looper.ui.views.CometRingView
import com.notap.looper.ui.views.RippleEmitterView
import com.notap.looper.ui.views.VuMeterView
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import kotlin.math.abs

class MainActivity : AppCompatActivity() {

    private val viewModel: LooperViewModel by viewModels()

    // UI Components
    private lateinit var modeText: TextView
    private lateinit var statusText: TextView
    private lateinit var bpmText: TextView
    private lateinit var btnBpmDown: TextView
    private lateinit var btnBpmUp: TextView
    private lateinit var cometRing: CometRingView
    private lateinit var rippleEmitter: RippleEmitterView
    private lateinit var stompLabel: TextView
    private lateinit var stompContainer: FrameLayout
    private lateinit var vuMeter: VuMeterView

    // רכיבי שכבת ההזנה החדשה (Overlay)
    private lateinit var bpmInputOverlay: FrameLayout
    private lateinit var bpmEditText: android.widget.EditText

    // Color Palette
    private val palette = object {
        val cyan   = Color.parseColor("#00E5FF")
        val red    = Color.parseColor("#FF1744")
        val green  = Color.parseColor("#00E676")
        val amber  = Color.parseColor("#FF9100")
        val void   = Color.parseColor("#000000")
        val idle   = Color.parseColor("#222222")
    }

    private var pulseAnimator: ValueAnimator? = null
    private var currentJitterX = 0f
    private var currentJitterY = 0f

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setupStudioUI()

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.RECORD_AUDIO), 1)
        } else {
            viewModel.bootEngine()
        }

        observeViewModel()
    }

    private fun observeViewModel() {
        lifecycleScope.launch {
            viewModel.uiState.collectLatest { state ->
                modeText.text = "${state.modeName}  ▾"

                if (state.modeIndex == 2) {
                    btnBpmDown.visibility = View.VISIBLE
                    btnBpmUp.visibility = View.VISIBLE
                    bpmText.text = "BPM: ${state.targetBpm.toInt()}"
                    bpmText.setTextColor(Color.parseColor("#64FFDA"))
                } else {
                    btnBpmDown.visibility = View.GONE
                    btnBpmUp.visibility = View.GONE
                    bpmText.text = if (state.bpm > 0f) "BPM: ${String.format("%.1f", state.bpm)}" else "BPM: --"
                    bpmText.setTextColor(Color.parseColor("#4CAF50"))
                }

                if (state.hasError) {
                    statusText.text = state.rawStateString
                    statusText.setTextColor(palette.red)
                    return@collectLatest
                }

                cometRing.progress = state.loopPosition
                updateVisualDynamics(state.rms, state.noiseStdDev, state.transientHit)
                transitionToState(state.engineState, state.rawStateString, state.modeIndex)
            }
        }
    }

    private fun transitionToState(state: EngineState, rawString: String, modeIndex: Int) {
        statusText.text = rawString

        val isActive = state in listOf(EngineState.RECORDING, EngineState.LOOPING, EngineState.OVERDUBBING)
        val uiAlpha = if (isActive) 0.1f else 1.0f
        modeText.alpha = uiAlpha
        bpmText.alpha = uiAlpha

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
            }
            EngineState.RECORDING -> {
                stompLabel.text = if (modeIndex == 1) "TAP STOP" else "REC"
                statusText.setTextColor(palette.red)
                statusText.setShadowLayer(25f, 0f, 0f, palette.red)
                cometRing.ringColor = palette.red
                startPulsing()
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
                if (modeIndex == 1) {
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
        val displayLevel = (rms * 6f)
        vuMeter.level = displayLevel

        val targetAlpha = 0.3f + displayLevel.coerceIn(0f, 0.7f)
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

    private fun Number.dpToPx(): Int = (this.toFloat() * resources.displayMetrics.density).toInt()

    private fun setupStudioUI() {
        val rootLayout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            clipChildren = false
            clipToPadding = false
            background = GradientDrawable().apply {
                shape = GradientDrawable.RECTANGLE
                colors = intArrayOf(Color.parseColor("#1A1D24"), Color.parseColor("#07080B"))
                gradientType = GradientDrawable.RADIAL_GRADIENT
                gradientRadius = 1400f
            }
            setPadding(0, 16.dpToPx(), 0, 0)
        }

        val topBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(16.dpToPx(), 0, 16.dpToPx(), 32.dpToPx())
            }
        }

        val btnOptions = createIconButton("⋮", "#FFFFFF") { showFileMenuDialog() }
        val spacer = View(this).apply { layoutParams = LinearLayout.LayoutParams(0, 1, 1f) }
        val btnTrash = createIconButton("🗑", "#FF1744") {
            viewModel.clearCurrentLoop()
            it.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
        }

        topBar.addView(btnOptions)
        topBar.addView(spacer)
        topBar.addView(btnTrash)

        val lcdContainer = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = GradientDrawable().apply {
                shape = GradientDrawable.RECTANGLE
                cornerRadius = 24f.dpToPx().toFloat()
                colors = intArrayOf(Color.parseColor("#12151C"), Color.parseColor("#0A0B0E"))
                setStroke(2.dpToPx(), Color.parseColor("#242936"))
            }
            layoutParams = LinearLayout.LayoutParams(340.dpToPx(), ViewGroup.LayoutParams.WRAP_CONTENT)
                .apply { setMargins(0, 0, 0, 40.dpToPx()) }
            setPadding(24.dpToPx(), 16.dpToPx(), 24.dpToPx(), 16.dpToPx())
            elevation = 30f
        }

        val textDataLayout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_VERTICAL or Gravity.START
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }

        modeText = TextView(this).apply {
            textSize = 11f
            typeface = Typeface.create("sans-serif-bold", Typeface.NORMAL)
            letterSpacing = 0.15f
            setTextColor(palette.cyan)
            isAllCaps = true
            background = GradientDrawable().apply {
                shape = GradientDrawable.RECTANGLE
                cornerRadius = 30f.dpToPx().toFloat()
                colors = intArrayOf(Color.parseColor("#1A00E5FF"), Color.parseColor("#0500E5FF"))
                setStroke(1.dpToPx(), Color.parseColor("#4D00E5FF"))
            }
            setPadding(16.dpToPx(), 6.dpToPx(), 16.dpToPx(), 6.dpToPx())
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, 0, 0, 12.dpToPx())
            }
            setOnClickListener { showModeSelectionDialog() }
            setOnTouchListener { v, event ->
                when (event.action) {
                    MotionEvent.ACTION_DOWN -> v.animate().scaleX(0.92f).scaleY(0.92f).alpha(0.8f).setDuration(50).start()
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        v.animate().scaleX(1f).scaleY(1f).alpha(1f).setDuration(150).start()
                        if (event.action == MotionEvent.ACTION_UP) v.performClick()
                    }
                }
                true
            }
        }

        statusText = TextView(this).apply {
            textSize = 32f
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            letterSpacing = 0.05f
            setTextColor(Color.WHITE)
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                .apply { setMargins(0, 8.dpToPx(), 0, 4.dpToPx()) }
        }

        val bpmContainer = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)

            // הפעלת שכבת ה-Overlay במקום קריאה לדיאלוג
            setOnClickListener {
                if (viewModel.uiState.value.modeIndex == 2) {
                    val currentBpm = viewModel.uiState.value.targetBpm.toInt().toString()
                    bpmEditText.setText(currentBpm)
                    bpmEditText.setSelection(bpmEditText.text.length)
                    bpmInputOverlay.visibility = View.VISIBLE

                    // מתן פוקוס והקפצת מקלדת לאחר עיכוב קל להבטחת רינדור השכבה
                    bpmEditText.postDelayed({
                        bpmEditText.requestFocus()
                        val imm = getSystemService(android.content.Context.INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager
                        imm.showSoftInput(bpmEditText, android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT)
                    }, 100)

                    it.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                }
            }
        }

        btnBpmDown = TextView(this).apply {
            text = "◀"
            textSize = 12f
            setTextColor(Color.parseColor("#4D4CAF50"))
            setPadding(0, 0, 16.dpToPx(), 0)
            visibility = View.GONE
            setOnTouchListener { v, event ->
                if (event.action == MotionEvent.ACTION_DOWN) {
                    v.animate().scaleX(0.8f).scaleY(0.8f).alpha(0.5f).setDuration(50).start()
                    viewModel.adjustTargetBpm(-1f)
                    v.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                } else if (event.action == MotionEvent.ACTION_UP || event.action == MotionEvent.ACTION_CANCEL) {
                    v.animate().scaleX(1f).scaleY(1f).alpha(1f).setDuration(100).start()
                }
                true
            }
        }

        bpmText = TextView(this).apply {
            textSize = 15f
            typeface = Typeface.MONOSPACE
            letterSpacing = 0.1f
            setTextColor(Color.parseColor("#4CAF50"))
        }

        btnBpmUp = TextView(this).apply {
            text = "▶"
            textSize = 12f
            setTextColor(Color.parseColor("#4D4CAF50"))
            setPadding(16.dpToPx(), 0, 0, 0)
            visibility = View.GONE
            setOnTouchListener { v, event ->
                if (event.action == MotionEvent.ACTION_DOWN) {
                    v.animate().scaleX(0.8f).scaleY(0.8f).alpha(0.5f).setDuration(50).start()
                    viewModel.adjustTargetBpm(1f)
                    v.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
                } else if (event.action == MotionEvent.ACTION_UP || event.action == MotionEvent.ACTION_CANCEL) {
                    v.animate().scaleX(1f).scaleY(1f).alpha(1f).setDuration(100).start()
                }
                true
            }
        }

        bpmContainer.addView(btnBpmDown)
        bpmContainer.addView(bpmText)
        bpmContainer.addView(btnBpmUp)

        textDataLayout.addView(modeText)
        textDataLayout.addView(statusText)
        textDataLayout.addView(bpmContainer)

        vuMeter = VuMeterView(this).apply { layoutParams = LinearLayout.LayoutParams(16.dpToPx(), 90.dpToPx()) }

        lcdContainer.addView(textDataLayout)
        lcdContainer.addView(vuMeter)

        val pedalContainer = FrameLayout(this).apply {
            clipChildren = false
            clipToPadding = false
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
        }

        rippleEmitter = RippleEmitterView(this).apply {
            layoutParams = FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        }

        cometRing = CometRingView(this).apply {
            layoutParams = FrameLayout.LayoutParams(380.dpToPx(), 380.dpToPx()).apply { gravity = Gravity.CENTER }
        }

        val stompSize = 200.dpToPx()
        stompContainer = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(stompSize, stompSize).apply { gravity = Gravity.CENTER }
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                colors = intArrayOf(Color.parseColor("#343846"), Color.parseColor("#15171C"))
                gradientType = GradientDrawable.RADIAL_GRADIENT
                gradientRadius = stompSize.toFloat()
                setStroke(3.dpToPx(), Color.parseColor("#0A0B0E"))
            }
            elevation = 60f
        }

        val stompInner = FrameLayout(this).apply {
            val innerSize = 170.dpToPx()
            layoutParams = FrameLayout.LayoutParams(innerSize, innerSize).apply { gravity = Gravity.CENTER }
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                colors = intArrayOf(Color.parseColor("#262933"), Color.parseColor("#1A1D24"))
                gradientType = GradientDrawable.RADIAL_GRADIENT
                gradientRadius = innerSize.toFloat()
                setStroke(1.dpToPx(), Color.parseColor("#3F4556"))
            }
        }

        stompLabel = TextView(this).apply {
            textSize = 20f
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            letterSpacing = 0.2f
            setTextColor(Color.parseColor("#E2E8F0"))
            gravity = Gravity.CENTER
            setShadowLayer(6f, 0f, 4f, Color.BLACK)
            layoutParams = FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        }

        stompInner.addView(stompLabel)
        stompContainer.addView(stompInner)

        pedalContainer.setOnTouchListener { _, event ->
            when (event.action) {
                MotionEvent.ACTION_DOWN -> {
                    pedalContainer.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                    stopPulsing()
                    stompContainer.animate().scaleX(0.90f).scaleY(0.90f).translationY(15f)
                        .setDuration(60).setInterpolator(OvershootInterpolator(1.0f)).start()
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    stompContainer.animate().scaleX(1f).scaleY(1f).translationY(0f)
                        .setDuration(180).setInterpolator(OvershootInterpolator(2.5f)).start()
                    if (event.action == MotionEvent.ACTION_UP) viewModel.handleActionClick()
                }
            }
            true
        }

        pedalContainer.addView(rippleEmitter)
        pedalContainer.addView(cometRing)
        pedalContainer.addView(stompContainer)

        rootLayout.addView(topBar)
        rootLayout.addView(lcdContainer)
        rootLayout.addView(pedalContainer)

        // --- בניית שכבת ההזנה האבסולוטית (In-Layout Overlay) ---
        bpmInputOverlay = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
            setBackgroundColor(Color.parseColor("#E6000000")) // חושך אבסולוטי (90% אטימות)
            visibility = View.GONE
            isClickable = true // מונע לחיצות שחודרות לסטודיו שמתחתיו

            val inputBox = LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER
                layoutParams = FrameLayout.LayoutParams(
                    (resources.displayMetrics.widthPixels * 0.85).toInt(),
                    ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply { gravity = Gravity.CENTER }

                setPadding(32.dpToPx(), 40.dpToPx(), 32.dpToPx(), 40.dpToPx())
                background = GradientDrawable().apply {
                    setColor(Color.parseColor("#0A0B0E"))
                    cornerRadius = 40f.dpToPx().toFloat()
                    setStroke(2.dpToPx(), Color.parseColor("#242936"))
                }

                addView(TextView(this@MainActivity).apply {
                    text = "ENTER BPM"
                    textSize = 14f
                    letterSpacing = 0.3f
                    setTextColor(Color.parseColor("#6A7A9C"))
                    typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
                    gravity = Gravity.CENTER
                })

                bpmEditText = android.widget.EditText(this@MainActivity).apply {
                    inputType = android.text.InputType.TYPE_CLASS_NUMBER
                    isSingleLine = true
                    setTextColor(Color.parseColor("#00E5FF"))
                    textSize = 56f
                    typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
                    gravity = Gravity.CENTER
                    background = null

                    layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                        setMargins(0, 16.dpToPx(), 0, 16.dpToPx())
                    }

                    setOnEditorActionListener { _, actionId, _ ->
                        if (actionId == android.view.inputmethod.EditorInfo.IME_ACTION_DONE || actionId == android.view.inputmethod.EditorInfo.IME_NULL) {
                            val input = text.toString().toFloatOrNull()
                            if (input != null) viewModel.setAbsoluteTargetBpm(input)

                            // סגירה הרמטית של המקלדת והשכבה
                            val imm = getSystemService(android.content.Context.INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager
                            imm.hideSoftInputFromWindow(windowToken, 0)
                            bpmInputOverlay.visibility = View.GONE
                            true
                        } else false
                    }
                }
                addView(bpmEditText)
            }
            addView(inputBox)
        }

        // --- הרכבת המרחב כולו למקשה אחת ---
        val masterFrame = FrameLayout(this)
        masterFrame.addView(rootLayout)
        masterFrame.addView(bpmInputOverlay)

        setContentView(masterFrame)
    }

    private fun createIconButton(icon: String, colorHex: String, onClick: (View) -> Unit): View {
        return TextView(this).apply {
            text = icon
            textSize = 22f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(colorHex))
            gravity = Gravity.CENTER
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                colors = intArrayOf(Color.parseColor("#1Affffff"), Color.parseColor("#05ffffff"))
                setStroke(1.dpToPx(), Color.parseColor("#33ffffff"))
            }
            layoutParams = LinearLayout.LayoutParams(48.dpToPx(), 48.dpToPx())

            setOnTouchListener { v, event ->
                when (event.action) {
                    MotionEvent.ACTION_DOWN -> v.animate().scaleX(0.85f).scaleY(0.85f).alpha(0.7f).setDuration(50).start()
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        v.animate().scaleX(1f).scaleY(1f).alpha(1f).setDuration(150).start()
                        if (event.action == MotionEvent.ACTION_UP) onClick(v)
                    }
                }
                true
            }
        }
    }

    private fun showFileMenuDialog() {
        val dialog = android.app.AlertDialog.Builder(this).create()
        dialog.window?.setBackgroundDrawable(android.graphics.drawable.ColorDrawable(Color.TRANSPARENT))

        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(40.dpToPx(), 40.dpToPx(), 40.dpToPx(), 40.dpToPx())
            background = GradientDrawable().apply {
                setColor(Color.parseColor("#12151C"))
                cornerRadius = 32f.dpToPx().toFloat()
                setStroke(2.dpToPx(), Color.parseColor("#242936"))
            }
        }

        val title = TextView(this).apply {
            text = "FILE SYSTEM"
            textSize = 14f
            letterSpacing = 0.2f
            setTextColor(Color.parseColor("#8892B0"))
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            setPadding(0, 0, 0, 32.dpToPx())
            gravity = Gravity.CENTER
        }
        layout.addView(title)

        val btnSave = createMenuAction("💾  EXPORT TO WAV", "#64FFDA") {
            dialog.dismiss()
        }

        val btnLoad = createMenuAction("📂  IMPORT FROM WAV", "#FFD600") {
            dialog.dismiss()
        }

        layout.addView(btnSave)
        layout.addView(btnLoad)

        dialog.setView(layout)
        dialog.show()
    }

    private fun createMenuAction(label: String, colorHex: String, onClick: () -> Unit): View {
        return TextView(this).apply {
            text = label
            textSize = 14f
            typeface = Typeface.create("sans-serif-bold", Typeface.NORMAL)
            setTextColor(Color.parseColor(colorHex))
            setPadding(24.dpToPx(), 20.dpToPx(), 24.dpToPx(), 20.dpToPx())
            background = GradientDrawable().apply {
                shape = GradientDrawable.RECTANGLE
                cornerRadius = 16f.dpToPx().toFloat()
                setColor(Color.parseColor("#1A1D24"))
                setStroke(1.dpToPx(), Color.parseColor("#242936"))
            }
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                setMargins(0, 0, 0, 16.dpToPx())
            }

            setOnTouchListener { v, event ->
                when (event.action) {
                    MotionEvent.ACTION_DOWN -> v.animate().scaleX(0.96f).scaleY(0.96f).setDuration(50).start()
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        v.animate().scaleX(1f).scaleY(1f).setDuration(150).start()
                        if (event.action == MotionEvent.ACTION_UP) onClick()
                    }
                }
                true
            }
        }
    }

    private fun showModeSelectionDialog() {
        val dialog = android.app.AlertDialog.Builder(this).create()
        dialog.window?.setBackgroundDrawable(android.graphics.drawable.ColorDrawable(Color.TRANSPARENT))

        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(40.dpToPx(), 40.dpToPx(), 40.dpToPx(), 40.dpToPx())
            background = GradientDrawable().apply {
                setColor(Color.parseColor("#12151C"))
                cornerRadius = 32f.dpToPx().toFloat()
                setStroke(2.dpToPx(), Color.parseColor("#242936"))
            }
        }

        val title = TextView(this).apply {
            text = "DETECTION ONTOLOGY"
            textSize = 18f
            letterSpacing = 0.1f
            setTextColor(Color.WHITE)
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            setPadding(0, 0, 0, 32.dpToPx())
            gravity = Gravity.CENTER
        }
        layout.addView(title)

        val modes = listOf(
            Triple(0, "AUTO SILENCE", "System analyzes acoustic energy and automatically stops upon volume decay.\n\nPros: Absolute physical freedom.\nCons: Vulnerable to background noise, prevents intentional pauses within a riff."),
            Triple(1, "TAP & TRIM", "Manual boundary marking. Tap pedal to start, tap to stop.\n\nPros: Mathematical precision and full control over loop length.\nCons: Requires physical action, potentially breaking focus."),
            Triple(2, "FIXED BPM", "Time is predetermined. System waits for an exact beat count.\n\nPros: Hermetic synchronization with metronomes or external hardware.\nCons: Zero organic tempo flexibility.")
        )

        modes.forEach { (index, name, desc) ->
            val isSelected = viewModel.uiState.value.modeIndex == index

            val btn = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(24.dpToPx(), 24.dpToPx(), 24.dpToPx(), 24.dpToPx())
                background = GradientDrawable().apply {
                    shape = GradientDrawable.RECTANGLE
                    cornerRadius = 16f.dpToPx().toFloat()
                    setColor(if (isSelected) Color.parseColor("#1A00E5FF") else Color.parseColor("#1A1D24"))
                    setStroke(2.dpToPx(), if (isSelected) palette.cyan else Color.TRANSPARENT)
                }
                layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                    setMargins(0, 0, 0, 16.dpToPx())
                }

                setOnTouchListener { v, event ->
                    when (event.action) {
                        MotionEvent.ACTION_DOWN -> v.animate().scaleX(0.96f).scaleY(0.96f).setDuration(50).start()
                        MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                            v.animate().scaleX(1f).scaleY(1f).setDuration(150).start()
                            if (event.action == MotionEvent.ACTION_UP) {
                                viewModel.setDetectionModeDirectly(index)
                                dialog.dismiss()
                            }
                        }
                    }
                    true
                }
            }

            val nameTxt = TextView(this@MainActivity).apply {
                text = name
                textSize = 15f
                setTextColor(if (isSelected) palette.cyan else Color.WHITE)
                typeface = Typeface.create("sans-serif-bold", Typeface.NORMAL)
            }
            val descTxt = TextView(this@MainActivity).apply {
                text = desc
                textSize = 13f
                setLineSpacing(4f, 1f)
                setTextColor(if (isSelected) Color.parseColor("#B0C4DE") else Color.parseColor("#8892B0"))
                setPadding(0, 8.dpToPx(), 0, 0)
            }

            btn.addView(nameTxt)
            btn.addView(descTxt)
            layout.addView(btn)
        }

        dialog.setView(layout)
        dialog.show()
    }

    override fun onDestroy() {
        super.onDestroy()
        pulseAnimator?.cancel()
    }
}