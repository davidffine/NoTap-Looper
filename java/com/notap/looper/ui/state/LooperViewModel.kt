package com.notap.looper.ui.state

import android.app.Application
import android.os.SystemClock
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import com.notap.looper.AudioEngine
import com.notap.looper.EngineState

/**
 * CONCURRENCY MODEL (read before touching):
 * The C++ engine never calls up into Kotlin — this is a PULL architecture.
 * JNI getters read C++ atomics; JNI commands set lock-free flags the worker
 * consumes. Everything in this class (the 60fps poll coroutine AND all user
 * action methods) runs on Dispatchers.Main, so ViewModel state mutation is
 * fully serialized — there is no JNI-callback race by construction.
 * Rules that keep it that way:
 *  1. Engine state transitions are detected EDGE-WISE inside the single poll
 *     coroutine (prevEngineState) — one writer for onboarding/analytics logic.
 *  2. UI-only fields (onboarding, paywall) ride through the poll loop's copy()
 *     untouched; the poll never writes them, actions never write telemetry.
 *  3. Analytics fire on edges/actions only — NEVER inside the per-tick body.
 */
class LooperViewModel(app: Application) : AndroidViewModel(app) {

    private val audioEngine = AudioEngine()
    private val prefs = Prefs(app)
    private val modes = arrayOf("AUTO SILENCE", "TAP & TRIM", "CLOCK SYNC")

    private val _uiState = MutableStateFlow(
        LooperUiState(
            onboardingActive = !prefs.hasOnboarded,
            isPro = prefs.isPro
        )
    )
    val uiState: StateFlow<LooperUiState> = _uiState.asStateFlow()

    // מעטפת הגל של הלופ הפעיל — נדגמת ב-~2Hz (משתנה רק בהקלטה/אוברדאב/אפקט)
    private val _waveform = MutableStateFlow(FloatArray(0))
    val waveform: StateFlow<FloatArray> = _waveform.asStateFlow()
    private val waveBins = FloatArray(120)
    private var frameTick = 0

    private var pollingJob: Job? = null
    // Single JNI crossing per frame — index contract documented on AudioEngine.pollTelemetry.
    private val telemetryBuffer = FloatArray(9)
    private var prevEngineState = EngineState.UNKNOWN
    private var everLooped = false
    private var recordingStartedAtMs = 0L   // take-timer anchor (poll-loop owned)

    // Lifecycle: booted = engine+loop exist in memory (survive background);
    // audioActive = Oboe streams currently open. The engine outlives the streams.
    private var booted = false
    private var audioActive = false

    // --- tutorial state machine: ONE writer (the poll coroutine + main-thread
    //     actions — all on Dispatchers.Main, fully serialized). UiState only
    //     mirrors tutStage; timing is timestamp comparison per tick, NO new
    //     coroutines and NO delay() anywhere. ---
    private var tutStage = OnboardingStage.LISTEN_ROOM
    private var stageShownAtMs = 0L
    private var reverbTasted = false          // ELEVATE gate: user has touched reverb

    private companion object {
        // The ONLY timer left in the tutorial: a reading buffer for step 1, so the
        // app's premise is legible before calibration (~1s) whisks it away. Every
        // later step is 100% event-driven (waits indefinitely for the interaction).
        const val WAIT_MIN_DWELL_MS = 3500L
        const val FREE_REVERB_MAX = 0.30f     // Model A: basic reverb free ≤30%, studio depth (>30%) is Pro
    }

    // ------------------------------------------------------------------
    //  Analytics seam — single integration point, no-op today.
    //  TODO(analytics): wire PostHog / Firebase here. Keep the call sites;
    //  swap only this body. Must stay cheap: called on edges/actions only.
    // ------------------------------------------------------------------
    private fun track(event: String, detail: String = "") {
        Log.d("NoTapAnalytics", if (detail.isEmpty()) event else "$event {$detail}")
    }

