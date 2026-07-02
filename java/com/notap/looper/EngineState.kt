package com.notap.looper

enum class EngineState {
    IDLE, RECORDING, PROCESSING, LOOPING, OVERDUBBING, CALIBRATING, UNKNOWN;

    companion object {
        fun fromString(state: String?): EngineState {
            return entries.find { it.name == state } ?: UNKNOWN
        }
    }
}