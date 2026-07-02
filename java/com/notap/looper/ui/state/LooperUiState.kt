package com.notap.looper.ui.state

import com.notap.looper.EngineState

data class LooperUiState(
    val engineState: EngineState = EngineState.UNKNOWN,
    val rawStateString: String = "BOOTING",
    val modeIndex: Int = 0,
    val modeName: String = "AUTO SILENCE",
    val bpm: Float = 0f,
    val loopPosition: Float = 0f,
    val rms: Float = 0f,
    val noiseStdDev: Float = 0f,
    val transientHit: Boolean = false,
    val hasError: Boolean = false,
    val targetBpm: Float = 120f

)