    fun bootEngine() {
        try {
            audioEngine.startEngine()
            audioEngine.setDetectionMode(_uiState.value.modeIndex)
            booted = true
            audioActive = true
            startTelemetryPolling()
            // Offer the previous session's loop back — but never over the tutorial
            // (a first-run user has no autosave anyway; a replayer's funnel is sacred).
            if (!_uiState.value.onboardingActive && autosaveFile().exists()) {
                _uiState.update { it.copy(showRestorePrompt = true) }
            }
            track("app_open", "onboarding=${_uiState.value.onboardingActive}")
        } catch (e: Exception) {
            _uiState.update { it.copy(hasError = true, rawStateString = "ERR: KERNEL") }
        }
    }

    // ------------------------------------------------------------------
    //  App lifecycle (driven from MainActivity onStart/onStop).
    //  Streams open/close with the foreground; the engine + loop persist.
    // ------------------------------------------------------------------
    fun onForeground() {
        if (!booted) { bootEngine(); return }
        if (!audioActive) {
            audioEngine.resumeAudio()      // reconnect streams to the surviving engine/loop
            audioActive = true
            startTelemetryPolling()
            track("audio_resumed")
        }
    }

    fun onBackground() {
        if (!booted || !audioActive) return
        // Golden window: stop polling first, then SYNCHRONOUSLY release the HAL
        // (pauseAudio blocks until close()) — so a swipe-kill after this finds
        // the audio hardware already cleanly freed. Engine + loop stay in memory.
        pollingJob?.cancel()
        // Session autosave — the only thing standing between a back-press/swipe-kill
        // and losing the loop forever. SYNCHRONOUS on purpose: onCleared() (full
        // engine teardown) runs after onStop, so an async writer could race a
        // deleted engine. Saves the FULL layer structure (NTSN: every layer's dry
        // audio + gain/fx/reverb), so restore brings back editable layers, not a
        // flattened mix. An overdub still in progress isn't included (uncommitted).
        // No loop when leaving = the user cleared it → drop the file too.
        val st = _uiState.value.engineState
        if (st == EngineState.LOOPING || st == EngineState.OVERDUBBING) {
            if (audioEngine.saveSession(autosaveFile().absolutePath)) track("session_autosaved")
        } else {
            autosaveFile().delete()
        }
        audioEngine.pauseAudio()
        audioActive = false
        track("audio_paused")
    }

    // ------------------------------------------------------------------
    //  Session restore — an autosaved loop from the previous run is offered
    //  back on boot. NEVER auto-imported: import → instant LOOPING → the app
    //  would open blasting last week's loop unprompted. The user must opt in.
    // ------------------------------------------------------------------
    private fun autosaveFile() =
        java.io.File(getApplication<Application>().filesDir, "autosave_session.ntl")

    fun restoreLastSession() {
        _uiState.update { it.copy(showRestorePrompt = false) }
        // NTSN load: brings back the full layer stack (deletable overdubs, per-layer
        // FX intact) — the layers sheet and FX dashboard re-bind via the poll loop.
        val ok = audioEngine.loadSession(autosaveFile().absolutePath)
        track("session_restored", "ok=$ok")
    }

    /** Declined restore. The file is kept (not deleted): declining is "not now",
     *  and the next background pass overwrites/deletes it by the loop rule anyway. */
    fun dismissRestorePrompt() {
        _uiState.update { it.copy(showRestorePrompt = false) }
        track("session_restore_dismissed")
    }

