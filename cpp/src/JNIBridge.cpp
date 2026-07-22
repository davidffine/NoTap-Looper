#ifdef __ANDROID__
#include <jni.h>
#include <string>
#include "../headers/LooperEngine.hpp"
#include "../headers/OboeBridge.hpp"

static LooperEngine* g_engine = nullptr;
static OboeLooperEngine* g_oboe_bridge = nullptr;

extern "C" {

JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_startEngine(JNIEnv *env, jobject thiz) {
    if (!g_engine) {
        g_engine = new LooperEngine();
        g_oboe_bridge = new OboeLooperEngine(*g_engine);
        g_oboe_bridge->start();
    }
}

JNIEXPORT void JNICALL Java_com_notap_looper_AudioEngine_executeRecordStart(JNIEnv *env, jobject thiz) {
    if (g_engine) {
        g_engine->execute_record_start_command();
    }
}

JNIEXPORT void JNICALL Java_com_notap_looper_AudioEngine_executeRecordStop(JNIEnv *env, jobject thiz) {
    if (g_engine) {
        g_engine->execute_record_stop_command();
    }
}

// חציית-JNI *יחידה* לכל פריים של לולאת ה-60fps: כל מה שה-UI צריך פר-טיק נארז
// למערך float אחד, ללא שום הקצאת אובייקט. (בעבר: 6 חציות/פריים, אחת מהן
// NewStringUTF — הקצאת String פר-פריים = ‎~5M הקצאות ליום שימוש, זבל GC בלולאת
// הרנדר.) חוזה האינדקסים (מראה מדויקת ב-AudioEngine.kt/LooperViewModel):
//   [0]=rms · [1]=noise_std · [2]=transient(0/1) · [3]=state ordinal (חוזה
//   LooperState: 0=CAL 1=IDLE 2=REC 3=PROC 4=LOOP 5=OVERDUB) · [4]=bpm ·
//   [5]=loop_beats · [6]=loop_pos(0..1) · [7]=layer_count · [8]=denoised_count ·
//   [9]=transport_stopped(0/1) · [10]=output_muted(0/1) · [11]=closure_progress(0..1) ·
//   [12]=overdub_armed(0/1) · [13]=undo_available(0/1) · [14]=count_in_beats_left ·
//   [15]=layer_mute_mask (bitfield, exact in float32 up to 2^24) · [16]=loop_seconds
//
// 9-11 הם *מראה* של מצב שה-UI עצמו כתב — ובכל זאת נקראים מהמנוע ולא נזכרים
// ב-Kotlin: ה-Worker מאפס טרנספורט בכל לופ חדש (clear/import/session/בסיס),
// וזיכרון מקומי ב-UI היה נשאר תקוע על "עצור" מול מנוע שכבר מנגן.
JNIEXPORT void JNICALL Java_com_notap_looper_AudioEngine_pollTelemetry(JNIEnv *env, jobject thiz, jfloatArray outData) {
    if (!g_engine || outData == nullptr) return;
    if (env->GetArrayLength(outData) < 17) return;

    jfloat telemetry[17];
    telemetry[0] = g_engine->current_rms_.load(std::memory_order_relaxed);
    telemetry[1] = g_engine->current_noise_std_dev_.load(std::memory_order_relaxed);
    // קריאה ואיפוס אטומי של דגל הטרנזיינט
    telemetry[2] = g_engine->transient_hit_flag_.exchange(false, std::memory_order_relaxed) ? 1.0f : 0.0f;
    telemetry[3] = static_cast<jfloat>(static_cast<int>(g_engine->get_current_state()));
    telemetry[4] = g_engine->get_estimated_bpm();
    telemetry[5] = g_engine->get_loop_beats();
    telemetry[6] = g_engine->get_loop_position();
    telemetry[7] = static_cast<jfloat>(g_engine->get_layer_count());
    telemetry[8] = static_cast<jfloat>(g_engine->get_layer_denoise_count());
    telemetry[9] = g_engine->is_transport_stopped() ? 1.0f : 0.0f;
    telemetry[10] = g_engine->is_output_muted() ? 1.0f : 0.0f;
    telemetry[11] = g_engine->get_closure_progress();
    telemetry[12] = g_engine->is_overdub_armed() ? 1.0f : 0.0f;
    telemetry[13] = g_engine->is_undo_available() ? 1.0f : 0.0f;
    telemetry[14] = static_cast<jfloat>(g_engine->get_count_in_beats_left());
    telemetry[15] = static_cast<jfloat>(g_engine->get_layer_mute_mask());
    telemetry[16] = g_engine->get_loop_seconds();

    // כתיבה חזרה למערך של Kotlin ללא הקצאות זיכרון חדשות
    env->SetFloatArrayRegion(outData, 0, 17, telemetry);
}

// מסכת האזנה (Solo/Mute) — כתיבה אטומית אחת לכל האצווה.
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setLayerMuteMask(JNIEnv *env, jobject thiz, jint mask) {
    if (g_engine) g_engine->set_layer_mute_mask(static_cast<uint32_t>(mask));
}

// --- ביטול מחיקת שכבה (עומק 1) ---
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_undoDeleteLayer(JNIEnv *env, jobject thiz) {
    if (g_engine) g_engine->undo_delete_layer();
}

// --- ספירה-לתוך (SYNC) ---
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_startCountIn(JNIEnv *env, jobject thiz) {
    if (g_engine) g_engine->start_count_in();
}

JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setCountInBars(JNIEnv *env, jobject thiz, jfloat bars) {
    if (g_engine) g_engine->set_count_in_bars(bars);
}

JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_cancelCountIn(JNIEnv *env, jobject thiz) {
    if (g_engine) g_engine->cancel_count_in();
}

// --- הצהרה בדיעבד על אורך הלופ בתיבות ---
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setLoopBars(JNIEnv *env, jobject thiz, jfloat bars) {
    if (g_engine) g_engine->set_loop_bars(bars);
}

// --- למידה מחדש של רעש החדר ---
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_recalibrateRoom(JNIEnv *env, jobject thiz) {
    if (g_engine) g_engine->request_recalibrate();
}

// --- טרנספורט: עצירה (שומרת לופ) והשתקה. הכפתור ההרסני היחיד היה ✕ עד כה. ---
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setTransportStopped(JNIEnv *env, jobject thiz, jboolean stopped) {
    if (g_engine) g_engine->set_transport_stopped(stopped == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setOutputMuted(JNIEnv *env, jobject thiz, jboolean muted) {
    if (g_engine) g_engine->set_output_muted(muted == JNI_TRUE);
}

// עוצמת ניטור (Master) 0..1 — משפיעה על מה ששומעים בלבד. הייצוא והשכבות
// נשארים ברמה מלאה, ולכן "לנגן בשקט ב-23:00" לא מחליש את ההקלטה.
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setMasterVolume(JNIEnv *env, jobject thiz, jfloat volume) {
    if (g_engine) g_engine->set_master_volume(volume);
}

// סגירה-אוטומטית של אוברדאב אחרי סיבוב אחד (ברירת מחדל: דלוק).
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setOverdubAutoClose(JNIEnv *env, jobject thiz, jboolean on) {
    if (g_engine) g_engine->set_overdub_auto_close(on == JNI_TRUE);
}

// הצמדת תחילת האוברדאב לראש הלופ (ברירת מחדל: דלוק) — מוציאה את תזמון הנקישה
// מהמשוואה.
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setOverdubQuantize(JNIEnv *env, jobject thiz, jboolean on) {
    if (g_engine) g_engine->set_overdub_quantize(on == JNI_TRUE);
}

// --- כוונון זיהוי שנחשף למשתמש ---
// שני הכוונונים היחידים שנחשפים: כמה זמן להמתין לפני סגירה (סבלנות) וכמה גבוה
// לשים את רצפת השקט (רגישות). שאר הכוונונים נשארים פנימיים — הם מכוילים מול
// הקורפוס ואין למשתמש דרך להעריך אותם.
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setSilenceHoldSeconds(JNIEnv *env, jobject thiz, jfloat sec) {
    if (g_engine) g_engine->set_silence_hold_seconds(sec);
}

JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setSilenceRelMult(JNIEnv *env, jobject thiz, jfloat mult) {
    if (g_engine) g_engine->set_silence_rel_mult(mult);
}

// מסלול הסגירה האיטי (היעדר-פעילות) — זה שמכריע על זנב-צלצול/ריברב. "סבלנות"
// שמזיזה רק את מסלול השקט המהיר לא משנה דבר בדיוק במקרה שבגללו המשתמש נגע בה.
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setActivityHoldSeconds(JNIEnv *env, jobject thiz, jfloat sec) {
    if (g_engine) g_engine->set_activity_hold_seconds(sec);
}

// תצפית: סה"כ דגימות-כניסה שנשמטו (תור מלא). ~0 תמיד; חשיפה לקצה-מקרה פתולוגי.
// uint32 מורחב ל-jlong כדי להימנע מסימן שלילי בתצוגה.
JNIEXPORT jlong JNICALL
Java_com_notap_looper_AudioEngine_getInputOverrunCount(JNIEnv *env, jobject thiz) {
    return g_engine ? static_cast<jlong>(g_engine->get_input_overrun_count()) : 0;
}

// פירוק חומרה מלא — נקרא רק ביציאה אמיתית (onCleared/onDestroy).
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_stopEngine(JNIEnv *env, jobject thiz) {
    if (g_oboe_bridge) {
        g_oboe_bridge->stop();
        delete g_oboe_bridge;
        g_oboe_bridge = nullptr;
    }
    if (g_engine) {
        delete g_engine;
        g_engine = nullptr;
    }
}

// "חלון הזהב" של onStop(): משחרר את זרמי ה-Oboe (וה-HAL) *סינכרונית* אך משאיר
// את ה-LooperEngine ואת חוצץ הלופ חיים בזיכרון. אם המשתמש רק מ-backgrounds
// וחוזר — resumeAudio יחבר זרמים חדשים לאותו מנוע/לופ. אם הוא עושה Swipe-kill,
// ה-HAL כבר שוחרר נקי לפני מוות התהליך, והזיכרון נאסף על-ידי מות התהליך.
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_pauseAudio(JNIEnv *env, jobject thiz) {
    if (g_oboe_bridge) g_oboe_bridge->stop();   // requestStop()+close() חוסמים — חוזר רק אחרי שחרור
}

// חיבור-מחדש של הזרמים אל המנוע הקיים (start() אידמפוטנטי — לא יפתח כפילות).
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_resumeAudio(JNIEnv *env, jobject thiz) {
    if (g_oboe_bridge) g_oboe_bridge->start();
}

JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_executeOverdub(JNIEnv *env, jobject thiz) {
    if (g_engine) {
        g_engine->execute_overdub_command();
    }
}

JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_executeLoop(JNIEnv *env, jobject thiz) {
    if (g_engine) {
        g_engine->execute_loop_command();
    }
}

JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_clearLoop(JNIEnv *env, jobject thiz) {
    if (g_engine) {
        g_engine->execute_clear_command();
    }
}

// רב-מסלול: מחיקת שכבת-אוברדאב לפי אינדקס (>=1; 0=בסיס מוגן).
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_deleteLayer(JNIEnv *env, jobject thiz, jint index) {
    if (g_engine) {
        g_engine->delete_layer(static_cast<int>(index));
    }
}

// CLEAN (שער ספקטרלי, Pro): הדלקה/כיבוי על כל השכבות בבת-אחת.
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setDenoiseAll(JNIEnv *env, jobject thiz, jboolean on) {
    if (g_engine) g_engine->set_denoise_all(on == JNI_TRUE);
}

// --- אפקטים פר-שכבה (פייז 3) ---
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setLayerFx(JNIEnv *env, jobject thiz, jint index, jint kind) {
    if (g_engine) g_engine->set_layer_fx(static_cast<int>(index), static_cast<int>(kind));
}
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setLayerGain(JNIEnv *env, jobject thiz, jint index, jfloat gain) {
    if (g_engine) g_engine->set_layer_gain(static_cast<int>(index), gain);
}
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setLayerReverb(JNIEnv *env, jobject thiz, jint index, jfloat wet) {
    if (g_engine) g_engine->set_layer_reverb(static_cast<int>(index), wet);
}
JNIEXPORT jint JNICALL
Java_com_notap_looper_AudioEngine_getLayerFx(JNIEnv *env, jobject thiz, jint index) {
    return g_engine ? g_engine->get_layer_fx(static_cast<int>(index)) : 0;
}
JNIEXPORT jfloat JNICALL
Java_com_notap_looper_AudioEngine_getLayerGain(JNIEnv *env, jobject thiz, jint index) {
    return g_engine ? g_engine->get_layer_gain(static_cast<int>(index)) : 1.0f;
}
JNIEXPORT jfloat JNICALL
Java_com_notap_looper_AudioEngine_getLayerReverb(JNIEnv *env, jobject thiz, jint index) {
    return g_engine ? g_engine->get_layer_reverb(static_cast<int>(index)) : 0.0f;
}

// [חדש] העברת הפקודה לתוך הפיזיקה
JNIEXPORT void JNICALL
Java_com_notap_looper_AudioEngine_setDetectionMode(JNIEnv *env, jobject thiz, jint mode) {
    if (g_engine) {
        g_engine->set_detection_mode(mode);
    }
}

// (getCurrentState/getEstimatedBPM/getLoopBeats/getLoopPosition/getLayerCount
// נמחקו: כולם אוחדו לתוך pollTelemetry — חציית-JNI אחת פר-פריים, אפס הקצאות.
// getCurrentState היה הגרוע מכולם: NewStringUTF פר-פריים ב-60fps.)

JNIEXPORT jboolean JNICALL Java_com_notap_looper_AudioEngine_exportLoopWav(JNIEnv *env, jobject thiz, jstring path) {
    if (!g_engine) return false;
    const char *nativeString = env->GetStringUTFChars(path, 0);
    bool result = g_engine->export_to_wav(nativeString);
    env->ReleaseStringUTFChars(path, nativeString);
    return result;
}

JNIEXPORT jboolean JNICALL Java_com_notap_looper_AudioEngine_importLoopWav(JNIEnv *env, jobject thiz, jstring path) {
    if (!g_engine) return false;
    const char *nativeString = env->GetStringUTFChars(path, 0);
    bool result = g_engine->import_from_wav(nativeString);
    env->ReleaseStringUTFChars(path, nativeString);
    return result;
}

// סשן רב-מסלולי (NTSN): שמירה/שחזור של כל השכבות כולל פרמטרי fx/gain/reverb.
// save חוסם עד השלמת הכתיבה (ראה save_session); load = מסירה בסגנון Import.
JNIEXPORT jboolean JNICALL Java_com_notap_looper_AudioEngine_saveSession(JNIEnv *env, jobject thiz, jstring path) {
    if (!g_engine) return false;
    const char *nativeString = env->GetStringUTFChars(path, 0);
    bool result = g_engine->save_session(nativeString);
    env->ReleaseStringUTFChars(path, nativeString);
    return result;
}

JNIEXPORT jboolean JNICALL Java_com_notap_looper_AudioEngine_loadSession(JNIEnv *env, jobject thiz, jstring path) {
    if (!g_engine) return false;
    const char *nativeString = env->GetStringUTFChars(path, 0);
    bool result = g_engine->load_session(nativeString);
    env->ReleaseStringUTFChars(path, nativeString);
    return result;
}

JNIEXPORT void JNICALL Java_com_notap_looper_AudioEngine_setTargetBPM(JNIEnv *env, jobject thiz, jfloat bpm) {
    if (g_engine) {
        g_engine->set_target_bpm(bpm);
    }
}

JNIEXPORT void JNICALL Java_com_notap_looper_AudioEngine_setMetronomeEnabled(JNIEnv *env, jobject thiz, jboolean on) {
    if (g_engine) {
        g_engine->set_metronome_enabled(on == JNI_TRUE);
    }
}

// (applyLoopEffect / setReverbWet נמחקו: הנתיב הגלובלי מת — כל האפקטים והריברב
// הם פר-שכבה דרך setLayerFx/setLayerGain/setLayerReverb.)

JNIEXPORT jint JNICALL Java_com_notap_looper_AudioEngine_getLoopWaveform(JNIEnv *env, jobject thiz, jfloatArray out) {
    if (!g_engine || out == nullptr) return 0;
    jsize len = env->GetArrayLength(out);
    if (len <= 0) return 0;
    jfloat* buf = env->GetFloatArrayElements(out, nullptr);
    int bins = g_engine->get_loop_waveform(buf, static_cast<int>(len));
    env->ReleaseFloatArrayElements(out, buf, 0);
    return bins;
}

}
#endif