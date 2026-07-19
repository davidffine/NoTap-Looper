package com.notap.looper.ui.state

import com.notap.looper.EngineState

/**
 * Stages of the first-run "magic minute": ride the REAL engine state machine
 * to the user's first hands-free loop. No faked demo — each stage is derived
 * from actual engine transitions inside the ViewModel's single poll coroutine.
 */
enum class OnboardingStage {
    LISTEN_ROOM,   // engine CALIBRATING — "learning your room" (min-dwell 3.5s, presentation only)
    PLUCK,         // engine IDLE — "just play something"
    KEEP_PLAYING,  // engine RECORDING — "stop when you're done"
    BUILDING,      // engine PROCESSING — "finding the beat"
    FIRST_LOOP,    // first LOOPING — celebrate, wait for Continue tap
    LAYER,         // guide the overdub; advances when a layer is COMMITTED
    LAYER_CELEBRATE, // layer committed — "listen to it blend", wait for Continue tap
    LAYERS_INTRO,  // reveal the LAYERS mixer — delete any overdub / per-layer FX (Pro)
    ELEVATE        // guide the reverb; auto-completes on the first reverb drag
}

data class LooperUiState(
    val engineState: EngineState = EngineState.UNKNOWN,
    val rawStateString: String = "BOOTING",
    val modeIndex: Int = 0,
    val modeName: String = "AUTO SILENCE",
    val bpm: Float = 0f,
    val loopBeats: Float = 0f,
    val loopPosition: Float = 0f,
    val rms: Float = 0f,
    val noiseStdDev: Float = 0f,
    val transientHit: Boolean = false,
    val hasError: Boolean = false,

    // Multi-track: number of live layers (base + overdubs). 0 = no loop, 1 = base
    // only (nothing deletable), ≥2 means there are overdubs that can be deleted.
    val layerCount: Int = 0,

    // Recording elapsed-time readout: whole seconds since the take started, or -1
    // when not RECORDING. The tempo slot shows it live (the number IS the future
    // loop length; a runaway count also exposes a non-closing AUTO take early).
    val recordingElapsedSec: Int = -1,

    // CLEAN (spectral gate, Pro): true when every live layer is denoised — the
    // CLEAN pill's lit state. Derived per-tick from the engine mirror.
    val cleanAllOn: Boolean = false,
    val targetBpm: Float = 120f,
    val metronomeEnabled: Boolean = true,

    // --- first-time onboarding (UI-only; poll loop's copy() carries these through) ---
    val onboardingActive: Boolean = false,
    val onboardingStage: OnboardingStage = OnboardingStage.LISTEN_ROOM,
    // true when the current stage's advance condition is met and the coach should
    // show a Continue/Finish affordance (FIRST_LOOP always; ELEVATE after reverb touched)
    val onboardingCanContinue: Boolean = false,

    // --- freemium (persisted via Prefs; mirrored here for binding) ---
    val isPro: Boolean = false,
    val showPaywall: Boolean = false,
    val paywallFeature: String = "",

    // Session restore: an autosaved loop from the previous run exists — offer it
    // back on boot (opt-in; restoring starts playback, so never automatic).
    val showRestorePrompt: Boolean = false
)