    private fun startTelemetryPolling() {
        pollingJob?.cancel()
        pollingJob = viewModelScope.launch {
            // לולאת רענון בקצב של כ-60 פריימים בשנייה
            while (isActive) {
                // ONE JNI crossing per frame, zero allocations (the old shape was 6
                // crossings incl. a per-frame NewStringUTF + linear enum search).
                audioEngine.pollTelemetry(telemetryBuffer)
                val rms = telemetryBuffer[0]
                val noiseStdDev = telemetryBuffer[1]
                val transientHit = telemetryBuffer[2] > 0f
                val engineState = EngineState.fromNative(telemetryBuffer[3].toInt())
                val bpm = telemetryBuffer[4]
                val loopBeats = telemetryBuffer[5]
                val loopPos = telemetryBuffer[6]
                val layerCount = telemetryBuffer[7].toInt()
                val denoisedCount = telemetryBuffer[8].toInt()

                // --- edge detection: the ONLY place engine transitions drive logic ---
                if (engineState != prevEngineState) {
                    onEngineTransition(prevEngineState, engineState)
                    prevEngineState = engineState
                }

                // Recording take timer: self-arming inside the poll (single writer) —
                // stamps on the first RECORDING tick, clears on any other state.
                if (engineState == EngineState.RECORDING) {
                    if (recordingStartedAtMs == 0L) recordingStartedAtMs = SystemClock.uptimeMillis()
                } else {
                    recordingStartedAtMs = 0L
                }
                val recElapsedSec =
                    if (recordingStartedAtMs > 0L)
                        ((SystemClock.uptimeMillis() - recordingStartedAtMs) / 1000L).toInt()
                    else -1

                if (_uiState.value.onboardingActive) advanceTutorial(engineState)

                _uiState.update { currentState ->
                    currentState.copy(
                        engineState = engineState,
                        rawStateString = engineState.name,   // enum constant — no per-frame alloc
                        bpm = bpm,
                        loopBeats = loopBeats,
                        loopPosition = loopPos,
                        rms = rms,
                        noiseStdDev = noiseStdDev,
                        transientHit = transientHit,
                        layerCount = layerCount,
                        cleanAllOn = layerCount > 0 && denoisedCount == layerCount,
                        recordingElapsedSec = recElapsedSec,
                        onboardingStage = tutStage,   // projection only — tutStage is the source
                        // The two celebration steps use a Continue button; ELEVATE
                        // auto-completes on the first reverb drag (no button).
                        onboardingCanContinue = currentState.onboardingActive &&
                            (tutStage == OnboardingStage.FIRST_LOOP ||
                             tutStage == OnboardingStage.LAYER_CELEBRATE)
                    )
                }

                // מעטפת הגל: דגימה ב-~2Hz כשיש לופ; ריקון פעם אחת כשאין
                val looping = engineState == EngineState.LOOPING || engineState == EngineState.OVERDUBBING
                if (++frameTick % 30 == 0) {
                    if (looping) {
                        val n = audioEngine.getLoopWaveform(waveBins)
                        _waveform.value = if (n > 0) waveBins.copyOf(n) else FloatArray(0)
                    } else if (_waveform.value.isNotEmpty()) {
                        _waveform.value = FloatArray(0)
                    }
                }

                delay(16) // ~60 FPS
            }
        }
    }

    /** Engine-state → coach stage, for the pre-loop "magic" phase only.
     *  From FIRST_LOOP onward the tutorial is owned by explicit user events. */
    private fun engineMapped(s: EngineState): OnboardingStage = when (s) {
        EngineState.CALIBRATING -> OnboardingStage.LISTEN_ROOM
        EngineState.IDLE -> OnboardingStage.PLUCK
        EngineState.RECORDING -> OnboardingStage.KEEP_PLAYING
        EngineState.PROCESSING -> OnboardingStage.BUILDING
        else -> tutStage
    }

    /** Runs once per poll tick while onboarding is active (main thread only).
     *  HYBRID model: the pre-loop steps ride real ENGINE events (state edges);
     *  the ONLY time component is a 3.5s reading buffer on step 1. The interactive
     *  steps (FIRST_LOOP / LAYER / ELEVATE) NEVER auto-advance here — they wait
     *  indefinitely for an explicit event (onCoachContinue / overdub-commit edge /
     *  reverb interaction). This method only handles the pre-loop mapping and the
     *  "user cleared the loop mid-tutorial" fallback. */
    private fun advanceTutorial(engine: EngineState) {
        val now = SystemClock.uptimeMillis()
        if (stageShownAtMs == 0L) stageShownAtMs = now

        val next: OnboardingStage = when (tutStage) {
            OnboardingStage.LISTEN_ROOM -> {
                val raw = engineMapped(engine)
                // Reading buffer: hold "one quiet second" for 3.5s so it's legible —
                // but only while the engine is merely waiting (IDLE). If the user
                // already plucked (RECORDING+), snap forward: never show "quiet…"
                // over a live recording.
                if (raw == OnboardingStage.PLUCK && now - stageShownAtMs < WAIT_MIN_DWELL_MS)
                    OnboardingStage.LISTEN_ROOM
                else raw
            }
            OnboardingStage.PLUCK,
            OnboardingStage.KEEP_PLAYING,
            OnboardingStage.BUILDING -> engineMapped(engine)

            // Interactive steps: event-driven only. Here we handle nothing but the
            // clear-mid-tutorial fallback (defense — clearing is now blocked during
            // onboarding, so this is rarely reachable, but a loop ending any other
            // way still points back at the play prompt).
            OnboardingStage.FIRST_LOOP,
            OnboardingStage.LAYER,
            OnboardingStage.LAYER_CELEBRATE,
            OnboardingStage.LAYERS_INTRO,
            OnboardingStage.ELEVATE ->
                if (engine == EngineState.IDLE) OnboardingStage.PLUCK else tutStage
        }
        if (next != tutStage) {
            tutStage = next
            stageShownAtMs = now
        }
    }

