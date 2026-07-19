package com.notap.looper

class AudioEngine {

    init {
        System.loadLibrary("looper_core")
    }

    external fun startEngine()
    external fun stopEngine()

    // Lifecycle: release/reconnect the Oboe streams (and HAL) WITHOUT destroying
    // the engine or the recorded loop. pauseAudio() blocks until the streams are
    // fully closed (the onStop "golden window" before a swipe-kill).
    external fun pauseAudio()
    external fun resumeAudio()

    external fun executeRecordStart()
    external fun executeRecordStop()

    // חציית-JNI יחידה פר-פריים (60fps), אפס הקצאות. המערך חייב ≥9 תאים:
    //   [0]=rms · [1]=noiseStd · [2]=transient(0/1) · [3]=state ordinal
    //   (EngineState.fromNative) · [4]=bpm · [5]=loopBeats · [6]=loopPos(0..1) ·
    //   [7]=layerCount · [8]=denoisedLayerCount. חוזה האינדקסים ממורא ב-JNIBridge.cpp.
    external fun pollTelemetry(outData: FloatArray)

    // CLEAN (שער ספקטרלי, Pro): מדליק/מכבה ניקוי-רעש על כל השכבות בבת-אחת.
    // הפיך לחלוטין (ה-dry הפריסטיני נשמר); מצב נוכחי דרך pollTelemetry[8].
    external fun setDenoiseAll(on: Boolean)

    // תצפית: סה"כ דגימות-כניסה שנשמטו (תור לכידה מלא). ~0 תמיד — חשיפה לקצה-מקרה
    // פתולוגי (המנוע לא עמד בקצב > 5 שניות). קריאה על-פי-דרישה, לא בלולאת ה-60fps.
    external fun getInputOverrunCount(): Long

    external fun executeOverdub()
    external fun executeLoop()
    external fun clearLoop()

    // רב-מסלול: מחיקת שכבת-אוברדאב לפי אינדקס (>=1; 0=בסיס מוגן). מס' השכבות
    // מגיע דרך pollTelemetry[7].
    external fun deleteLayer(index: Int)

    // אפקטים פר-שכבה (פייז 3): fx (0=none 1=reverse 2=oct+ 3=oct−), gain (0..2),
    // reverb (0..1). ה-set רושם פקודה; ה-get קורא את מצב-המראה שה-Worker מפרסם.
    external fun setLayerFx(index: Int, kind: Int)
    external fun setLayerGain(index: Int, gain: Float)
    external fun setLayerReverb(index: Int, wet: Float)
    external fun getLayerFx(index: Int): Int
    external fun getLayerGain(index: Int): Float
    external fun getLayerReverb(index: Int): Float

    // [חדש] פקודה להגדרת האונטולוגיה של הזיהוי
    // 0 = Auto Silence | 1 = Tap & Trim | 2 = Fixed BPM
    external fun setDetectionMode(mode: Int)

    // (getCurrentState/getEstimatedBPM/getLoopBeats/getLoopPosition נמחקו —
    // אוחדו לתוך pollTelemetry; getCurrentState אף הקצה String פר-פריים.)
    external fun exportLoopWav(absolutePath: String): Boolean
    external fun importLoopWav(absolutePath: String): Boolean

    // סשן רב-מסלולי (NTSN): שומר/משחזר את *כל* השכבות (dry + gain/fx/reverb) —
    // שחזור מלא של מבנה השכבות, לא מיקס משוטח. save חוסם עד גמר הכתיבה (בטוח
    // לקרוא מ-onStop, לפני teardown); load = מסירה א-סינכרונית בסגנון import.
    external fun saveSession(absolutePath: String): Boolean
    external fun loadSession(absolutePath: String): Boolean

    external fun setTargetBPM(bpm: Float)
    external fun setMetronomeEnabled(on: Boolean)

    // תצוגת גל (מעטפת הלופ הפעיל, נדגם ~2Hz)
    external fun getLoopWaveform(out: FloatArray): Int
}