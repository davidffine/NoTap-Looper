package com.notap.looper

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.RectF
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Bundle
import android.text.InputType
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.animation.OvershootInterpolator
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.lifecycle.lifecycleScope
import com.notap.looper.ui.Design
import com.notap.looper.ui.state.LooperViewModel
import com.notap.looper.ui.state.OnboardingStage
import com.notap.looper.ui.state.PremiumPlans
import com.notap.looper.ui.views.IconMorphButton
import com.notap.looper.ui.views.LevelMeterView
import com.notap.looper.ui.views.LoopVisualizerView
import com.notap.looper.ui.views.SegmentedControlView
import com.notap.looper.ui.views.SliderView
import com.notap.looper.ui.views.SpotlightView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

class MainActivity : AppCompatActivity() {

    private companion object {
        // Metronome toggle glyphs — one source so the icon and its muted twin can't
        // drift apart between construction and the state binding.
        const val METRO_ICON_ON = "🔊"
        const val METRO_ICON_OFF = "🔇"
    }

    private val viewModel: LooperViewModel by viewModels()

    private lateinit var visualizer: LoopVisualizerView
    private lateinit var orbTitle: TextView
    private lateinit var orbSub: TextView
    private lateinit var stateDot: TextView
    private lateinit var stateName: TextView
    private lateinit var bpmReadout: TextView
    private lateinit var levelMeter: LevelMeterView
    private lateinit var segmented: SegmentedControlView
    private lateinit var bpmStepper: LinearLayout
    private lateinit var bpmStepValue: TextView
    private lateinit var metroToggle: TextView
    private var lastMetroOn: Boolean? = null
    private var lastBpmShown = -1
    // (fxRow / FLIP / OCT± removed from the main dashboard — per-layer FX now lives
    //  ONLY in the LAYERS sheet, where each layer owns its own strip.)
    private lateinit var reverbRow: LinearLayout
    private lateinit var reverbSlider: SliderView
    private lateinit var reverbWetLabel: TextView
    private lateinit var heroContainer: FrameLayout
    private lateinit var heroLabels: LinearLayout      // orb title+sub — morphs WITH the circle
    // Hero morph: switching to/from SYNC shows/hides the BPM stepper, which resizes
    // the hero band and made the orb SNAP to a new radius. This animator carries the
    // circle continuously between the two sizes (shared-element / Flutter-Hero feel).
    private var heroMorphAnim: android.animation.ValueAnimator? = null
    private var lastModeIndex: Int? = null
    private var heroMorphArmed = false          // one morph per layout pass, not one per trigger
    private var heroCelebratePending = false    // loop just born → morph settles with an overshoot
    private var lastEngineStateForAnim: EngineState? = null
    private var layersRenderedCount = 0         // detects "a layer appeared" for the row-in animation

    private lateinit var bpmOverlay: FrameLayout
    private lateinit var bpmEditText: EditText
    private lateinit var ioButton: IconMorphButton

    // Onboarding / freemium chrome
    private lateinit var bottomCol: LinearLayout
    private lateinit var coachCard: LinearLayout          // floating glass coach pill (over the UI)
    private lateinit var onboardingTitle: TextView
    private lateinit var onboardingSub: TextView
    private lateinit var coachTrailing: TextView          // one trailing control: ✕ skip / → continue / ✓ finish
    private var coachTrailingContinues = false            // true → tapping advances; false → skips
    private lateinit var spotlightView: SpotlightView
    private lateinit var masterFrame: FrameLayout
    private var coachPulse: android.animation.ValueAnimator? = null
    private var coachPulseTarget: View? = null
    private lateinit var paywallOverlay: FrameLayout
    private lateinit var paywallFeatureLabel: TextView
    private lateinit var reverbProBadge: TextView

    // Phase 4 chrome
    private lateinit var contextButton: TextView          // top-left: 💡 help ⇄ ✕ clear
    private var contextClearMode = false                  // current function of contextButton
    private var lastContextClearMode: Boolean? = null
    // Exit guard: BACK first closes any open overlay; otherwise it arms a 2s
    // "press back again to exit" window — no more silent app-exit (which tears
    // down the engine). The autosave in onStop is the second net under this.
    private var backArmedAtMs = 0L
    // Session-restore prompt shown at most once per boot (edge-detected).
    private var lastShowRestore = false
    // KEEP_SCREEN_ON mirror — screen timeout fires onStop, which closes the audio
    // streams: a hands-free looper MUST hold the screen while the engine works.
    private var lastKeepScreenOn: Boolean? = null
    private lateinit var feedbackSheet: FrameLayout
    private lateinit var feedbackPanel: LinearLayout
    private lateinit var premiumPanel: LinearLayout
    private lateinit var bpmSlider: SliderView

    // Multi-track layers: a pill that appears while looping with ≥1 overdub, opening
    // a sheet to delete any individual overdub (the base take is protected).
    private lateinit var layersRow: LinearLayout
    private lateinit var layersButton: TextView
    private lateinit var layersSheet: FrameLayout
    private lateinit var layersPanel: LinearLayout
    private lateinit var layersList: LinearLayout
    private var lastLayerCount = -1
    // CLEAN (spectral noise gate, Pro): shares the row with LAYERS; lit when every
    // layer is denoised. Crown badge shown to non-Pro users like the other gates.
    private lateinit var cleanButton: TextView
    private lateinit var cleanContainer: FrameLayout   // crown wrapper — visibility target
    private lateinit var cleanBadge: TextView
    private var lastCleanOn: Boolean? = null
    private var layersBtnAnim: android.animation.ValueAnimator? = null
    // Fade state for every height-changing bottom row. Lazy so they bind after
    // buildBottom() has created the views; see setRowShown for the ordering rules.
    private val bpmFader by lazy { RowFader(bpmStepper) }
    private val reverbFader by lazy { RowFader(reverbRow) }
    private val layersFader by lazy { RowFader(layersRow) }

    private var currentAccent = Design.cyan
    private var hasLoop = false
    private var lastRecElapsed = -1   // take-timer rebind guard (-1 = not recording)
    private var lastOnboardingActive: Boolean? = null
    private var lastStage: OnboardingStage? = null
    private var lastShowPaywall = false
    private var lastIsPro: Boolean? = null
    // Mirrors of ViewModel gating state, so the reverb slider's onChange (a
    // closure) can keep its label honest without re-reading uiState each frame.
    private var isProNow = false
    private var reverbTasteWindow = false
    private val freeReverbMax = 0.30f