    /** Explicit "Continue →" / "Finish ✓" tap on the coach card — the event that
     *  advances the two steps with no inherent hardware trigger. FIRST_LOOP → LAYER
     *  on continue; ELEVATE → complete, but only once reverb was actually touched. */
    fun onCoachContinue() {
        // The two celebration steps advance via this tap. ELEVATE auto-completes
        // on the reverb drag, so it has no Continue.
        if (!_uiState.value.onboardingActive) return
        when (tutStage) {
            OnboardingStage.FIRST_LOOP -> {
                tutStage = OnboardingStage.LAYER
                stageShownAtMs = SystemClock.uptimeMillis()
                track("onboarding_layer_shown")
            }
            OnboardingStage.LAYER_CELEBRATE -> {
                tutStage = OnboardingStage.LAYERS_INTRO
                stageShownAtMs = SystemClock.uptimeMillis()
                track("onboarding_layers_intro_shown")
            }
            OnboardingStage.LAYERS_INTRO -> {
                tutStage = OnboardingStage.ELEVATE
                stageShownAtMs = SystemClock.uptimeMillis()
                track("onboarding_elevate_shown")
            }
            else -> {}
        }
    }

    /** Fires once per engine transition, inside the poll coroutine (serialized). */
    private fun onEngineTransition(from: EngineState, to: EngineState) {
        when {
            // onLoopDetected — the engine autonomously decided a real note started
            from == EngineState.IDLE && to == EngineState.RECORDING ->
                track("recording_started")   // TODO(analytics): props: mode, session#

            // onLoopCreated — a loop was assembled and is now playing
            from == EngineState.PROCESSING && to == EngineState.LOOPING -> {
                track("loop_created")        // TODO(analytics): props: length, bpm, mode
                if (!everLooped) {
                    everLooped = true
                    if (_uiState.value.onboardingActive) {
                        // The magic happened — persist immediately so a crash
                        // never replays onboarding; the extended steps are bonus.
                        // FIRST_LOOP now waits for an explicit "Continue" tap.
                        prefs.hasOnboarded = true
                        tutStage = OnboardingStage.FIRST_LOOP
                        stageShownAtMs = SystemClock.uptimeMillis()
                        track("onboarding_first_loop")
                    }
                } else if (_uiState.value.onboardingActive &&
                           tutStage <= OnboardingStage.BUILDING) {
                    // Re-recorded after clearing mid-tutorial — skip the
                    // celebration, go straight back to the layering prompt.
                    tutStage = OnboardingStage.LAYER
                    stageShownAtMs = SystemClock.uptimeMillis()
                }
            }

            // Layer COMMITTED (not merely started) — the overdub round-trip is done.
            // Advance to the celebration step (listen to the stacked loop); the user
            // moves on to ELEVATE only when they tap Continue.
            from == EngineState.OVERDUBBING && to == EngineState.LOOPING -> {
                if (_uiState.value.onboardingActive && tutStage == OnboardingStage.LAYER) {
                    tutStage = OnboardingStage.LAYER_CELEBRATE
                    stageShownAtMs = SystemClock.uptimeMillis()
                    track("onboarding_layer_committed")
                    track("onboarding_layer_celebrate_shown")
                }
            }

            to == EngineState.OVERDUBBING ->
                track("overdub_started")     // TODO(analytics): layer count someday
        }
    }

