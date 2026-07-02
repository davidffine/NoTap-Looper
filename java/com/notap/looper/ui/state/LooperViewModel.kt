package com.notap.looper.ui.state

import androidx.lifecycle.ViewModel
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

class LooperViewModel : ViewModel() {

    private val audioEngine = AudioEngine()
    private val modes = arrayOf("AUTO SILENCE", "TAP & TRIM", "FIXED BPM")

    private val _uiState = MutableStateFlow(LooperUiState())
    val uiState: StateFlow<LooperUiState> = _uiState.asStateFlow()

    private var pollingJob: Job? = null
    private val telemetryBuffer = FloatArray(3)

    fun bootEngine() {
        try {
            audioEngine.startEngine()
            audioEngine.setDetectionMode(_uiState.value.modeIndex)
            startTelemetryPolling()
        } catch (e: Exception) {
            _uiState.update { it.copy(hasError = true, rawStateString = "ERR: KERNEL") }
        }
    }

    private fun startTelemetryPolling() {
        pollingJob?.cancel()
        pollingJob = viewModelScope.launch {
            // לולאת רענון בקצב של כ-60 פריימים בשנייה
            while (isActive) {
                audioEngine.pollTelemetry(telemetryBuffer)
                val rms = telemetryBuffer[0]
                val noiseStdDev = telemetryBuffer[1]
                val transientHit = telemetryBuffer[2] > 0f

                val rawState = audioEngine.getCurrentState()
                val engineState = EngineState.fromString(rawState)
                val bpm = audioEngine.getEstimatedBPM()
                val loopPos = audioEngine.getLoopPosition()

                _uiState.update { currentState ->
                    currentState.copy(
                        engineState = engineState,
                        rawStateString = rawState,
                        bpm = bpm,
                        loopPosition = loopPos,
                        rms = rms,
                        noiseStdDev = noiseStdDev,
                        transientHit = transientHit
                    )
                }
                delay(16) // ~60 FPS
            }
        }
    }

    fun cycleDetectionMode(direction: Int) {
        val newIndex = (_uiState.value.modeIndex + direction + modes.size) % modes.size
        _uiState.update { it.copy(modeIndex = newIndex, modeName = modes[newIndex]) }
        audioEngine.setDetectionMode(newIndex)
    }

    fun handleActionClick() {
        val currentState = _uiState.value.engineState
        val modeIndex = _uiState.value.modeIndex

        when (currentState) {
            EngineState.LOOPING -> audioEngine.executeOverdub()
            EngineState.OVERDUBBING -> audioEngine.executeLoop()
            EngineState.IDLE -> if (modeIndex == 1) audioEngine.executeRecordStart()
            EngineState.RECORDING -> if (modeIndex == 1) audioEngine.executeRecordStop()
            else -> {}
        }
    }

    override fun onCleared() {
        super.onCleared()
        pollingJob?.cancel()
        audioEngine.stopEngine()
    }

    fun clearCurrentLoop() {
        audioEngine.clearLoop()
        // ניתן גם לאפס סטייטים ספציפיים אם רוצים, אבל המנוע כבר יעביר אותנו ל-IDLE
    }

    fun exportLoop(filePath: String): Boolean {
        return audioEngine.exportLoopWav(filePath)
    }

    fun importLoop(filePath: String): Boolean {
        return audioEngine.importLoopWav(filePath)
    }

    // הוספנו פונקציה לבחירה ישירה של מצב מהדיאלוג החדש
    fun setDetectionModeDirectly(index: Int) {
        _uiState.update { it.copy(modeIndex = index, modeName = modes[index]) }
        audioEngine.setDetectionMode(index)
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
}