    // OpenDocument (SAF browser) lists files by their document MIME, so it shows
    // our exported WAVs regardless of MediaStore audio-indexing. octet-stream is
    // included because some providers report .wav that way.
    private val importLauncher =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            uri?.let { handleImport(it) }
        }
    private val exportLauncher =
        registerForActivityResult(ActivityResultContracts.CreateDocument("audio/wav")) { uri ->
            uri?.let { handleExport(it) }
        }

    private var audioPermissionGranted = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.getInsetsController(window, window.decorView).isAppearanceLightStatusBars = false
        setContentView(buildUi())

        // BACK: overlays close first (standard Android); an actual exit needs a
        // second press within 2s — leaving tears the engine down, so it must
        // never happen from one stray tap.
        onBackPressedDispatcher.addCallback(this, object : androidx.activity.OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = handleBack()
        })

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED
        ) {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.RECORD_AUDIO), 1)
        } else {
            audioPermissionGranted = true
            // Actual boot happens in onStart (fires right after onCreate). The
            // audio session is now owned by the foreground lifecycle, not onCreate.
        }
        observe()
    }

    // Streams follow the foreground. onStart → open (or first-boot); onStop →
    // synchronously release the HAL. This is the fix for the swipe-kill routing
    // glitch: the graceful close runs in onStop, guaranteed before process death.
    override fun onStart() {
        super.onStart()
        if (audioPermissionGranted) viewModel.onForeground()
    }

    override fun onStop() {
        super.onStop()
        if (audioPermissionGranted) viewModel.onBackground()
    }

    /** BACK routing: dismiss the topmost open overlay; on a bare screen, require a
     *  second press within 2s to actually exit ("press back again to exit"). */
    private fun handleBack() {
        when {
            bpmOverlay.visibility == View.VISIBLE -> closeBpmOverlay()
            paywallOverlay.visibility == View.VISIBLE -> viewModel.dismissPaywall()
            layersSheet.visibility == View.VISIBLE -> hideLayersSheet()
            feedbackSheet.visibility == View.VISIBLE -> hideFeedbackSheet()
            else -> {
                val now = android.os.SystemClock.uptimeMillis()
                if (now - backArmedAtMs > 2000L) {
                    backArmedAtMs = now
                    toast("Press back again to exit")
                } else {
                    finish()
                }
            }
        }
    }

    override fun onRequestPermissionsResult(rc: Int, perms: Array<out String>, res: IntArray) {
        super.onRequestPermissionsResult(rc, perms, res)
        if (res.isNotEmpty() && res[0] == PackageManager.PERMISSION_GRANTED) {
            audioPermissionGranted = true
            viewModel.onForeground()   // onStart already ran (pre-grant); boot now
        }
    }

    // ---------------------------------------------------------------------
    //  Reactive binding
    // ---------------------------------------------------------------------
    private fun observe() {
        lifecycleScope.launch {
            viewModel.waveform.collectLatest { w -> visualizer.setWaveform(w) }
        }
        lifecycleScope.launch {
            viewModel.uiState.collectLatest { s ->
                val look = lookFor(s.engineState, s.modeIndex)

                if (look.accent != currentAccent) {
                    currentAccent = look.accent
                    visualizer.setAccent(look.accent)
                    segmented.accent = look.accent
                    stateDot.setTextColor(look.accent)
                    stateName.setTextColor(look.accent)
                    orbTitle.setTextColor(look.accent)
                    // Neon glow in the state colour — makes the state word pop on
                    // video. Set only on transition (a few times/session), never
                    // per frame; cost is a static shadow layer on two TextViews.
                    stateDot.setShadowLayer(dp(6).toFloat(), 0f, 0f, look.accent)
                    stateName.setShadowLayer(dp(8).toFloat(), 0f, 0f, look.accent)
                    orbTitle.setShadowLayer(dp(18).toFloat(), 0f, 0f, look.accent)
                }

                stateName.text = look.stateLabel
                orbTitle.text = look.title
                orbSub.text = look.sub
                visualizer.intensity = look.intensity

                // Hero morph trigger — MUST run before the BPM stepper's visibility
                // flips below, so it captures the orb's radius while the old layout
                // is still in place. Only SYNC (mode 2) changes the hero's height,
                // but EVERY mode change gets a ring sweep: the instrument's character
                // changed, and the orb is where that identity lives.
                if (lastModeIndex != s.modeIndex) {
                    val prevMode = lastModeIndex
                    lastModeIndex = s.modeIndex
                    // Ring sweep on any mode change. The hero morph is NOT armed here:
                    // the BPM card's exit is delayed by its fade, so the morph must be
                    // armed at the instant the layout actually changes — see
                    // setRowShown, which owns that timing for every row.
                    if (prevMode != null) visualizer.fireRipple()
                }
                // Loop-is-born edge (PROCESSING → LOOPING): the app's magic moment.
                // Flagged here, consumed by the morph the FX-row reveal arms below.
                if (lastEngineStateForAnim != s.engineState) {
                    if (lastEngineStateForAnim == EngineState.PROCESSING &&
                        s.engineState == EngineState.LOOPING) heroCelebratePending = true
                    lastEngineStateForAnim = s.engineState
                }

                // Tempo slot: while RECORDING it carries the live take timer — there's
                // no tempo to show yet, and this number IS the future loop's length
                // (a runaway count also exposes a non-closing AUTO take early).
                // Otherwise: measured vs target BPM as before.
                if (s.recordingElapsedSec < 0 && lastRecElapsed != -1) {
                    lastRecElapsed = -1; lastBpmShown = -1   // left timer mode — re-arm the BPM readout
                }
                if (s.recordingElapsedSec >= 0) {
                    if (lastRecElapsed != s.recordingElapsedSec) {
                        lastRecElapsed = s.recordingElapsedSec
                        bpmReadout.text =
                            "⏺ ${s.recordingElapsedSec / 60}:${"%02d".format(s.recordingElapsedSec % 60)}"
                        bpmReadout.setTextColor(Design.red)
                    }
                } else if (s.modeIndex == 2) {
                    // Guard on the integer BPM so the readout, stepper value and
                    // slider only re-bind when the tempo actually changes (never
                    // 60×/s). setValueSilently keeps the slider off its onChange.
                    val bpmInt = s.targetBpm.toInt()
                    if (bpmInt != lastBpmShown) {
                        lastBpmShown = bpmInt
                        bpmReadout.text = "♩ $bpmInt"
                        bpmStepValue.text = "$bpmInt"
                        bpmSlider.setValueSilently(sliderFromBpm(s.targetBpm))
                    }
                    bpmReadout.setTextColor(Design.violet)
                    setRowShown(bpmFader, true)
                    if (lastMetroOn != s.metronomeEnabled) {
                        lastMetroOn = s.metronomeEnabled
                        val on = s.metronomeEnabled
                        // Speaker ⇄ muted-speaker. The glyph carries the state; the
                        // pill tint and a dimmed alpha reinforce it (emoji ignore
                        // setTextColor, so tinting alone would signal nothing).
                        metroToggle.text = if (on) METRO_ICON_ON else METRO_ICON_OFF
                        metroToggle.alpha = if (on) 1f else 0.5f
                        metroToggle.background = Design.pill(
                            this@MainActivity,
                            if (on) Design.alpha(Design.green, 0.14f) else Design.surfaceHi,
                            if (on) Design.green else Design.stroke
                        )
                    }
                } else {
                    lastBpmShown = -1   // re-arm the SYNC guard for next entry
                    bpmReadout.text = if (s.bpm > 0f) "♩ ${s.bpm.toInt()}" else "♩ – –"
                    bpmReadout.setTextColor(Design.textMid)
                    setRowShown(bpmFader, false)
                }

                if (segmented.selected != s.modeIndex) segmented.setSelectedSilently(s.modeIndex)

                hasLoop = s.engineState == EngineState.LOOPING || s.engineState == EngineState.OVERDUBBING
                ioButton.setDownload(hasLoop)
                // Context button ✕ = "cancel/clear" — armed whenever there's audio in
                // flight or stored, i.e. any state that isn't the idle/calibrating rest
                // position. This now includes RECORDING (tap ✕ to abort the take) and
                // PROCESSING (kept armed so ✕ stays continuous through REC→LOOP with no
                // flicker back to 💡). IDLE/CALIBRATING/UNKNOWN → 💡 replay-tutorial.
                val cancelMode = when (s.engineState) {
                    EngineState.RECORDING, EngineState.PROCESSING,
                    EngineState.LOOPING, EngineState.OVERDUBBING -> true
                    else -> false
                }
                // Hold the screen while the engine is working: screen timeout fires
                // onStop → the audio streams close → THE LOOP GOES SILENT mid-
                // performance. A hands-free instrument can't allow that. Released
                // again at rest (IDLE/CALIBRATING) so idle battery is unaffected.
                if (lastKeepScreenOn != cancelMode) {
                    lastKeepScreenOn = cancelMode
                    if (cancelMode)
                        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                    else
                        window.clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                }
                // Session-restore prompt (once per boot, on the flag's rising edge).
                if (s.showRestorePrompt && !lastShowRestore) showRestoreDialog()
                lastShowRestore = s.showRestorePrompt
                // Morphs 💡⇄✕ on the edge (first bind is silent — the button is built
                // already showing 💡).
                if (lastContextClearMode != cancelMode) {
                    if (lastContextClearMode == null && !cancelMode) {
                        lastContextClearMode = false
                    } else {
                        lastContextClearMode = cancelMode
                        morphContextButton(cancelMode)
                    }
                }
                // Hide the top-left context button during onboarding: clearing is
                // disabled (it would derail the funnel) and the pill already carries
                // SKIP, so a live-looking ✕ here would be a dead click.
                val ctxTarget = if (s.onboardingActive) View.GONE else View.VISIBLE
                if (contextButton.visibility != ctxTarget) contextButton.visibility = ctxTarget
                visualizer.beatCount = if (hasLoop) Math.round(s.loopBeats) else 0
                // Controls are hidden during the "magic" phase, revealed from LAYER
                // onward (feature discovery) and of course after onboarding.
                val controlsRevealed = !s.onboardingActive || s.onboardingStage >= OnboardingStage.LAYER
                // Progressive disclosure: during onboarding the reverb row stays
                // hidden through LISTEN_ROOM/FIRST_LOOP/LAYER and reveals ONLY at
                // ELEVATE — the scoped TransitionManager gives that a satisfying
                // reveal. Outside onboarding: shown whenever a loop exists.
                // (FLIP/OCT± no longer live here — per-layer FX is the LAYERS sheet's job.)
                val fxRevealed = if (s.onboardingActive) s.onboardingStage == OnboardingStage.ELEVATE else true
                val fxTarget = if (hasLoop && fxRevealed) View.VISIBLE else View.GONE
                // Control rows fluidly fade/slide via a transition scoped to
                // bottomCol ONLY (never a scene root containing the hero).
                setRowShown(reverbFader, fxTarget == View.VISIBLE)
                // Multi-track row: LAYERS appears in the settled LOOPING state once at
                // least one overdub exists (layerCount ≥ 2). Gated to LOOPING (not
                // mid-overdub) so a delete never queues behind a take. Hidden during
                // onboarding EXCEPT the LAYERS_INTRO step, which spotlights it.
                // CLEAN appears for ANY loop (base included — one layer still carries
                // room noise) but never during the tutorial. Row shows if either does.
                val layersRevealed = !s.onboardingActive || s.onboardingStage == OnboardingStage.LAYERS_INTRO
                val looping = s.engineState == EngineState.LOOPING
                val layersBtnTarget =
                    if (layersRevealed && looping && s.layerCount >= 2) View.VISIBLE else View.GONE
                val cleanTarget =
                    if (!s.onboardingActive && looping && s.layerCount >= 1) View.VISIBLE else View.GONE
                val rowTarget =
                    if (layersBtnTarget == View.VISIBLE || cleanTarget == View.VISIBLE) View.VISIBLE else View.GONE
                val layersRowShown = rowTarget == View.VISIBLE
                if (layersFader.intent != layersRowShown) {
                    // The whole row is appearing/disappearing. On the way IN, put the
                    // children in their final state first so the row fades in already
                    // correct; on the way OUT leave them alone — re-laying out children
                    // mid-fade (CLEAN widening as LAYERS vanishes) reads as a glitch.
                    if (layersRowShown) {
                        layersBtnAnim?.cancel()
                        resetLayersButtonParams(layersBtnTarget == View.VISIBLE)
                        cleanContainer.visibility = cleanTarget
                    }
                    setRowShown(layersFader, layersRowShown)
                } else if (layersRowShown) {
                    // Row stays put; only its contents change (e.g. the last overdub was
                    // deleted → LAYERS goes away and CLEAN must take the freed width).
                    if (layersButton.visibility != layersBtnTarget)
                        animateLayersButton(layersBtnTarget == View.VISIBLE)
                    if (cleanContainer.visibility != cleanTarget)
                        cleanContainer.visibility = cleanTarget
                }
                // CLEAN lit state: filled green when every layer is denoised.
                if (lastCleanOn != s.cleanAllOn) {
                    lastCleanOn = s.cleanAllOn
                    if (s.cleanAllOn) {
                        cleanButton.setTextColor(Color.BLACK)
                        cleanButton.background = Design.pill(this@MainActivity, Design.green, Design.green)
                    } else {
                        cleanButton.setTextColor(Design.green)
                        cleanButton.background = Design.glass(this@MainActivity, 14f)
                    }
                }
                if (lastLayerCount != s.layerCount) {
                    lastLayerCount = s.layerCount
                    // Pill shows the overdub count (layers beyond the base).
                    if (s.layerCount >= 2) layersButton.text = "🎚  LAYERS · ${s.layerCount - 1}"
                    // Keep an open sheet honest: repopulate on any count change; if the
                    // last overdub is gone, close it (nothing left to manage).
                    if (layersSheet.visibility == View.VISIBLE) {
                        if (s.layerCount >= 2) populateLayersList(s.layerCount) else hideLayersSheet()
                    }
                    // Re-seed the main FX dashboard to the new current (newest) layer —
                    // a fresh overdub reads back fx=none / reverb=0, so the dashboard
                    // resets; a delete re-points it at whatever the new newest layer holds.
                    reseedMainFxDashboard()
                }
                val bottomTarget = if (controlsRevealed) View.VISIBLE else View.INVISIBLE
                if (bottomCol.visibility != bottomTarget) bottomCol.visibility = bottomTarget

                // --- onboarding: floating pill (fade), spotlight, anchor, trailing.
                //     Hero keeps full weight — pill floats, never reflows layout. ---
                if (lastOnboardingActive != s.onboardingActive) {
                    lastOnboardingActive = s.onboardingActive
                    lastStage = null
                    // Freeze + dim the mode switcher during the tutorial (the
                    // ViewModel also blocks the switch); restore on completion.
                    segmented.alpha = if (s.onboardingActive) 0.45f else 1f
                    if (s.onboardingActive) {
                        coachCard.visibility = View.VISIBLE
                        coachCard.alpha = 0f
                        coachCard.animate().alpha(1f).setDuration(220).start()
                    } else {
                        // Tutorial over. Do NOT reset the reverb slider here — the
                        // ELEVATE drag-completion keeps the dialed value playing, and
                        // it's already 0 on every skip path (reverb row is hidden
                        // until ELEVATE), so a reset would only wrongly zero a live value.
                        coachCard.animate().alpha(0f).setDuration(180)
                            .withEndAction { coachCard.visibility = View.GONE }.start()
                        spotlightView.hide(); spotlightView.visibility = View.GONE
                        stopCoachPulse()
                    }
                }
                if (s.onboardingActive && lastStage != s.onboardingStage) {
                    lastStage = s.onboardingStage
                    bindOnboardingStage(s.onboardingStage)
                    updateSpotlight(s.onboardingStage)
                    updateCoachTrailing(s.onboardingStage)
                    anchorCoachPill(s.onboardingStage)
                }

                // --- paywall ---
                if (lastShowPaywall != s.showPaywall) {
                    lastShowPaywall = s.showPaywall
                    if (s.showPaywall) {
                        paywallFeatureLabel.text = "${s.paywallFeature} is a Pro feature — here's everything it unlocks:"
                        paywallOverlay.alpha = 0f
                        paywallOverlay.visibility = View.VISIBLE
                        paywallOverlay.animate().alpha(1f).setDuration(180).start()
                        premiumPanel.translationY = dp(600).toFloat()
                        premiumPanel.animate().translationY(0f).setDuration(320)
                            .setInterpolator(OvershootInterpolator(1.05f)).start()
                    } else {
                        premiumPanel.animate().translationY(dp(600).toFloat()).setDuration(220).start()
                        paywallOverlay.animate().alpha(0f).setDuration(220)
                            .withEndAction { paywallOverlay.visibility = View.GONE }.start()
                        // Cancelled the reverb upsell → smoothly settle the fader back
                        // to the free limit (not dry). If they upgraded, isPro is now
                        // true and we leave the fader where they dragged it.
                        if (!s.isPro && s.paywallFeature.contains("REVERB")) {
                            reverbSlider.animateTo(freeReverbMax)
                            reverbWetLabel.text = "${Math.round(freeReverbMax * 100)}%"
                        }
                    }
                }

                // --- pro affordances: crown badges on every gated control ---
                reverbTasteWindow = s.onboardingActive && s.onboardingStage == OnboardingStage.ELEVATE
                if (lastIsPro != s.isPro) {
                    lastIsPro = s.isPro
                    isProNow = s.isPro
                    val badgeVis = if (s.isPro) View.GONE else View.VISIBLE
                    reverbProBadge.visibility = badgeVis
                    cleanBadge.visibility = badgeVis
                    // The free-tier line only exists for non-Pro users.
                    reverbSlider.freeMarker = if (s.isPro) -1f else freeReverbMax
                }

                if (s.hasError) {
                    stateName.text = "ENGINE ERROR"
                    stateName.setTextColor(Design.red)
                    orbTitle.text = "ERR"
                    return@collectLatest
                }

                visualizer.progress = s.loopPosition
                visualizer.level = (s.rms * 6f).coerceIn(0f, 1f)
                levelMeter.level = (s.rms * 6f).coerceIn(0f, 1f)
                if (s.transientHit) visualizer.fireRipple()
            }
        }
    }

    private data class Look(
        val accent: Int, val stateLabel: String, val title: String,
        val sub: String, val intensity: Float
    )

    /** Mode is a FIRST-CLASS input here, not just a copy switch: at rest the orb
     *  carries the MODE's identity colour — the same three tokens the segmented control
     *  and the mode sheet already use (AUTO cyan · TAP amber · SYNC violet). So
     *  switching modes while idle cross-fades the whole hero, which is what makes
     *  the mode feel like a property of the instrument rather than a hidden
     *  setting. The instant the engine is ACTIVE (rec/proc/loop/overdub) colour
     *  reverts to STATE — urgency outranks identity, and those colours are the
     *  ones a player reads from across a room. Because the accent still flows
     *  through this single function, the binding remains the one source of truth
     *  and no colour can ever get "stuck" out of sync with the engine. */
    private fun lookFor(state: EngineState, mode: Int): Look = when (state) {
        EngineState.CALIBRATING -> Look(Design.slate, "CALIBRATING", "WAIT",
            "Learning the room's noise floor…", 0.1f)
        EngineState.IDLE, EngineState.UNKNOWN -> when (mode) {
            1 -> Look(Design.amber, "READY", "START", "Tap the orb to begin recording", 0.12f)
            2 -> Look(Design.violet, "READY", "LISTEN", "Pluck to start — I lock to your tempo", 0.12f)
            else -> Look(Design.cyan, "READY", "LISTEN", "Pluck a string to start the loop", 0.12f)
        }
        EngineState.RECORDING ->
            if (mode == 1) Look(Design.red, "RECORDING", "STOP", "Tap the orb to end the take", 1f)
            else Look(Design.red, "RECORDING", "REC", "Play your phrase — I stop when you do", 1f)
        EngineState.PROCESSING -> Look(Design.violet, "SYNCING", "SYNC", "Finding the beat & trimming…", 0.5f)
        EngineState.LOOPING -> Look(Design.green, "LOOPING", "OVERDUB", "Tap the orb to layer a new pass", 0.3f)
        EngineState.OVERDUBBING -> Look(Design.amber, "OVERDUBBING", "LOCK", "Tap the orb to finish the layer", 0.7f)
    }

    // ---------------------------------------------------------------------
    //  UI construction
    // ---------------------------------------------------------------------
    private fun buildUi(): View {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            clipChildren = false; clipToPadding = false
            background = GradientDrawable().apply {
                gradientType = GradientDrawable.RADIAL_GRADIENT
                colors = intArrayOf(Design.bgTop, Design.bgBottom)
                gradientRadius = 1500f
                setGradientCenter(0.5f, 0.42f)
            }
            setPadding(dp(20), dp(28), dp(20), dp(16))
        }

        root.addView(buildTopBar())
        root.addView(buildTelemetryStrip())
        root.addView(buildHero())        // full weight — no longer squeezed by the coach
        root.addView(buildBottom())

        val master = FrameLayout(this)
        masterFrame = master
        master.addView(root)
        // z-order: chrome (root) < spotlight (dims chrome) < coach pill (lit
        // subtitle, floats over everything) < modal overlays (fully lit on top).
        spotlightView = SpotlightView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT
            )
            visibility = View.GONE
        }
        master.addView(spotlightView)
        master.addView(buildCoachCard())   // floating pill, dynamically anchored
        master.addView(buildBpmOverlay())
        master.addView(buildPaywallOverlay())
        master.addView(buildFeedbackSheet())
        master.addView(buildLayersSheet())
        return master
    }

    private fun buildTopBar(): View {
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { bottomMargin = dp(18) }
        }
        val wordmark = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        wordmark.addView(TextView(this).apply {
            text = "NOTAP"
            setTextColor(Design.textHi)
            textSize = 17f
            letterSpacing = 0.34f
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
        })
        wordmark.addView(TextView(this).apply {
            text = "LOOPER"
            setTextColor(Design.alpha(Design.cyan, 0.9f))
            textSize = 9f
            letterSpacing = 0.62f
            typeface = Typeface.create("sans-serif-medium", Typeface.BOLD)
            // Set-once cyan bloom — the wordmark reads as a lit nameplate on
            // video. Static shadow layer, zero per-frame cost.
            setShadowLayer(Design.dpf(5f, context), 0f, 0f, Design.cyan)
        })
        val spacer = View(this).apply { layoutParams = LinearLayout.LayoutParams(0, 1, 1f) }

        ioButton = IconMorphButton(this).apply {
            layoutParams = LinearLayout.LayoutParams(dp(44), dp(44))
            onTap = {
                if (hasLoop) exportLauncher.launch("notap_loop.wav")
                else importLauncher.launch(arrayOf("audio/*", "application/octet-stream"))
            }
        }

        // State-aware context button (top-left of the action cluster):
        //   no loop  → 💡 neutral "replay tutorial" (a help affordance, not a
        //              scary red exit); loop exists → morphs to red ✕ "clear loop".
        contextButton = TextView(this).apply {
            text = "💡"; textSize = 18f; gravity = Gravity.CENTER
            setTextColor(Design.textMid)
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL; setColor(Design.surface); setStroke(dp(1), Design.stroke)
            }
            layoutParams = LinearLayout.LayoutParams(dp(44), dp(44))
            setOnTouchListener { v, e ->
                tapScale(v, e) {
                    if (contextClearMode) {
                        viewModel.clearCurrentLoop()
                        v.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
                    } else {
                        viewModel.replayOnboarding()
                        v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                    }
                }
                true
            }
        }

        bar.addView(wordmark)
        bar.addView(spacer)
        // Feedback (✉️ — async mail to the developer; ≠ 💬 chat, ≠ 💡 help) → sheet
        bar.addView(iconButton("✉️", Design.amber) {
            viewModel.onFeatureRequestTapped()
            openFeatureRequest()
        })
        bar.addView(View(this).apply { layoutParams = LinearLayout.LayoutParams(dp(10), 1) })
        bar.addView(ioButton)
        bar.addView(View(this).apply { layoutParams = LinearLayout.LayoutParams(dp(10), 1) })
        bar.addView(contextButton)
        return bar
    }

    /** Morphs the top-left context button between 💡 (help) and ✕ (clear).
     *  Guarded by the caller so it animates only on the hasLoop transition. */
    private fun morphContextButton(clearMode: Boolean) {
        contextClearMode = clearMode
        val glyph = if (clearMode) "✕" else "💡"
        val color = if (clearMode) Design.red else Design.textMid
        // quick spin-and-swap: shrink+fade out, swap glyph, overshoot back in
        contextButton.animate().rotationBy(90f).scaleX(0.6f).scaleY(0.6f).alpha(0f)
            .setDuration(130)
            .withEndAction {
                contextButton.text = glyph
                contextButton.setTextColor(color)
                contextButton.setShadowLayer(
                    if (clearMode) Design.dpf(8f, this) else 0f, 0f, 0f, Design.red
                )
                contextButton.rotation = -90f
                contextButton.animate().rotation(0f).scaleX(1f).scaleY(1f).alpha(1f)
                    .setDuration(240).setInterpolator(OvershootInterpolator(2.2f)).start()
            }.start()
    }

    private fun openFeatureRequest() = showFeedbackSheet()

    // Bespoke, fully token-styled bottom sheet — built from our own scrim+glass
    // pattern (no fragment/Material dialog) so it feels native to the app.
    private fun buildFeedbackSheet(): View {
        feedbackSheet = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT
            )
            setBackgroundColor(Color.parseColor("#CC000000"))
            visibility = View.GONE
            isClickable = true
            setOnClickListener { hideFeedbackSheet() }
        }
        feedbackPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = Design.glass(context, 28f, Design.bgBottom)
            setPadding(dp(28), dp(24), dp(28), dp(28))
            isClickable = true   // swallow taps inside the panel
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.BOTTOM
                leftMargin = dp(12); rightMargin = dp(12); bottomMargin = dp(12)
            }
        }
        // grab handle
        feedbackPanel.addView(View(this).apply {
            background = GradientDrawable().apply {
                cornerRadius = Design.dpf(2, context); setColor(Design.stroke)
            }
            layoutParams = LinearLayout.LayoutParams(dp(40), dp(4)).apply {
                gravity = Gravity.CENTER_HORIZONTAL; bottomMargin = dp(18)
            }
        })
        feedbackPanel.addView(TextView(this).apply {
            text = "SHARE YOUR THOUGHTS"; textSize = 15f; letterSpacing = 0.12f
            setTextColor(Design.textHi); typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
        })
        feedbackPanel.addView(TextView(this).apply {
            text = "Send us feedback or suggest a feature — it goes straight to the developer's inbox."
            textSize = 13f; setTextColor(Design.textMid); setLineSpacing(dp(3).toFloat(), 1f)
            setPadding(0, dp(10), 0, dp(22))
        })
        feedbackPanel.addView(sheetButton("SEND EMAIL", Design.amber, filled = true) {
            hideFeedbackSheet(); fireFeedbackEmail()
        })
        feedbackPanel.addView(sheetButton("NOT NOW", Design.textLo, filled = false) {
            hideFeedbackSheet()
        })
        feedbackSheet.addView(feedbackPanel)
        return feedbackSheet
    }

    /** Full-width sheet action in the app's tactile language. */
    private fun sheetButton(label: String, color: Int, filled: Boolean, onTap: () -> Unit): View =
        TextView(this).apply {
            text = label; textSize = 13.5f; gravity = Gravity.CENTER; letterSpacing = 0.1f
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            setTextColor(if (filled) Color.BLACK else color)
            background = if (filled) Design.pill(context, color, color)
                        else Design.pill(context, Design.surfaceHi, Design.stroke)
            setPadding(dp(20), dp(14), dp(20), dp(14))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(10) }
            setOnTouchListener { v, e ->
                tapScale(v, e) { onTap(); v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY) }
                true
            }
        }

    /** Boot-time offer to bring back the autosaved loop from the previous session.
     *  Opt-in by design: restoring starts playback immediately, so it must never
     *  happen without an explicit tap. Cancel = "not now" (the file survives). */
    private fun showRestoreDialog() {
        val (dialog, layout) = sheet("PICK UP WHERE YOU LEFT OFF?")
        layout.addView(TextView(this).apply {
            text = "Your last loop was saved when you left. Restore it and keep building, or start fresh."
            textSize = 13f; setTextColor(Design.textMid); gravity = Gravity.CENTER
            setLineSpacing(dp(3).toFloat(), 1f)
            setPadding(0, 0, 0, dp(20))
        })
        layout.addView(sheetButton("RESTORE LAST LOOP", Design.green, filled = true) {
            dialog.dismiss(); viewModel.restoreLastSession()
        })
        layout.addView(sheetButton("START FRESH", Design.textLo, filled = false) {
            dialog.dismiss(); viewModel.dismissRestorePrompt()
        })
        dialog.setOnCancelListener { viewModel.dismissRestorePrompt() }
        dialog.show()
    }

    private fun showFeedbackSheet() {
        feedbackSheet.visibility = View.VISIBLE
        feedbackSheet.alpha = 0f
        feedbackSheet.animate().alpha(1f).setDuration(160).start()
        feedbackPanel.translationY = dp(320).toFloat()
        feedbackPanel.animate().translationY(0f).setDuration(280)
            .setInterpolator(OvershootInterpolator(1.1f)).start()
    }

    private fun hideFeedbackSheet() {
        feedbackPanel.animate().translationY(dp(320).toFloat()).setDuration(200).start()
        feedbackSheet.animate().alpha(0f).setDuration(200)
            .withEndAction { feedbackSheet.visibility = View.GONE }.start()
    }

    private fun fireFeedbackEmail() {
        // TODO(product): swap for an in-app form / Canny board when one exists.
        try {
            startActivity(android.content.Intent(android.content.Intent.ACTION_SENDTO).apply {
                data = Uri.parse("mailto:feedback@notaplooper.app")
                putExtra(android.content.Intent.EXTRA_SUBJECT, "[NoTap Looper] Feedback & Feature Request")
                putExtra(
                    android.content.Intent.EXTRA_TEXT,
                    "Hi Developer, here is what I love and what I think can be improved:\n\n" +
                    "What I love:\n· \n\n" +
                    "What could be better:\n· \n\n" +
                    "A feature I wish existed:\n· \n"
                )
            })
        } catch (e: Exception) {
            toast("No email app found — reach us @notaplooper")
        }
    }

    // ---------------------------------------------------------------------
    //  Multi-track LAYERS sheet — same scrim+glass pattern as the feedback
    //  sheet. Lists the base take (protected) and every overdub; any overdub
    //  can be deleted from the mix. The list is rebuilt on open and kept live
    //  from observe() as layerCount changes (a delete pops a row instantly).
    // ---------------------------------------------------------------------
    private fun buildLayersSheet(): View {
        layersSheet = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT
            )
            setBackgroundColor(Color.parseColor("#CC000000"))
            visibility = View.GONE
            isClickable = true
            setOnClickListener { hideLayersSheet() }
        }
        layersPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = Design.glass(context, 28f, Design.bgBottom)
            setPadding(dp(24), dp(20), dp(24), dp(24))
            isClickable = true   // swallow taps inside the panel
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.BOTTOM
                leftMargin = dp(12); rightMargin = dp(12); bottomMargin = dp(12)
            }
        }
        // grab handle
        layersPanel.addView(View(this).apply {
            background = GradientDrawable().apply {
                cornerRadius = Design.dpf(2, context); setColor(Design.stroke)
            }
            layoutParams = LinearLayout.LayoutParams(dp(40), dp(4)).apply {
                gravity = Gravity.CENTER_HORIZONTAL; bottomMargin = dp(18)
            }
        })
        layersPanel.addView(TextView(this).apply {
            text = "LAYERS"; textSize = 15f; letterSpacing = 0.12f
            setTextColor(Design.textHi); typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
        })
        layersPanel.addView(TextView(this).apply {
            text = "Your loop, layer by layer. Delete any overdub to pull it out of the mix — the base take stays put."
            textSize = 12.5f; setTextColor(Design.textMid); setLineSpacing(dp(3).toFloat(), 1f)
            setPadding(0, dp(8), 0, dp(18))
        })
        layersList = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        // Cap the list height at ~half the screen so a tall stack scrolls instead of
        // pushing the panel off-screen; shrinks to content when short.
        val scroll = object : android.widget.ScrollView(this) {
            override fun onMeasure(ws: Int, hs: Int) {
                val maxH = (resources.displayMetrics.heightPixels * 0.52f).toInt()
                super.onMeasure(ws, MeasureSpec.makeMeasureSpec(maxH, MeasureSpec.AT_MOST))
            }
        }.apply {
            isVerticalScrollBarEnabled = false
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            )
            addView(layersList)
        }
        layersPanel.addView(scroll)
        layersPanel.addView(sheetButton("DONE", Design.cyan, filled = false) { hideLayersSheet() })
        layersSheet.addView(layersPanel)
        return layersSheet
    }

    /** Rebuild the layer rows for [count] live layers (index 0 = protected base,
     *  1..count-1 = deletable overdubs, newest last). */
    private fun populateLayersList(count: Int) {
        // A layer appeared while the sheet was open (overdub committed) → slide the
        // new row in instead of having the list blink to a new length. Only the
        // newcomer animates; the rows above it are already where they belong.
        val grew = count > layersRenderedCount && layersRenderedCount > 0
        layersRenderedCount = count
        layersList.removeAllViews()
        for (index in 0 until count) {
            layersList.addView(layerRow(index, count))
        }
        if (grew) {
            layersList.getChildAt(count - 1)?.let { fresh ->
                fresh.alpha = 0f
                fresh.translationY = dp(18).toFloat()
                fresh.animate().alpha(1f).translationY(0f).setDuration(300)
                    .setInterpolator(android.view.animation.DecelerateInterpolator()).start()
            }
        }
    }

    /** Collapse a layer row out of the list (fade + slide + height→0), then run
     *  [onEnd]. The engine delete is deferred to the end action so the rebuild
     *  driven by layerCount can't stomp the animation midway — the motion IS the
     *  confirmation, and ~240ms before the audio changes is imperceptible. */
    private fun animateLayerRowOut(row: View, onEnd: () -> Unit) {
        val h = row.height
        if (h <= 0) { onEnd(); return }
        val lp = row.layoutParams
        android.animation.ValueAnimator.ofFloat(1f, 0f).apply {
            duration = 240
            interpolator = android.view.animation.AccelerateInterpolator(1.4f)
            addUpdateListener {
                val f = it.animatedValue as Float
                row.alpha = f
                row.translationX = (1f - f) * dp(48)
                lp.height = (h * f).toInt().coerceAtLeast(0)
                row.layoutParams = lp          // re-assign drives the re-layout
            }
            addListener(object : android.animation.AnimatorListenerAdapter() {
                override fun onAnimationEnd(a: android.animation.Animator) = onEnd()
            })
            start()
        }
    }

    /** One channel strip in the layers sheet. Header (name + 🔒 base / 🗑 delete)
     *  plus a per-layer FX section: FLIP/OCT± toggles and VOL/REV faders when Pro,
     *  or a single tap-to-unlock upsell banner when not. The base (index 0) is
     *  protected from deletion but still takes per-layer FX (it's a layer too). */
    private fun layerRow(index: Int, count: Int): View {
        val isBase = index == 0
        val accent = if (isBase) Design.cyan else Design.green
        val card = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(14), dp(12), dp(14), dp(12))
            background = GradientDrawable().apply {
                cornerRadius = Design.dpf(14, context)
                setColor(Design.surfaceHi); setStroke(dp(1), Design.strokeSoft)
            }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { bottomMargin = dp(10) }
        }

        // --- header ---
        val header = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
        }
        header.addView(TextView(this).apply {
            text = "●"; textSize = 12f
            setTextColor(accent); setPadding(0, 0, dp(12), 0)
            setShadowLayer(Design.dpf(6f, context), 0f, 0f, accent)
        })
        header.addView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            addView(TextView(this@MainActivity).apply {
                text = if (isBase) "BASE LOOP" else "OVERDUB $index"
                textSize = 14f; setTextColor(Design.textHi)
                typeface = Typeface.create("sans-serif-black", Typeface.NORMAL); letterSpacing = 0.04f
            })
            addView(TextView(this@MainActivity).apply {
                text = if (isBase) "The foundation — protected" else "Layered on top"
                textSize = 11.5f; setTextColor(Design.textMid); setPadding(0, dp(2), 0, 0)
            })
        })
        if (isBase) {
            header.addView(TextView(this).apply {
                text = "🔒"; textSize = 15f; gravity = Gravity.CENTER
                setTextColor(Design.textLo)
                layoutParams = LinearLayout.LayoutParams(dp(40), dp(40))
            })
        } else {
            header.addView(TextView(this).apply {
                text = "🗑"; textSize = 16f; gravity = Gravity.CENTER
                background = GradientDrawable().apply {
                    shape = GradientDrawable.OVAL
                    setColor(Design.alpha(Design.red, 0.12f)); setStroke(dp(1), Design.alpha(Design.red, 0.5f))
                }
                layoutParams = LinearLayout.LayoutParams(dp(40), dp(40))
                setOnTouchListener { v, e ->
                    tapScale(v, e) {
                        v.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
                        // Collapse the row first; the engine delete fires on the end
                        // action so the layerCount rebuild lands after the motion.
                        v.isEnabled = false                 // no double-fire mid-collapse
                        animateLayerRowOut(card) { viewModel.deleteOverdub(index) }
                    }
                    true
                }
            })
        }
        card.addView(header)

        // --- per-layer FX (Pro) ---
        // Real controls for EVERY user — gating happens per control inside the strip
        // (FLIP + VOL free · OCT± Pro · REV ≤30% free), not by hiding the whole thing.
        card.addView(buildLayerFxStrip(index))
        return card
    }

    /** FX controls for one layer: FLIP/OCT−/OCT+ mutually-exclusive toggles (tap
     *  the active one again → back to dry) plus VOL and REV faders. Highlights
     *  update optimistically on tap; the engine mirrors the true state for reopen. */
    private fun buildLayerFxStrip(index: Int): View {
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(12) }
        }

        var currentFx = viewModel.layerFx(index)
        val pills = ArrayList<Pair<TextView, Int>>()
        fun fxColor(kind: Int) = when (kind) { 1 -> Design.violet; 2 -> Design.amber; else -> Design.cyan }
        fun restyle() {
            for ((pill, kind) in pills) {
                val on = currentFx == kind
                pill.setTextColor(if (on) Color.BLACK else fxColor(kind))
                pill.background = if (on) Design.pill(this, fxColor(kind), fxColor(kind))
                                  else Design.glass(this, 12f)
            }
        }
        val fxRowV = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        // OCT± are Pro, FLIP is free. A Pro pill tapped by a free user raises the
        // paywall and must NOT latch on — so the optimistic highlight only moves
        // once we know the change was allowed.
        fun addFxPill(label: String, kind: Int, last: Boolean) {
            val pro = (kind == 2 || kind == 3)
            val pill = TextView(this).apply {
                text = label; textSize = 11.5f; gravity = Gravity.CENTER
                typeface = Typeface.create("sans-serif-black", Typeface.NORMAL); letterSpacing = 0.04f
                setPadding(0, dp(11), 0, dp(11))
                layoutParams = if (pro) FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
                ) else LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                    .apply { if (!last) marginEnd = dp(8) }
                setOnTouchListener { v, e ->
                    tapScale(v, e) {
                        val next = if (currentFx == kind) 0 else kind
                        // Turning a Pro effect ON without Pro → paywall, no state change.
                        // Turning one OFF is always free (never trap a lapsed user).
                        if (pro && next != 0 && !isProNow) {
                            viewModel.setLayerFx(index, next)   // raises the paywall
                        } else {
                            currentFx = next
                            viewModel.setLayerFx(index, next)
                            restyle()
                        }
                        v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                    }
                    true
                }
            }
            pills.add(pill to kind)
            if (pro) {
                val (wrap, badge) = wrapWithCrown(pill, last)
                badge.visibility = if (isProNow) View.GONE else View.VISIBLE
                fxRowV.addView(wrap)
            } else {
                fxRowV.addView(pill)
            }
        }
        addFxPill("FLIP", 1, false)
        addFxPill("OCT −", 3, false)
        addFxPill("OCT +", 2, true)
        restyle()
        col.addView(fxRowV)

        // VOL fader: slider 0..1 → gain 0..2 (0.5 = unity), applied live — free, it's
        // a mixer fader. REV fader: 0..1 wet, applied on RELEASE only (each change
        // re-bakes a Freeverb tail, far too costly per drag frame); ≤30% free, the
        // ViewModel clamps and raises the paywall beyond, and the amber freeMarker
        // shows a free user exactly where that ceiling sits.
        col.addView(layerFader("VOL", Design.green, (viewModel.layerGain(index) / 2f),
            onChange = { v -> viewModel.setLayerGain(index, v * 2f) }))
        col.addView(layerFader("REV", Design.violet, viewModel.layerReverb(index),
            onRelease = { v -> viewModel.setLayerReverb(index, v) },
            freeMarker = if (isProNow) -1f else freeReverbMax))
        return col
    }

    /** A compact labelled fader row for the per-layer FX strip. [onChange] fires
     *  live during the drag; [onRelease] fires once on touch-up (for costly commits). */
    private fun layerFader(
        label: String, accent: Int, initial: Float,
        onChange: ((Float) -> Unit)? = null, onRelease: ((Float) -> Unit)? = null,
        freeMarker: Float = -1f
    ): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(6) }
        }
        row.addView(TextView(this).apply {
            text = label; textSize = 10f; letterSpacing = 0.1f
            setTextColor(Design.textLo)
            layoutParams = LinearLayout.LayoutParams(dp(34), ViewGroup.LayoutParams.WRAP_CONTENT)
        })
        row.addView(SliderView(this).apply {
            this.accent = accent
            layoutParams = LinearLayout.LayoutParams(0, dp(36), 1f)
            setValueSilently(initial.coerceIn(0f, 1f))
            this.onChange = onChange
            this.onRelease = onRelease
            this.freeMarker = freeMarker   // amber tick + dimmed Pro zone (non-Pro only)
        })
        return row
    }

    // (layerProUpsell removed — the strip is no longer hidden behind one wall. Free
    //  users get the real controls with per-control gating: crowns on OCT±, the amber
    //  free-ceiling marker on REV. An upsell you can SEE and partly USE converts
    //  better than a blank banner, and it doesn't delete anyone's free FLIP.)

    /** Per-row fade state: the view, its INTENDED visibility, and the running fade. */
    private class RowFader(val row: View) {
        var intent: Boolean? = null
        var anim: android.animation.ValueAnimator? = null
    }

    /** Fade ANY bottom-control row in/out, with the hero morph correctly ordered
     *  around it. Every row that changes the bottom column's height goes through
     *  here — the BPM card, the reverb card and the LAYERS/CLEAN row — so they all
     *  behave identically whether they're revealed by a mode switch, a loop
     *  starting, or ✕ clearing the loop.
     *
     *  ENTER: claim the space FIRST (arm the morph, go visible) so the circle starts
     *  gliding away immediately, then fade the row into the gap it just vacated —
     *  they move apart, so they can never overlap.
     *
     *  EXIT: fade the row out FIRST, and only free its space once it's invisible.
     *  The reverse order is what made a disappearing card cover the circle:
     *  bottomCol draws ABOVE the hero, so releasing the space immediately sent the
     *  circle expanding into a region the row was still being painted over.
     *
     *  Replaces AutoTransition for these rows entirely. AutoTransition runs
     *  fade→bounds→fade SEQUENTIALLY (3× the set's duration) and its ChangeBounds
     *  resizes without re-measuring, so it both outlived the morph and mangled
     *  TextView widths. Hand-rolling keeps the ordering explicit.
     *
     *  Edge-guarded on INTENT, never on current visibility: observe() runs at 60fps
     *  and the exit path deliberately defers GONE until the fade ends, so a
     *  visibility check would re-enter every frame and restart the fade forever. */
    private fun setRowShown(state: RowFader, show: Boolean) {
        if (state.intent == show) return
        state.intent = show
        state.anim?.cancel()
        val row = state.row
        var cancelled = false
        if (show) {
            armHeroMorph()                       // layout changes on the next pass
            row.alpha = 0f
            row.visibility = View.VISIBLE
            state.anim = android.animation.ValueAnimator.ofFloat(0f, 1f).apply {
                duration = 260
                interpolator = android.view.animation.DecelerateInterpolator()
                addUpdateListener { row.alpha = it.animatedValue as Float }
                start()
            }
        } else {
            state.anim = android.animation.ValueAnimator.ofFloat(row.alpha, 0f).apply {
                duration = 170                   // brisk: the circle is waiting on it
                interpolator = android.view.animation.AccelerateInterpolator()
                addUpdateListener { row.alpha = it.animatedValue as Float }
                addListener(object : android.animation.AnimatorListenerAdapter() {
                    override fun onAnimationCancel(a: android.animation.Animator) { cancelled = true }
                    override fun onAnimationEnd(a: android.animation.Animator) {
                        if (cancelled) return    // a re-show interrupted us — leave it visible
                        armHeroMorph()           // NOW the space is freed → circle glides in
                        row.visibility = View.GONE
                        row.alpha = 1f           // reset for the next reveal
                    }
                })
                start()
            }
        }
    }

    /** Show/hide the LAYERS pill by animating its LAYOUT WEIGHT — not with
     *  AutoTransition. AutoTransition's ChangeBounds animates bounds through
     *  setLeftTopRightBottom and never re-measures, so a TextView keeps the text
     *  layout from its previous width: the centred label renders clipped ("cut in
     *  the middle") and the pill can settle at a stale width, leaving CLEAN
     *  without the space LAYERS just freed. Animating the weight forces a real
     *  measure+layout every frame, so CLEAN genuinely grows into the gap and both
     *  labels stay correctly centred the whole way. */
    private fun animateLayersButton(show: Boolean) {
        layersBtnAnim?.cancel()
        val lp = layersButton.layoutParams as LinearLayout.LayoutParams
        if (show) layersButton.visibility = View.VISIBLE
        layersBtnAnim = android.animation.ValueAnimator.ofFloat(
            if (show) 0f else 1f, if (show) 1f else 0f
        ).apply {
            duration = 260
            interpolator = android.view.animation.PathInterpolator(0.05f, 0.7f, 0.1f, 1f)
            addUpdateListener {
                val f = it.animatedValue as Float
                lp.weight = f.coerceAtLeast(0.0001f)   // 0 weight + width 0 ⇒ collapsed
                lp.marginEnd = (dp(8) * f).toInt()     // the gap closes with it
                layersButton.alpha = f
                layersButton.layoutParams = lp         // re-assign ⇒ requestLayout ⇒ true re-measure
            }
            addListener(object : android.animation.AnimatorListenerAdapter() {
                override fun onAnimationEnd(a: android.animation.Animator) =
                    resetLayersButtonParams(show)
            })
            start()
        }
    }

    /** Restore the pill's resting params (full weight + gap) and set its final
     *  visibility, so a finished or cancelled collapse can never strand it at
     *  zero weight. */
    private fun resetLayersButtonParams(visible: Boolean) {
        val lp = layersButton.layoutParams as LinearLayout.LayoutParams
        lp.weight = 1f
        lp.marginEnd = dp(8)
        layersButton.alpha = 1f
        layersButton.visibility = if (visible) View.VISIBLE else View.GONE
        layersButton.layoutParams = lp
    }

    private fun showLayersSheet() {
        layersRenderedCount = 0   // fresh open: render the list as-is, nothing "grew"
        populateLayersList(viewModel.uiState.value.layerCount)
        layersSheet.visibility = View.VISIBLE
        layersSheet.alpha = 0f
        layersSheet.animate().alpha(1f).setDuration(160).start()
        layersPanel.translationY = dp(360).toFloat()
        layersPanel.animate().translationY(0f).setDuration(280)
            .setInterpolator(OvershootInterpolator(1.1f)).start()
    }

    private fun hideLayersSheet() {
        layersPanel.animate().translationY(dp(360).toFloat()).setDuration(200).start()
        layersSheet.animate().alpha(0f).setDuration(200)
            .withEndAction { layersSheet.visibility = View.GONE }.start()
        // The sheet can edit the CURRENT layer's reverb too — resync the main fader
        // so it never shows stale state after the sheet closes.
        reseedMainFxDashboard()
    }

    /** Re-seed the main reverb fader from the current (newest) layer's live engine
     *  state. (The FLIP/OCT± pills that used to live here are gone — per-layer FX
     *  is edited in the LAYERS sheet.) */
    private fun reseedMainFxDashboard() {
        val cur = viewModel.uiState.value.layerCount - 1
        if (cur < 0) return
        val rv = viewModel.layerReverb(cur)
        reverbSlider.setValueSilently(rv)
        val shown = if (isProNow) rv else minOf(rv, freeReverbMax)
        reverbWetLabel.text = "${Math.round(shown * 100)}%"
    }

    private fun buildTelemetryStrip(): View {
        val strip = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = Design.glass(context, 18f)
            setPadding(dp(18), dp(14), dp(18), dp(14))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { bottomMargin = dp(8) }
        }

        stateDot = TextView(this).apply {
            text = "●"; textSize = 10f; setTextColor(Design.cyan)
            setPadding(0, 0, dp(8), 0)
        }
        stateName = TextView(this).apply {
            text = "BOOTING"; textSize = 14f
            setTextColor(Design.cyan); letterSpacing = 0.14f
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
        }
        val spacer = View(this).apply { layoutParams = LinearLayout.LayoutParams(0, 1, 1f) }

        bpmReadout = TextView(this).apply {
            text = "♩ – –"; textSize = 14f
            setTextColor(Design.textMid); typeface = Typeface.MONOSPACE
            setPadding(dp(12), dp(6), dp(12), dp(6))
            setOnTouchListener { v, e ->
                tapScale(v, e) { if (viewModel.uiState.value.modeIndex == 2) openBpmOverlay() }
                true
            }
        }
        levelMeter = LevelMeterView(this).apply {
            layoutParams = LinearLayout.LayoutParams(dp(74), dp(6)).apply { marginStart = dp(14) }
        }

        strip.addView(stateDot)
        strip.addView(stateName)
        strip.addView(spacer)
        strip.addView(bpmReadout)
        strip.addView(levelMeter)
        return strip
    }

    private fun buildHero(): View {
        heroContainer = FrameLayout(this).apply {
            clipChildren = false; clipToPadding = false
            // weight=1 → the hero absorbs all leftover space AFTER the wrap-content
            // coach card + controls are measured, so siblings can never overlap it.
            // minHeight is the floor for the "limited real estate" case: the orb
            // (which self-sizes from these bounds) shrinks gracefully instead of
            // collapsing; on a pathological screen the controls scroll off first.
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
            minimumHeight = dp(180)
        }

        visualizer = LoopVisualizerView(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT
            )
        }

        val labels = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            isClickable = false; isFocusable = false
            // (kept in a field too — the hero morph scales labels together with the
            //  circle so the pair reads as ONE element changing size, not two views.)
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { gravity = Gravity.CENTER }
        }
        orbTitle = TextView(this).apply {
            text = "…"; textSize = 40f; gravity = Gravity.CENTER
            setTextColor(Design.cyan); letterSpacing = 0.06f
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
        }
        orbSub = TextView(this).apply {
            text = ""; textSize = 12.5f; gravity = Gravity.CENTER
            setTextColor(Design.textMid); letterSpacing = 0.02f
            setPadding(dp(24), dp(6), dp(24), 0)
            maxWidth = dp(220)
        }
        labels.addView(orbTitle)
        labels.addView(orbSub)

        heroLabels = labels
        heroContainer.addView(visualizer)
        heroContainer.addView(labels)

        heroContainer.setOnTouchListener { _, e ->
            when (e.action) {
                MotionEvent.ACTION_DOWN -> {
                    // A press takes over the orb's scale — drop any in-flight morph so
                    // the two animators never fight over scaleX/scaleY, and settle the
                    // transform to identity (the press animator only touches scale, so
                    // a leftover translationY would strand the orb off-centre forever).
                    heroMorphAnim?.cancel(); heroMorphAnim = null
                    setHeroTransform(0f, 1f)
                    heroContainer.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                    visualizer.animate().scaleX(0.94f).scaleY(0.94f).setDuration(70).start()
                    labels.animate().scaleX(0.94f).scaleY(0.94f).setDuration(70).start()
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    visualizer.animate().scaleX(1f).scaleY(1f).setDuration(220)
                        .setInterpolator(OvershootInterpolator(2.4f)).start()
                    labels.animate().scaleX(1f).scaleY(1f).setDuration(220)
                        .setInterpolator(OvershootInterpolator(2.4f)).start()
                    if (e.action == MotionEvent.ACTION_UP) {
                        // Layer-cap feedback: at the engine's ceiling (kMaxLayers=16 —
                        // base + 15 overdubs) an overdub tap is silently ignored by the
                        // worker; surface WHY instead of a dead-feeling orb.
                        val s = viewModel.uiState.value
                        if (s.engineState == EngineState.LOOPING && s.layerCount >= 16) {
                            toast("Layer limit reached — delete a layer to add more")
                        } else {
                            viewModel.handleActionClick()
                        }
                    }
                }
            }
            true
        }
        return heroContainer
    }

    // ---------------------------------------------------------------------
    //  Hero morph — a shared-element ("Hero") transition for the centre circle.
    //
    //  Entering/leaving CLOCK SYNC shows/hides the BPM stepper, which changes the
    //  bottom column's height. The hero band has weight=1, so it absorbs that
    //  delta and the orb — whose radius is derived from its measured bounds —
    //  used to JUMP to a new size in one frame. The control rows animate (their
    //  transition is scoped to bottomCol) but the hero deliberately isn't in that
    //  scene root: a view that self-invalidates at 60fps would thrash a scene
    //  capture. So the circle is morphed here instead, by hand.
    //
    //  Technique: capture the orb's *apparent* radius before the layout change,
    //  then on the next pre-draw (after measure+layout, before the new size is
    //  ever painted) scale the orb back to that old apparent size and settle it
    //  to 1.0. The user never sees the jump — only one circle changing size.
    //  Cost: two view properties on a hardware layer; zero allocation per frame.
    // ---------------------------------------------------------------------
    //  Every bottom-row reveal (BPM stepper on SYNC · FX+reverb rows when a loop
    //  starts/ends · LAYERS/CLEAN row) resizes the hero, so they all arm this.
    //  Armed once per layout pass: several rows can change in the same observe
    //  tick, and each must not queue its own morph.
    /** The orb's drawn centre in window coords. This — not the radius — is what
     *  actually jumps: the circle is drawn at height/2, so shrinking the hero band
     *  by the stepper's ~100dp slides the centre up ~50dp. (On most phones the
     *  radius is WIDTH-limited via min(w,h), so it often doesn't change at all —
     *  which is why a scale-only morph animated nothing and the jump remained.)
     *  Includes any in-flight transform, so it reports where the orb *looks*. */
    private fun heroCenterYInWindow(): Float {
        val loc = IntArray(2)
        visualizer.getLocationInWindow(loc)
        return loc[1] + visualizer.height / 2f
    }

    private fun setHeroTransform(ty: Float, sc: Float) {
        visualizer.translationY = ty; visualizer.scaleX = sc; visualizer.scaleY = sc
        heroLabels.translationY = ty; heroLabels.scaleX = sc; heroLabels.scaleY = sc
    }

    private fun armHeroMorph() {
        if (heroMorphArmed) return
        if (visualizer.height <= 0) return                 // not laid out yet
        // Capture where the circle currently *looks* — both values fold in an
        // in-flight morph, so rapid toggling chains smoothly instead of snapping
        // back to a stale layout position.
        val fromCenterY = heroCenterYInWindow()
        val fromRadius = visualizer.orbRadius() * visualizer.scaleX
        val vto = visualizer.viewTreeObserver
        if (!vto.isAlive) return
        heroMorphArmed = true
        vto.addOnPreDrawListener(object : android.view.ViewTreeObserver.OnPreDrawListener {
            override fun onPreDraw(): Boolean {
                visualizer.viewTreeObserver.removeOnPreDrawListener(this)
                heroMorphArmed = false
                startHeroMorph(fromCenterY, fromRadius)
                return true                                // let this frame draw
            }
        })
    }

    private fun startHeroMorph(fromCenterY: Float, fromRadius: Float) {
        // A loop being born coincides with the FX rows appearing (= a hero resize),
        // so the celebration rides the SAME animator instead of fighting it: same
        // morph, but it settles with an overshoot and a burst of rings.
        val celebrate = heroCelebratePending
        heroCelebratePending = false

        heroMorphAnim?.cancel()
        // Measure the settled target with an IDENTITY transform — a previous morph
        // may still have translation/scale on these views, which would poison the
        // reading. Safe to do mid-preDraw: nothing has been painted this frame yet.
        setHeroTransform(0f, 1f)
        val toCenterY = heroCenterYInWindow()
        val newRadius = visualizer.orbRadius()

        val dy = fromCenterY - toCenterY                   // px the orb must travel
        val s0 = if (newRadius > 1f) (fromRadius / newRadius).coerceIn(0.55f, 1.8f) else 1f
        val moved = kotlin.math.abs(dy) >= 1f
        val resized = kotlin.math.abs(s0 - 1f) >= 0.02f
        if (!moved && !resized) {                          // geometry didn't actually change
            if (celebrate) playHeroBloom()
            return
        }

        // k: 1 = "still at the old place/size", 0 = settled into the new layout.
        heroMorphAnim = android.animation.ValueAnimator.ofFloat(1f, 0f).apply {
            duration = if (celebrate) 640 else 480
            // Material "emphasized decelerate" — quick departure, long graceful
            // settle. This is the motion signature of a shared-element transition;
            // a symmetric ease would read as a resize, not as one element moving.
            interpolator = if (celebrate) OvershootInterpolator(1.4f)
                           else android.view.animation.PathInterpolator(0.05f, 0.7f, 0.1f, 1f)
            addUpdateListener {
                val k = it.animatedValue as Float          // may pass below 0 on overshoot
                setHeroTransform(dy * k, 1f + (s0 - 1f) * k)
            }
            addListener(object : android.animation.AnimatorListenerAdapter() {
                override fun onAnimationEnd(a: android.animation.Animator) = setHeroTransform(0f, 1f)
            })
            start()
        }
        if (celebrate) fireRippleBurst()
    }

    /** The loop-is-born celebration when the hero did NOT resize: a symmetric
     *  bell-curve swell (1 → 1.07 → 1) plus the ring burst. Never overlaps a
     *  morph — startHeroMorph folds the celebration in when both apply. */
    private fun playHeroBloom() {
        heroMorphAnim?.cancel()
        heroMorphAnim = android.animation.ValueAnimator.ofFloat(0f, 1f).apply {
            duration = 720
            interpolator = android.view.animation.DecelerateInterpolator()
            addUpdateListener {
                val t = it.animatedValue as Float
                setHeroTransform(0f, 1f + 0.07f * kotlin.math.sin(t * Math.PI).toFloat())
            }
            addListener(object : android.animation.AnimatorListenerAdapter() {
                override fun onAnimationEnd(a: android.animation.Animator) = setHeroTransform(0f, 1f)
            })
            start()
        }
        fireRippleBurst()
    }

    /** Three staggered rings from the visualizer's fixed pool (no allocation) —
     *  reads as energy radiating out of the orb. */
    private fun fireRippleBurst() {
        visualizer.fireRipple()
        visualizer.postDelayed({ visualizer.fireRipple() }, 90)
        visualizer.postDelayed({ visualizer.fireRipple() }, 180)
    }

    private fun buildBottom(): View {
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(8) }
        }
        bottomCol = col

        // BPM control (SYNC mode only) — ONE compact row: [120 BPM] · slider · CLICK.
        // Was two rows (± steppers above a full-width slider); the ± buttons are
        // gone because the slider plus the tap-to-type overlay already cover both
        // coarse and exact entry. Collapsing to a single row gives the hero band
        // back ~50dp, which is radius the circle keeps in SYNC mode.
        bpmStepper = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = Design.glass(context, 16f)
            setPadding(dp(14), dp(6), dp(10), dp(6))
            visibility = View.GONE
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { bottomMargin = dp(10) }
        }
        bpmStepValue = TextView(this).apply {
            text = "120"; textSize = 19f
            setTextColor(Design.violet); typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            setShadowLayer(Design.dpf(7f, context), 0f, 0f, Design.violet)   // backlit-segment glow
        }
        val bpmTag = TextView(this).apply {
            text = "BPM"; textSize = 9f; setTextColor(Design.textLo)
            letterSpacing = 0.14f; setPadding(dp(3), dp(4), 0, 0)            // optically baseline-aligned
        }
        // Value + unit act as one tap target → the numeric entry overlay.
        // Pinned LTR: on an RTL device locale a horizontal LinearLayout reverses
        // child order, which rendered the readout as "BPM 120". The value must
        // always lead its unit ("120 BPM"), exactly like the coach pill pins its
        // trailing control. Numbers+unit are LTR in every locale anyway.
        val bpmValueGroup = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
            layoutDirection = View.LAYOUT_DIRECTION_LTR
            setPadding(0, dp(6), 0, dp(6))
            addView(bpmStepValue); addView(bpmTag)
            setOnTouchListener { v, e -> tapScale(v, e) { openBpmOverlay() }; true }
        }

        // Macro drag: maps 0..1 → 40..300 BPM. Weighted so it takes exactly the gap
        // between the readout and CLICK. The binding echoes values back with
        // setValueSilently (no onChange) so the two never feed back.
        bpmSlider = SliderView(this).apply {
            accent = Design.violet
            // 120 BPM landmark: a tick on the track, a magnetic catch, and a detent
            // bump — so the most common tempo is findable at a glance, by feel, AND
            // actually landable. The full 40–300 range is compressed into ~190dp, so
            // 1 BPM is well under a pixel of travel: without a magnet, hitting exactly
            // 120 by drag is luck. The window is ±3 BPM — narrower than the finger can
            // resolve anyway — and any exact tempo is still reachable by tapping the
            // readout to type it.
            detent = sliderFromBpm(120f)
            detentSnap = sliderFromBpm(123f) - sliderFromBpm(120f)
            layoutParams = LinearLayout.LayoutParams(0, dp(34), 1f).apply {
                marginStart = dp(10); marginEnd = dp(8)
            }
            onChange = { v -> viewModel.setAbsoluteTargetBpm(bpmFromSlider(v)) }
        }

        // Metronome toggle — a speaker icon that becomes a muted speaker when off,
        // so the state is legible from the glyph alone (the pill's tint reinforces
        // it). Emoji render in their own colours, so the on/off signal lives in the
        // glyph + background + alpha rather than in setTextColor.
        metroToggle = TextView(this).apply {
            text = METRO_ICON_ON; textSize = 15f; gravity = Gravity.CENTER
            setPadding(dp(11), dp(7), dp(11), dp(7))
            setOnTouchListener { v, e ->
                tapScale(v, e) {
                    viewModel.toggleMetronome()
                    v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                }
                true
            }
        }

        bpmStepper.addView(bpmValueGroup)
        bpmStepper.addView(bpmSlider)
        bpmStepper.addView(metroToggle)

        // (The FLIP / OCT– / OCT+ row lived here and edited the newest layer. It's
        //  gone: per-layer FX belongs to the LAYERS sheet, where every layer has its
        //  own strip and it's unambiguous WHICH layer you're changing. That also
        //  frees another row of height back to the hero.)

        // Reverb — ONE compact row, matching the BPM card: [REVERB 👑] · slider · [30%].
        // Was a header line stacked over a full-width slider (~86dp); now ~46dp, and
        // that height goes back to the hero. The slider is inset from BOTH ends so it
        // never crowds the label or the readout.
        reverbRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            visibility = View.GONE
            background = Design.glass(context, 16f)
            setPadding(dp(14), dp(6), dp(12), dp(6))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { bottomMargin = dp(10) }
        }
        // Crown hint: signals up front that this control has a Pro ceiling, so
        // dragging past it is expected rather than click-frustration. The exact
        // boundary is drawn ON the slider (freeMarker tick + dimmed locked zone),
        // so the old "30%+" text was redundant once the row got tight. Hidden when Pro.
        reverbProBadge = TextView(this).apply {
            text = "👑"; textSize = 10f
            setShadowLayer(Design.dpf(Design.glowDp, context), 0f, 0f, Design.amber)
            setPadding(dp(4), 0, 0, 0)
        }
        // Label + crown read as one unit; pinned LTR so an RTL locale can't put the
        // crown before the word (same reason the BPM readout is pinned).
        val reverbLabelGroup = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
            layoutDirection = View.LAYOUT_DIRECTION_LTR
            addView(TextView(this@MainActivity).apply {
                text = "REVERB"; textSize = 11.5f; letterSpacing = 0.1f
                setTextColor(Design.textHi)
                typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            })
            addView(reverbProBadge)
        }
        reverbWetLabel = TextView(this).apply {
            text = "0%"; textSize = 12f; gravity = Gravity.END
            setTextColor(Design.green)
            typeface = Typeface.MONOSPACE
            setShadowLayer(Design.dpf(6f, context), 0f, 0f, Design.green)   // backlit-segment glow
            // Fixed width so the slider doesn't twitch as the readout grows 0%→100%.
            layoutParams = LinearLayout.LayoutParams(dp(38), ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        reverbSlider = SliderView(this).apply {
            accent = Design.green
            layoutParams = LinearLayout.LayoutParams(0, dp(34), 1f).apply {
                marginStart = dp(12); marginEnd = dp(10)
            }
            onChange = { v ->
                // Live: drive tutorial completion + the readout only. The per-layer
                // reverb bake is costly, so it's committed on RELEASE (below).
                viewModel.onCurrentLayerReverbChange(v)
                val shown = if (isProNow || reverbTasteWindow) v else minOf(v, freeReverbMax)
                reverbWetLabel.text = "${Math.round(shown * 100)}%"
            }
            // Bake the current (newest) layer's reverb on release — ≤30% free, deeper
            // is Pro (clamp + paywall).
            onRelease = { v -> viewModel.commitCurrentLayerReverb(v) }
        }
        reverbRow.addView(reverbLabelGroup)
        reverbRow.addView(reverbSlider)
        reverbRow.addView(reverbWetLabel)

        // Multi-track row: "LAYERS · N" (appears once overdubs exist — tap → the
        // layers sheet) beside "✨ CLEAN" (Pro spectral noise gate on the whole
        // mix — appears whenever a loop is playing). Each child toggles its own
        // visibility; the row shows if either is live.
        layersRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            visibility = View.GONE
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { bottomMargin = dp(10) }
        }
        layersButton = TextView(this).apply {
            text = "🎚  LAYERS"; textSize = 12f; gravity = Gravity.CENTER
            setTextColor(Design.textHi); letterSpacing = 0.08f
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            setPadding(0, dp(14), 0, dp(14))
            background = Design.glass(context, 14f)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                .apply { marginEnd = dp(8) }
            setOnTouchListener { v, e ->
                tapScale(v, e) {
                    // During the tutorial's LAYERS step, opening the mixer IS the advance
                    // to the next step (replaces the coach card's → arrow). No-op otherwise.
                    val st = viewModel.uiState.value
                    if (st.onboardingActive && st.onboardingStage == OnboardingStage.LAYERS_INTRO) {
                        viewModel.onCoachContinue()
                    }
                    showLayersSheet(); v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                }
                true
            }
        }
        layersRow.addView(layersButton)
        cleanButton = TextView(this).apply {
            text = "✨ CLEAN"; textSize = 12f; gravity = Gravity.CENTER
            setTextColor(Design.green); letterSpacing = 0.08f
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            setPadding(0, dp(14), 0, dp(14))
            background = Design.glass(context, 14f)
            setOnTouchListener { v, e ->
                tapScale(v, e) {
                    viewModel.toggleCleanAll()
                    v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                }
                true
            }
        }
        val (cleanWrap, cBadge) = wrapWithCrown(cleanButton, last = true)
        cleanContainer = cleanWrap
        cleanBadge = cBadge
        layersRow.addView(cleanWrap)

        segmented = SegmentedControlView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            )
            onSelect = { idx -> viewModel.setDetectionModeDirectly(idx) }
            onInfo = { idx -> showModeInfo(idx) }
        }

        col.addView(bpmStepper)
        col.addView(layersRow)
        col.addView(reverbRow)
        col.addView(segmented)
        return col
    }

    // (fxPill / onMainFxTap / restyleMainFx / styleMainFxPill removed with the main
    //  FX row — the LAYERS sheet builds its own per-layer pills and drives them
    //  through viewModel.setLayerFx(index, kind), which names the target layer
    //  explicitly instead of implying "the newest one".)

    // Reusable premium marker: a small amber crown with a soft glow. One source
    // for every Pro-gated affordance so the signal reads identically everywhere.
    private fun crownBadge(): TextView =
        TextView(this).apply {
            text = "👑"; textSize = 11f
            setShadowLayer(Design.dpf(Design.glowDp, context), 0f, 0f, Design.amber)
        }

    // Overlays a crown on a pill's top-right corner. Returns (wrapper, badge);
    // the wrapper inherits the pill's row weight, the badge toggles with isPro.
    private fun wrapWithCrown(pill: TextView, last: Boolean): Pair<FrameLayout, TextView> {
        val frame = FrameLayout(this).apply {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                .apply { if (!last) marginEnd = dp(8) }
        }
        pill.layoutParams = FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        )
        val badge = crownBadge().apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { gravity = Gravity.TOP or Gravity.END; topMargin = dp(4); marginEnd = dp(6) }
        }
        frame.addView(pill)
        frame.addView(badge)
        return frame to badge
    }

    // ---------------------------------------------------------------------
    //  Small reusable pieces
    // ---------------------------------------------------------------------
    private fun iconButton(glyph: String, color: Int, onClick: (View) -> Unit): View =
        TextView(this).apply {
            text = glyph; textSize = 20f; gravity = Gravity.CENTER
            setTextColor(color)
            typeface = Typeface.DEFAULT_BOLD
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(Design.surface)
                setStroke(dp(1), Design.stroke)
            }
            layoutParams = LinearLayout.LayoutParams(dp(44), dp(44))
            setOnTouchListener { v, e -> tapScale(v, e) { onClick(v) }; true }
        }

    // (stepButton removed with the BPM ±: the weighted slider covers coarse moves
    //  and the tap-to-type overlay covers exact ones — no other caller existed.)

    private inline fun tapScale(v: View, e: MotionEvent, crossinline onUp: () -> Unit) {
        when (e.action) {
            MotionEvent.ACTION_DOWN -> v.animate().scaleX(0.86f).scaleY(0.86f).alpha(0.7f).setDuration(50).start()
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                v.animate().scaleX(1f).scaleY(1f).alpha(1f).setDuration(150).start()
                if (e.action == MotionEvent.ACTION_UP) onUp()
            }
        }
    }

    private fun dp(v: Int) = Design.dp(v, this)

    // (AutoTransition is gone from the bottom rows. It ran fade→bounds→fade
    //  SEQUENTIALLY — 3× the set's duration, so a disappearing card outlived the
    //  hero morph and covered the circle — and its ChangeBounds resized views
    //  without re-measuring, which clipped TextView labels mid-flight. Every row
    //  now fades through setRowShown, where the ordering against the morph is
    //  explicit and the layout stays real. See also animateLayersButton.)

    // BPM ⇄ slider mapping (40–300 BPM over the 0..1 travel). Rounded to whole BPM:
    // 1 BPM is sub-pixel on this fader so finer resolution is unreachable anyway,
    // and rounding guarantees the 120 detent yields exactly 120.0 — float drift
    // could otherwise land on 119.99999, which the truncating readout shows as 119.
    private fun bpmFromSlider(v: Float) = Math.round(40f + v * 260f).toFloat()
    private fun sliderFromBpm(bpm: Float) = ((bpm - 40f) / 260f).coerceIn(0f, 1f)

    // ---------------------------------------------------------------------
    //  First-time onboarding coach — a FLOATING GLASS PILL over the UI. It never
    //  reflows the layout (the hero keeps its full weight); instead it is
    //  dynamically anchored to a safe band OUTSIDE the orb's measured bounds, so
    //  it can't overlap the visualizer. The SpotlightView carries the focus; the
    //  pill is just a subtitle. ≤2 lines + one trailing control.
    // ---------------------------------------------------------------------
    private fun buildCoachCard(): View {
        coachCard = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            // Force LTR: the UI is English, but on an RTL device locale a horizontal
            // LinearLayout flips child order and the trailing control lands on the
            // LEFT. Pin LTR so text-then-trailing is always physically left-then-right.
            layoutDirection = View.LAYOUT_DIRECTION_LTR
            background = Design.glass(context, 100f)   // fully-rounded pill
            setPadding(dp(18), dp(10), dp(10), dp(10))
            visibility = View.GONE
            alpha = 0f
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.TOP or Gravity.CENTER_HORIZONTAL
                leftMargin = dp(16); rightMargin = dp(16)
            }
        }
        val textCol = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        onboardingTitle = TextView(this).apply {
            textSize = 14f; setTextColor(Design.textHi)
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            maxLines = 1; ellipsize = android.text.TextUtils.TruncateAt.END
        }
        onboardingSub = TextView(this).apply {
            textSize = 11.5f; setTextColor(Design.textMid)
            maxLines = 1; ellipsize = android.text.TextUtils.TruncateAt.END
            setPadding(0, dp(2), 0, 0)
        }
        textCol.addView(onboardingTitle)
        textCol.addView(onboardingSub)
        // Single trailing control — ✕ skip / → continue / ✓ finish per state.
        coachTrailing = TextView(this).apply {
            text = "✕"; textSize = 15f; gravity = Gravity.CENTER
            setTextColor(Design.textMid)
            typeface = Typeface.DEFAULT_BOLD
            layoutParams = LinearLayout.LayoutParams(dp(36), dp(36)).apply { marginStart = dp(8) }
            setOnTouchListener { v, e ->
                tapScale(v, e) {
                    if (coachTrailingContinues) viewModel.onCoachContinue()
                    else viewModel.completeOnboarding(skipped = true)
                    v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                }
                true
            }
        }
        coachCard.addView(textCol)
        coachCard.addView(coachTrailing)
        return coachCard
    }

    private fun bindOnboardingStage(stage: OnboardingStage) {
        val (title, sub) = when (stage) {
            OnboardingStage.LISTEN_ROOM -> "One quiet second…" to "Learning your room so I only hear your guitar."
            OnboardingStage.PLUCK -> "Just start playing" to "No buttons — recording begins on your first note."
            OnboardingStage.KEEP_PLAYING -> "Keep playing" to "Stop when you're done and I'll catch the loop."
            OnboardingStage.BUILDING -> "Building your loop" to "Finding the beat, trimming the edges…"
            OnboardingStage.FIRST_LOOP -> "That's your loop 🔁" to "It plays forever. Ready to build on it?"
            OnboardingStage.LAYER -> "Tap the orb to layer" to "Overdub a second riff — tap again to lock it in."
            OnboardingStage.LAYER_CELEBRATE -> "Layers stacked! 🎚️" to "Listen to how they blend, then continue."
            OnboardingStage.LAYERS_INTRO -> "Your layer mixer 🎚️" to "Open LAYERS to delete any overdub — or add per-layer FX."
            OnboardingStage.ELEVATE -> "Make it bloom" to "Slide the glowing reverb — hear what Pro sounds like."
        }
        onboardingTitle.text = title
        onboardingSub.text = sub
        onboardingTitle.setTextColor(when (stage) {
            OnboardingStage.FIRST_LOOP, OnboardingStage.LAYER_CELEBRATE, OnboardingStage.LAYERS_INTRO -> Design.green
            OnboardingStage.ELEVATE -> Design.amber
            else -> Design.textHi
        })
        // crossfade the text so each new instruction registers as new
        onboardingTitle.alpha = 0f; onboardingSub.alpha = 0f
        onboardingTitle.animate().alpha(1f).setDuration(240).start()
        onboardingSub.animate().alpha(1f).setDuration(280).start()
    }

    /** The single trailing control: only FIRST_LOOP advances via a tap (→); every
     *  other step — including ELEVATE, which now auto-completes on the reverb
     *  drag — shows a skip ✕. */
    private fun updateCoachTrailing(stage: OnboardingStage) {
        // LAYERS_INTRO advances by tapping the LAYERS button itself (not the arrow),
        // so it carries the plain skip ✕ like the other interaction-driven steps.
        coachTrailingContinues =
            (stage == OnboardingStage.FIRST_LOOP || stage == OnboardingStage.LAYER_CELEBRATE)
        if (coachTrailingContinues) {
            coachTrailing.text = "→"; coachTrailing.setTextColor(Design.cyan)
        } else {
            coachTrailing.text = "✕"; coachTrailing.setTextColor(Design.textMid)
        }
    }

    /** Anchor the pill in a band that lies OUTSIDE the orb's measured circle —
     *  ELEVATE prefers the top band (clear of the bottom reverb target), every
     *  other step prefers the bottom band. Falls back to the other band, then the
     *  taller one, if the preferred can't hold the pill. Post-layout, per stage. */
    private fun anchorCoachPill(stage: OnboardingStage) {
        coachCard.post {
            val mo = IntArray(2); masterFrame.getLocationInWindow(mo)
            val ho = IntArray(2); heroContainer.getLocationInWindow(ho)
            val heroTop = (ho[1] - mo[1]).toFloat()
            val heroCenterY = heroTop + heroContainer.height / 2f
            val orbR = visualizer.orbRadius() * 1.25f          // include the glow ring
            val orbTop = heroCenterY - orbR
            val orbBottom = heroCenterY + orbR
            val margin = dp(16).toFloat()
            val pillH = coachCard.height.toFloat()

            val topBandTop = heroTop + margin
            val topBandBottom = orbTop - margin
            val bottomBandTop = orbBottom + margin
            val bottomBandBottom = heroTop + heroContainer.height - margin
            val topH = topBandBottom - topBandTop
            val bottomH = bottomBandBottom - bottomBandTop

            val preferTop = stage == OnboardingStage.ELEVATE || stage == OnboardingStage.LAYERS_INTRO
            val useTop = when {
                preferTop && topH >= pillH -> true
                !preferTop && bottomH >= pillH -> false
                topH >= pillH -> true
                bottomH >= pillH -> false
                else -> topH >= bottomH
            }
            // top band → anchor near the top; bottom band → anchor near the bottom
            val y = if (useTop) topBandTop else (bottomBandBottom - pillH)
            coachCard.animate().translationY(y.coerceAtLeast(topBandTop))
                .setDuration(260)
                .setInterpolator(android.view.animation.DecelerateInterpolator())
                .start()
        }
    }

    /** Dim the chrome and illuminate the hero + the current target. The pill
     *  floats ABOVE this scrim, so it needs no hole. Recomputed only on stage
     *  change (post-layout), never per frame. */
    private fun updateSpotlight(stage: OnboardingStage) {
        val spotlit = stage == OnboardingStage.LAYER || stage == OnboardingStage.ELEVATE ||
            stage == OnboardingStage.LAYERS_INTRO
        if (!spotlit) {
            spotlightView.hide(); spotlightView.visibility = View.GONE
            stopCoachPulse()
            return
        }
        spotlightView.visibility = View.VISIBLE
        spotlightView.post {
            val origin = IntArray(2); spotlightView.getLocationInWindow(origin)
            fun holeOf(v: View, padDp: Int): RectF {
                val loc = IntArray(2); v.getLocationInWindow(loc)
                val p = dp(padDp).toFloat()
                return RectF(
                    loc[0] - origin[0] - p, loc[1] - origin[1] - p,
                    loc[0] - origin[0] + v.width + p, loc[1] - origin[1] + v.height + p
                )
            }
            val hl = IntArray(2); heroContainer.getLocationInWindow(hl)
            val hcx = hl[0] - origin[0] + heroContainer.width / 2f
            val hcy = hl[1] - origin[1] + heroContainer.height / 2f
            val hr = minOf(heroContainer.width, heroContainer.height) / 2f * 0.82f
            val rects = mutableListOf<RectF>()
            if (stage == OnboardingStage.ELEVATE) rects.add(holeOf(reverbRow, 8))
            if (stage == OnboardingStage.LAYERS_INTRO) rects.add(holeOf(layersRow, 8))
            spotlightView.setHoles(hcx, hcy, hr, rects)
        }
        // The LAYERS pill is spotlit at full brightness through the scrim hole and
        // reads as the twin of the coach card — so its pulse is scale-only (no alpha
        // dip), keeping it at the SAME opacity as the "Your layer mixer" card. The
        // orb/reverb targets keep the breathing alpha.
        startCoachPulse(
            when (stage) {
                OnboardingStage.LAYER -> heroContainer
                OnboardingStage.LAYERS_INTRO -> layersRow
                else -> reverbRow
            },
            dimAlpha = stage != OnboardingStage.LAYERS_INTRO
        )
    }

    // Single INFINITE/REVERSE animator on the current target — one view, two
    // properties, cancelled on retarget and when onboarding ends. No arrows.
    // [dimAlpha] false keeps the target fully opaque (scale-only breathing).
    private fun startCoachPulse(target: View, dimAlpha: Boolean = true) {
        if (coachPulseTarget === target && coachPulse?.isRunning == true) return
        stopCoachPulse()
        coachPulseTarget = target
        coachPulse = android.animation.ValueAnimator.ofFloat(0f, 1f).apply {
            duration = 900
            repeatCount = android.animation.ValueAnimator.INFINITE
            repeatMode = android.animation.ValueAnimator.REVERSE
            interpolator = android.view.animation.AccelerateDecelerateInterpolator()
            addUpdateListener {
                val f = it.animatedValue as Float
                val s = 1f + 0.04f * f
                target.scaleX = s; target.scaleY = s
                target.alpha = if (dimAlpha) 0.85f + 0.15f * f else 1f
            }
            start()
        }
    }

    private fun stopCoachPulse() {
        coachPulse?.cancel(); coachPulse = null
        coachPulseTarget?.let { it.scaleX = 1f; it.scaleY = 1f; it.alpha = 1f }
        coachPulseTarget = null
    }

    // ---------------------------------------------------------------------
    //  PremiumUpgradeScreen — full-bleed slide-up overlay (keeps the audio
    //  session + hero alive behind it). Pricing/copy come ONLY from
    //  PremiumPlans; unlock is stubbed until Play Billing.
    // ---------------------------------------------------------------------
    private fun buildPaywallOverlay(): View {
        paywallOverlay = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT
            )
            setBackgroundColor(Color.parseColor("#EE000000"))
            visibility = View.GONE
            isClickable = true
            setOnClickListener { viewModel.dismissPaywall() }
        }
        premiumPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = Design.glass(context, 30f, Design.bgBottom)
            setPadding(dp(26), dp(22), dp(26), dp(24))
            isClickable = true
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.BOTTOM
                leftMargin = dp(10); rightMargin = dp(10); bottomMargin = dp(10)
            }
        }
        // grab handle
        premiumPanel.addView(View(this).apply {
            background = GradientDrawable().apply {
                cornerRadius = Design.dpf(2, context); setColor(Design.stroke)
            }
            layoutParams = LinearLayout.LayoutParams(dp(40), dp(4)).apply {
                gravity = Gravity.CENTER_HORIZONTAL; bottomMargin = dp(16)
            }
        })
        premiumPanel.addView(TextView(this).apply {
            text = "NOTAP PRO ✦"; textSize = 26f; gravity = Gravity.CENTER
            setTextColor(Design.amber); letterSpacing = 0.16f
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            setShadowLayer(Design.dpf(12f, context), 0f, 0f, Design.amber)
        })
        // Context line — which feature the user reached for (set in binding)
        paywallFeatureLabel = TextView(this).apply {
            textSize = 12.5f; gravity = Gravity.CENTER
            setTextColor(Design.textMid); setPadding(0, dp(8), 0, dp(18))
        }
        premiumPanel.addView(paywallFeatureLabel)

        // Value props
        PremiumPlans.valueProps.forEach { (head, detail) ->
            premiumPanel.addView(LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(0, dp(6), 0, dp(6))
                addView(TextView(this@MainActivity).apply {
                    text = "✦"; textSize = 14f; setTextColor(Design.amber)
                    setPadding(0, 0, dp(12), 0)
                })
                addView(LinearLayout(this@MainActivity).apply {
                    orientation = LinearLayout.VERTICAL
                    addView(TextView(this@MainActivity).apply {
                        text = head; textSize = 14f; setTextColor(Design.textHi)
                        typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
                    })
                    addView(TextView(this@MainActivity).apply {
                        text = detail; textSize = 12f; setTextColor(Design.textMid)
                        setPadding(0, dp(1), 0, 0)
                    })
                })
            })
        }

        // Pricing — decoy first, anchored hero second
        premiumPanel.addView(View(this).apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(18))
        })
        premiumPanel.addView(planCard(PremiumPlans.monthly))
        premiumPanel.addView(planCard(PremiumPlans.lifetime))

        premiumPanel.addView(TextView(this).apply {
            text = "Placeholder pricing — no charge yet (billing coming soon)."
            textSize = 10f; gravity = Gravity.CENTER; setTextColor(Design.textLo)
            setPadding(0, dp(14), 0, 0)
        })
        premiumPanel.addView(TextView(this).apply {
            text = "MAYBE LATER"; textSize = 12f; gravity = Gravity.CENTER
            setTextColor(Design.textLo); letterSpacing = 0.12f
            setPadding(0, dp(14), 0, dp(2))
            setOnTouchListener { v, e -> tapScale(v, e) { viewModel.dismissPaywall() }; true }
        })

        paywallOverlay.addView(premiumPanel)
        return paywallOverlay
    }

    /** One pricing card. Highlighted plan gets the accent border-glow + tag;
     *  the decoy gets a struck-through price. Tap → onPlanSelected(productId). */
    private fun planCard(plan: PremiumPlans.Plan): View {
        val accent = if (plan.highlighted) Design.amber else Design.textMid
        val card = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(18), dp(16), dp(18), dp(16))
            background = GradientDrawable().apply {
                cornerRadius = Design.dpf(18, context)
                setColor(if (plan.highlighted) Design.alpha(Design.amber, 0.10f) else Design.surfaceHi)
                setStroke(dp(if (plan.highlighted) 2 else 1),
                          if (plan.highlighted) Design.amber else Design.stroke)
            }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(10) }
            setOnTouchListener { v, e ->
                tapScale(v, e) {
                    viewModel.onPlanSelected(plan.productId)
                    v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                }
                true
            }
        }
        // left: title (+ tag) / subtext
        card.addView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
                addView(TextView(this@MainActivity).apply {
                    text = plan.title; textSize = 15f
                    setTextColor(if (plan.highlighted) Design.textHi else Design.textMid)
                    typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
                })
                plan.tag?.let { tag ->
                    addView(TextView(this@MainActivity).apply {
                        text = tag; textSize = 9f; letterSpacing = 0.1f
                        setTextColor(Color.BLACK)
                        typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
                        background = Design.pill(context, Design.amber, Design.amber)
                        setPadding(dp(8), dp(3), dp(8), dp(3))
                        layoutParams = LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
                        ).apply { marginStart = dp(8) }
                    })
                }
            })
            addView(TextView(this@MainActivity).apply {
                text = plan.subtext; textSize = 11.5f; setTextColor(Design.textMid)
                setPadding(0, dp(3), 0, 0)
            })
        })
        // right: price (decoy struck-through)
        card.addView(TextView(this).apply {
            text = plan.price; textSize = if (plan.highlighted) 22f else 15f
            setTextColor(accent)
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            if (!plan.highlighted) paintFlags = paintFlags or android.graphics.Paint.STRIKE_THRU_TEXT_FLAG
            if (plan.highlighted) setShadowLayer(Design.dpf(8f, context), 0f, 0f, Design.amber)
        })
        return card
    }

    // ---------------------------------------------------------------------
    //  BPM entry overlay
    // ---------------------------------------------------------------------
    private fun buildBpmOverlay(): View {
        bpmOverlay = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT
            )
            setBackgroundColor(Color.parseColor("#E8000000"))
            visibility = View.GONE
            isClickable = true
            setOnClickListener { closeBpmOverlay() }
        }
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER
            background = Design.glass(context, 28f, Design.bgBottom)
            setPadding(dp(36), dp(36), dp(36), dp(36))
            layoutParams = FrameLayout.LayoutParams(
                (resources.displayMetrics.widthPixels * 0.82).toInt(),
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { gravity = Gravity.CENTER }
            isClickable = true
        }
        box.addView(TextView(this).apply {
            text = "TARGET TEMPO"; textSize = 12f; letterSpacing = 0.28f
            setTextColor(Design.textLo); gravity = Gravity.CENTER
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
        })
        bpmEditText = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_NUMBER; isSingleLine = true
            setTextColor(Design.violet); textSize = 60f; gravity = Gravity.CENTER
            background = null
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(12); bottomMargin = dp(4) }
            setOnEditorActionListener { _, actionId, _ ->
                if (actionId == EditorInfo.IME_ACTION_DONE || actionId == EditorInfo.IME_NULL) {
                    text.toString().toFloatOrNull()?.let { viewModel.setAbsoluteTargetBpm(it) }
                    closeBpmOverlay(); true
                } else false
            }
        }
        box.addView(bpmEditText)
        box.addView(TextView(this).apply {
            text = "40 – 300 BPM"; textSize = 11f; setTextColor(Design.textLo)
            gravity = Gravity.CENTER; letterSpacing = 0.1f
        })
        bpmOverlay.addView(box)
        return bpmOverlay
    }

    private fun openBpmOverlay() {
        bpmEditText.setText(viewModel.uiState.value.targetBpm.toInt().toString())
        bpmEditText.setSelection(bpmEditText.text.length)
        bpmOverlay.visibility = View.VISIBLE
        bpmOverlay.alpha = 0f
        bpmOverlay.animate().alpha(1f).setDuration(160).start()
        bpmEditText.postDelayed({
            bpmEditText.requestFocus()
            (getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager)
                .showSoftInput(bpmEditText, InputMethodManager.SHOW_IMPLICIT)
        }, 120)
    }

    private fun closeBpmOverlay() {
        (getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager)
            .hideSoftInputFromWindow(bpmEditText.windowToken, 0)
        bpmOverlay.animate().alpha(0f).setDuration(140)
            .withEndAction { bpmOverlay.visibility = View.GONE }.start()
    }

    // ---------------------------------------------------------------------
    //  Dialogs
    // ---------------------------------------------------------------------
    private fun sheet(title: String): Pair<android.app.AlertDialog, LinearLayout> {
        val dialog = android.app.AlertDialog.Builder(this).create()
        dialog.window?.setBackgroundDrawable(android.graphics.drawable.ColorDrawable(Color.TRANSPARENT))
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(28), dp(28), dp(28), dp(28))
            background = Design.glass(context, 28f)
        }
        layout.addView(TextView(this).apply {
            text = title; textSize = 13f; letterSpacing = 0.22f
            setTextColor(Design.textLo); gravity = Gravity.CENTER
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            setPadding(0, 0, 0, dp(22))
        })
        dialog.setView(layout)
        return dialog to layout
    }

    // ---------------------------------------------------------------------
    //  Import / export (Storage Access Framework)
    // ---------------------------------------------------------------------
    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()

    private fun handleImport(uri: Uri) {
        // Copy the picked file to a real path (the native decoder needs a path,
        // not a content:// URI), then hand it to the engine. Decodes WAV/MP3/FLAC.
        lifecycleScope.launch(Dispatchers.IO) {
            val ok = try {
                val f = File(cacheDir, "notap_import.wav")
                contentResolver.openInputStream(uri)?.use { inp -> f.outputStream().use { inp.copyTo(it) } }
                viewModel.importLoop(f.absolutePath)
            } catch (e: Exception) { false }
            withContext(Dispatchers.Main) {
                toast(if (ok) "Loop imported — tap the orb to overdub"
                      else "Couldn't decode that file (use WAV, MP3 or FLAC)")
            }
        }
    }

    private fun handleExport(uri: Uri) {
        // Engine writes a WAV to a temp path; copy those bytes into the
        // user-chosen destination stream.
        lifecycleScope.launch(Dispatchers.IO) {
            val ok = try {
                val tmp = File(cacheDir, "notap_export.wav")
                val exported = viewModel.exportLoop(tmp.absolutePath)
                if (exported) {
                    contentResolver.openOutputStream(uri)?.use { out -> tmp.inputStream().use { it.copyTo(out) } }
                }
                exported
            } catch (e: Exception) { false }
            withContext(Dispatchers.Main) {
                toast(if (ok) "Loop saved" else "Nothing to save yet")
            }
        }
    }

    private fun showModeInfo(index: Int) {
        val (dialog, layout) = sheet("DETECTION MODE")
        val info = listOf(
            Triple("AUTO SILENCE", Design.cyan,
                "Fully hands-free. Starts on a real pluck, stops when the sound decays. Best for free playing — the pedal-killer."),
            Triple("TAP & TRIM", Design.amber,
                "You mark the boundaries. Tap the orb to start, tap to stop. Sample-exact loop length, full manual control."),
            Triple("CLOCK SYNC", Design.violet,
                "Hands-free start, clock-locked length. Pluck to begin; you set the tempo, and the loop snaps to an exact bar count — sample-perfect length that stays in sync. Best when playing to a known tempo.")
        )
        info.forEachIndexed { i, (name, color, desc) ->
            val selectedNow = viewModel.uiState.value.modeIndex == i
            val card = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(20), dp(18), dp(20), dp(18))
                background = GradientDrawable().apply {
                    cornerRadius = Design.dpf(16, context)
                    setColor(if (selectedNow) Design.alpha(color, 0.10f) else Design.surfaceHi)
                    setStroke(dp(if (selectedNow) 2 else 1), if (selectedNow) color else Design.strokeSoft)
                }
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply { bottomMargin = dp(12) }
                setOnTouchListener { v, e ->
                    tapScale(v, e) { viewModel.setDetectionModeDirectly(i); dialog.dismiss() }
                    true
                }
            }
            card.addView(TextView(this).apply {
                text = name; textSize = 15f
                setTextColor(if (selectedNow) color else Design.textHi)
                typeface = Typeface.create("sans-serif-black", Typeface.NORMAL); letterSpacing = 0.06f
            })
            card.addView(TextView(this).apply {
                text = desc; textSize = 12.5f; setLineSpacing(dp(3).toFloat(), 1f)
                setTextColor(Design.textMid); setPadding(0, dp(8), 0, 0)
            })
            layout.addView(card)
        }
        dialog.show()
    }
}