    // ------------------------------------------------------------------
    //  Onboarding
    // ------------------------------------------------------------------
    fun completeOnboarding(skipped: Boolean = false) {
        if (!_uiState.value.onboardingActive) return
        prefs.hasOnboarded = true
        // No engine touch-up needed on exit: reverb is per-layer and committed only
        // on slider release (already free-clamped there), so there is no live global
        // value to reset. Engine + loop are never touched here.
        _uiState.update { it.copy(onboardingActive = false) }
        track(if (skipped) "onboarding_skipped" else "onboarding_completed")
    }

    /** Manual replay from the help (💡) button — the safety net for users who
     *  missed the flow. Only reachable pre-loop (the button is 💡 only when no
     *  loop exists), so there is never a live loop to contradict the coaching.
     *  Resets the tutorial machine to the top; keeps hasOnboarded persisted. */
    fun replayOnboarding() {
        if (_uiState.value.onboardingActive) return
        tutStage = OnboardingStage.LISTEN_ROOM
        stageShownAtMs = 0L
        reverbTasted = false
        everLooped = false               // let the full first-loop celebration play again
        _uiState.update { it.copy(onboardingActive = true, onboardingStage = OnboardingStage.LISTEN_ROOM) }
        track("onboarding_replayed")
    }

    // ------------------------------------------------------------------
    //  Freemium — basic looping (record/overdub/FLIP/export) is free;
    //  OCT± and reverb are Pro. Gates live HERE so no UI path can bypass them.
    // ------------------------------------------------------------------
    private fun gate(feature: String): Boolean {
        if (_uiState.value.isPro) return true
        _uiState.update { it.copy(showPaywall = true, paywallFeature = feature) }
        track("paywall_shown", "feature=$feature")
        return false
    }

    // ------------------------------------------------------------------
    //  Main FX dashboard = the CURRENT (newest) layer's FX editor. Each new
    //  overdub makes a fresh layer current, so the dashboard resets to it. FLIP
    //  and clearing stay free; OCT± and deep reverb are Pro. Effects apply to the
    //  newest layer ONLY (per-layer, not the whole loop) — the LAYERS sheet edits
    //  any other layer. (The old whole-loop applyLoopEffect/global reverb paths are
    //  no longer driven from the UI.)
    // ------------------------------------------------------------------
    /** Newest layer index (the one the main dashboard edits); -1 if no loop. */
    private fun currentLayerIndex(): Int = _uiState.value.layerCount - 1

    /** Set the current layer's fx (0=none 1=reverse 2=oct-up 3=oct-down). FLIP and
     *  clearing are free; octaves are Pro (paywall + no-op if not Pro). */
    fun setCurrentLayerFx(kind: Int) {
        val idx = currentLayerIndex()
        if (idx < 0) return
        if ((kind == 2 || kind == 3) && !gate(if (kind == 2) "OCTAVE UP" else "OCTAVE DOWN")) return
        audioEngine.setLayerFx(idx, kind)
        track("effect_used", "fx=$kind layer=$idx")
    }

    /** Main reverb slider DRAG — drives only the tutorial completion; the per-layer
     *  bake is costly so it happens on release (commitCurrentLayerReverb). */
    fun onCurrentLayerReverbChange(wet: Float) {
        // ELEVATE tutorial step: the FIRST real reverb move IS the completion event —
        // finish instantly, WITHOUT resetting the value, so the dialed reverb carries
        // seamlessly into the live app.
        if (_uiState.value.onboardingActive && tutStage == OnboardingStage.ELEVATE
            && wet > 0.01f && !reverbTasted) {
            reverbTasted = true
            track("onboarding_reverb_taste")
            completeOnboarding()
        }
    }

    /** Main reverb slider RELEASE — bake the current layer's reverb. Model A freemium:
     *  basic reverb (≤30% wet) is FREE; studio depth is Pro (clamp + paywall). */
    fun commitCurrentLayerReverb(wet: Float) {
        val idx = currentLayerIndex()
        if (idx < 0) return
        if (_uiState.value.isPro) { audioEngine.setLayerReverb(idx, wet); return }
        if (wet <= FREE_REVERB_MAX) {
            audioEngine.setLayerReverb(idx, wet)
        } else {
            audioEngine.setLayerReverb(idx, FREE_REVERB_MAX)
            if (!_uiState.value.showPaywall) gate("STUDIO REVERB")
        }
    }

