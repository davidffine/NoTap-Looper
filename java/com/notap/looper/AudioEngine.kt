package com.notap.looper

class AudioEngine {

    init {
        System.loadLibrary("looper_core")
    }

    external fun startEngine()
    external fun stopEngine()

    external fun executeRecordStart()
    external fun executeRecordStop()

    external fun pollTelemetry(outData: FloatArray)

    external fun executeOverdub()
    external fun executeLoop()
    external fun clearLoop()

    // [חדש] פקודה להגדרת האונטולוגיה של הזיהוי
    // 0 = Auto Silence | 1 = Tap & Trim | 2 = Fixed BPM
    external fun setDetectionMode(mode: Int)

    external fun getEstimatedBPM(): Float
    external fun getLoopPosition(): Float
    external fun getCurrentState(): String
    external fun exportLoopWav(absolutePath: String): Boolean
    external fun importLoopWav(absolutePath: String): Boolean

    external fun setTargetBPM(bpm: Float)
}