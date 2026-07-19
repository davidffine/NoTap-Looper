package com.notap.looper

enum class EngineState {
    IDLE, RECORDING, PROCESSING, LOOPING, OVERDUBBING, CALIBRATING, UNKNOWN;

    companion object {
        /** Native ordinal contract — MUST mirror C++ `enum class LooperState`
         *  (LooperEngine.hpp): 0=CALIBRATING 1=IDLE 2=RECORDING 3=PROCESSING
         *  4=LOOPING 5=OVERDUBBING. Delivered per-frame as pollTelemetry[3];
         *  anything out of range (engine not booted yet) maps to UNKNOWN.
         *  Explicit table on purpose: this enum's own ordinal order differs,
         *  and a silent reorder on either side must not corrupt the mapping. */
        fun fromNative(ordinal: Int): EngineState = when (ordinal) {
            0 -> CALIBRATING
            1 -> IDLE
            2 -> RECORDING
            3 -> PROCESSING
            4 -> LOOPING
            5 -> OVERDUBBING
            else -> UNKNOWN
        }
    }
}