    fun dismissPaywall() {
        _uiState.update { it.copy(showPaywall = false) }
        track("paywall_dismissed", "feature=${_uiState.value.paywallFeature}")
    }

    /** A pricing card was chosen on the premium screen. productId maps to a
     *  Play Console SKU — at billing time this launches the purchase flow for
     *  that id instead of flipping the stub. */
    fun onPlanSelected(productId: String) {
        track("plan_selected", "id=$productId source=${_uiState.value.paywallFeature}")
        unlockPro()   // TODO(billing): launchBillingFlow(productId) then verify → unlock
    }

    fun unlockPro() {
        // TODO(billing): replace with Play Billing purchase flow + server verify.
        // Placeholder flips the persisted flag so the full UX is testable today.
        prefs.isPro = true
        _uiState.update { it.copy(isPro = true, showPaywall = false) }
        track("pro_unlocked", "source=${_uiState.value.paywallFeature}")
    }

    fun onFeatureRequestTapped() = track("feature_request_tapped")

    // ------------------------------------------------------------------
    //  Strict tutorial funnel — a feature introduced at [introStage] is DISABLED
    //  while the tutorial sits below it (ordinal <), ENABLED at/after it, and
    //  always enabled outside onboarding.
    // ------------------------------------------------------------------
    private fun tutorialAllows(introStage: OnboardingStage): Boolean =
        !_uiState.value.onboardingActive || tutStage >= introStage

    // ------------------------------------------------------------------
    //  Existing actions
    // ------------------------------------------------------------------
    fun handleActionClick() {
        val currentState = _uiState.value.engineState
        val modeIndex = _uiState.value.modeIndex

        when (currentState) {
            // Overdub (orb tap while looping) is gated behind the LAYER stage — a
            // premature tap during FIRST_LOOP would skip the celebration and
            // derail the funnel. Finishing an in-progress overdub stays free.
            EngineState.LOOPING ->
                if (tutorialAllows(OnboardingStage.LAYER)) audioEngine.executeOverdub()
                else track("onboarding_overdub_blocked")
            EngineState.OVERDUBBING -> audioEngine.executeLoop()
            EngineState.IDLE -> if (modeIndex == 1) audioEngine.executeRecordStart()
            EngineState.RECORDING -> if (modeIndex == 1) audioEngine.executeRecordStop()
            else -> {}
        }
    }

    override fun onCleared() {
        super.onCleared()
        pollingJob?.cancel()
        audioEngine.stopEngine()   // full teardown (bridge->stop() is idempotent if already paused)
        booted = false
        audioActive = false
    }

    fun clearCurrentLoop() {
        // Clearing sends the engine to IDLE and would derail the tutorial funnel —
        // disabled until onboarding is skipped or completed. (The UI also hides the
        // clear button during onboarding, so this is defense-in-depth.)
        if (_uiState.value.onboardingActive) { track("onboarding_clear_blocked"); return }
        audioEngine.clearLoop()
        track("loop_cleared")
    }

    /** Multi-track: delete one overdub layer by index (1-based over the base; index
     *  0 is the protected base and the engine ignores it). FREE feature — deleting a
     *  layer is "undo-any" hygiene, not a Pro effect. Tutorial gating follows the
     *  standard funnel rule: live from the stage that introduces the layers sheet
     *  (LAYERS_INTRO) — a blanket onboarding block would leave the sheet's 🗑
     *  buttons visibly dead during the very step that teaches them. Deleting an
     *  overdub keeps the base playing, so the funnel survives it. */
    fun deleteOverdub(index: Int) {
        if (!tutorialAllows(OnboardingStage.LAYERS_INTRO)) { track("onboarding_layer_delete_blocked"); return }
        if (index < 1 || index >= _uiState.value.layerCount) return
        audioEngine.deleteLayer(index)
        track("layer_deleted", "index=$index")
    }

    // ------------------------------------------------------------------
    //  Per-layer effects (Pro). Applying an effect to a single layer is a
    //  studio feature — gated behind the paywall. The setters no-op when not
    //  Pro (the layers sheet shows an upsell banner instead of live controls,
    //  so these are defense-in-depth); layerFxUpsell() raises the paywall.
    //  fx kind: 0=none 1=reverse 2=octave-up 3=octave-down.
    // ------------------------------------------------------------------
    fun setLayerFx(index: Int, kind: Int) {
        if (!_uiState.value.isPro || index < 0 || index >= _uiState.value.layerCount) return
        audioEngine.setLayerFx(index, kind)
        track("layer_fx", "index=$index kind=$kind")
    }

    fun setLayerGain(index: Int, gain: Float) {
        if (!_uiState.value.isPro || index < 0 || index >= _uiState.value.layerCount) return
        audioEngine.setLayerGain(index, gain)
    }

    fun setLayerReverb(index: Int, wet: Float) {
        if (!_uiState.value.isPro || index < 0 || index >= _uiState.value.layerCount) return
        audioEngine.setLayerReverb(index, wet)
    }

    fun layerFxUpsell() { gate("PER-LAYER FX") }

    /** CLEAN (Pro): toggle the spectral noise gate on ALL layers. Room noise stacks
     *  with every overdub (5 layers ≈ +7dB hiss) — this pulls it back out, music-safe
     *  and fully reversible (each layer's pristine dry is kept). Enabling is gated;
     *  disabling is always allowed (never trap a user inside a Pro state). */
    fun toggleCleanAll() {
        val s = _uiState.value
        if (s.layerCount < 1) return
        val turningOn = !s.cleanAllOn
        if (turningOn && !gate("NOISE CLEANUP")) return
        audioEngine.setDenoiseAll(turningOn)
        track("clean_all", "on=$turningOn layers=${s.layerCount}")
    }

    // Read-throughs so the sheet can seed its controls with each layer's live
    // state (the engine mirrors fx/gain/reverb per layer for exactly this).
    fun layerFx(index: Int): Int = audioEngine.getLayerFx(index)
    fun layerGain(index: Int): Float = audioEngine.getLayerGain(index)
    fun layerReverb(index: Int): Float = audioEngine.getLayerReverb(index)

    fun exportLoop(filePath: String): Boolean {
        track("export_tapped")   // TODO(analytics): success/failure props
        return audioEngine.exportLoopWav(filePath)
    }

    fun importLoop(filePath: String): Boolean {
        track("import_tapped")
        return audioEngine.importLoopWav(filePath)
    }

    // הוספנו פונקציה לבחירה ישירה של מצב מהדיאלוג החדש
    fun setDetectionModeDirectly(index: Int) {
        // The whole tutorial teaches the AUTO ("NoTap") magic — switching to
        // TAP/SYNC mid-lesson breaks its physical assumptions. Frozen during
        // onboarding (the UI also dims the switcher to signal the lock).
        if (_uiState.value.onboardingActive) { track("onboarding_mode_switch_blocked"); return }
        _uiState.update { it.copy(modeIndex = index, modeName = modes[index]) }
        audioEngine.setDetectionMode(index)
        track("mode_selected", "mode=${modes[index]}")
    }

    fun adjustTargetBpm(delta: Float) {
        _uiState.update { state ->
            val newBpm = (state.targetBpm + delta).coerceIn(40f, 300f) // חסימה גבולית הגיונית למוזיקה
            audioEngine.setTargetBPM(newBpm)
            state.copy(targetBpm = newBpm)
        }
    }

    fun setAbsoluteTargetBpm(bpm: Float) {
        _uiState.update { state ->
            val newBpm = bpm.coerceIn(40f, 300f) // גבולות גזרה פיזיקליים למוזיקה
            audioEngine.setTargetBPM(newBpm)
            state.copy(targetBpm = newBpm)
        }
    }

    fun toggleMetronome() {
        _uiState.update { state ->
            val next = !state.metronomeEnabled
            audioEngine.setMetronomeEnabled(next)
            state.copy(metronomeEnabled = next)
        }
    }
}
