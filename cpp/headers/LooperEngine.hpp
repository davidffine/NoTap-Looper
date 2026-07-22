#pragma once

#include "LockFreeRingBuffer.hpp"
#include "../includes/kissfft/kiss_fftr.h"   // FFT scratch pre-allocated in the ctor (novelty curve)
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <algorithm>

enum class LooperState {
    CALIBRATING,
    IDLE,
    RECORDING,
    PROCESSING,
    LOOPING,
    OVERDUBBING
};

std::string state_to_string(LooperState state);

// FTZ/DAZ הם מצב רגיסטר FP *פר-Thread*. חייב להיקרא על כל Thread שמריץ DSP:
// ה-Worker קורא לזה בעצמו; ה-Bridge של Oboe חייב לקרוא לזה על Thread הקולבק
// (הריברב — רשת משוב דועכת — הוא מחולל הדנורמלים הקלאסי, ורץ על Thread הפלט).
void enable_denormal_flush_to_zero();

struct EngineConfig {
    int sample_rate = 44100;
    int chunk_size = 512;
    float preroll_seconds = 0.15f;
};

// ברך-רכה על סכום *כלשהו*: לינארי לחלוטין עד KNEE, דחיסה אסימפטוטית ל-±1.0 מעליו.
// מקור-אמת יחיד לדחיסת המיקס — משמש גם את מיקס-האוברדאב הדו-אופרנדי וגם את
// חיבור ה-N שכבות (רב-מסלול). הנוסחה הקודמת x/(1+|x|) דחסה *בכל* עוצמה — כל
// מעבר אוברדאב הנחית את הלופ הקיים (0.3→0.23) גם עם כניסה שקטה.
inline float soft_clip_knee(float sum) {
    const float KNEE = 0.9f;
    float mag = std::abs(sum);
    if (mag <= KNEE) return sum;
    float over = mag - KNEE;
    float compressed = KNEE + (1.0f - KNEE) * (over / (over + (1.0f - KNEE)));
    return sum > 0.0f ? compressed : -compressed;
}
inline float mix_and_soft_clip(float existing_sample, float new_sample) {
    return soft_clip_knee(existing_sample + new_sample);
}

// ריברב Freeverb מונו (Schroeder/Moorer): 8 מסנני-מסרק מרוסנים במקביל אל תוך
// 4 מסנני All-pass בטור. זול, מוזיקלי, זמן-אמת. נוגעים בו רק ב-Thread של ה-Output;
// הפרמטרים דרך atomics במנוע. מכוון ל-44.1k — סטיית ~9% ב-48k בלתי-נשמעת.
struct FreeverbMono {
    static constexpr int NC = 8;
    static constexpr int NA = 4;
    std::vector<float> comb_[NC];
    std::vector<float> ap_[NA];
    int ci_[NC] = {0};
    int ai_[NA] = {0};
    float store_[NC] = {0};
    float feedback_ = 0.84f, damp1_ = 0.2f, damp2_ = 0.8f;
    static constexpr float AP_FB = 0.5f;
    static constexpr float GAIN = 0.015f;

    void init(int sr) {
        static const int cd[NC] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int ad[NA] = {556, 441, 341, 225};
        float scale = static_cast<float>(sr) / 44100.0f;
        for (int i = 0; i < NC; i++) { comb_[i].assign(std::max(1, (int)(cd[i] * scale)), 0.0f); ci_[i] = 0; store_[i] = 0; }
        for (int i = 0; i < NA; i++) { ap_[i].assign(std::max(1, (int)(ad[i] * scale)), 0.0f); ai_[i] = 0; }
    }
    void set_params(float room, float damp) {
        feedback_ = 0.70f + 0.28f * room;   // room 0..1 -> feedback 0.70..0.98
        damp1_ = damp * 0.5f;               // damp 0..1
        damp2_ = 1.0f - damp1_;
    }
    inline float process(float in) {
        float input = in * GAIN;
        float out = 0.0f;
        for (int i = 0; i < NC; i++) {
            float y = comb_[i][ci_[i]];
            store_[i] = y * damp2_ + store_[i] * damp1_;   // lowpass in the feedback path
            comb_[i][ci_[i]] = input + store_[i] * feedback_;
            if (++ci_[i] >= (int)comb_[i].size()) ci_[i] = 0;
            out += y;
        }
        for (int i = 0; i < NA; i++) {
            float bo = ap_[i][ai_[i]];
            float y = bo - out;                            // -out + bufout
            ap_[i][ai_[i]] = out + bo * AP_FB;
            if (++ai_[i] >= (int)ap_[i].size()) ai_[i] = 0;
            out = y;
        }
        return out;
    }
};

// שכבת-סשן: יחידת ההעברה של שמירה/שחזור רב-מסלולי (JNI ↔ Worker). מכילה את
// ה-dry הפריסטיני + פרמטרי השכבה; ה-samples מרונדרים מחדש בעת הטעינה
// (render_layer) — כך קובץ הסשן קטן פי-שניים ואין דגרדציה כפולה של אפקטים.
struct SessionLayer {
    std::vector<float> dry;
    float gain = 1.0f;
    int   fx = 0;          // 0=none 1=reverse 2=oct-up 3=oct-down
    float reverb = 0.0f;
    bool  denoise = false; // CLEAN (v2) — מחושב מחדש מה-dry בטעינה (המטמון לא נשמר)
};

// טלמטריה פר-צ'אנק עבור ה-Harness האופליין (כוונון אמפירי).
// במכשיר ה-Sink נשאר null והעלות היא בדיקת מצביע אחת לצ'אנק.
// הערכים נלקחים מתוך ה-Worker עצמו — מדידה של המנוע האמיתי, לא שחזור שלו.
struct ChunkTelemetry {
    double time_seconds;          // זמן מצטבר מדויק-לדגימה (סוף הצ'אנק)
    LooperState state_before;     // המצב שבו הצ'אנק עובד
    LooperState state_after;      // המצב אחרי — מעברים נגזרים מההפרש
    float raw_rms;
    float trigger_rms;
    float rise_ratio;             // trigger / prev_trigger — מדד ה-Flux בפועל
    float onset_level_threshold;  // סף ההתקף האפקטיבי (מרחב מסונן)
    float sustain_threshold;      // סף התהודה האפקטיבי (מרחב גולמי)
    float silence_threshold;      // סף השקט האפקטיבי בזמן הקלטה (מרחב גולמי)
    float peak_envelope;
    int onset_streak;
    float noise_mean_trigger;
    float noise_std_trigger;
    float noise_mean_raw;
    float noise_std_raw;
    size_t published_loop_samples; // [חדש] אורך לופ שפורסם בצ'אנק זה (0 אם לא פורסם)
    float yin_confidence;         // [חדש] מובהקות מחזוריות YIN בחלון ההחלטה (-1 = לא חושב)
    uint32_t input_overrun_count; // [חדש] סה"כ דגימות-כניסה שנשמטו (תור מלא) — ~0 תמיד
};
using TelemetrySink = void(*)(const ChunkTelemetry& telemetry, void* user);

class DynamicThresholdTracker {
private:
    std::vector<float> noise_history_;
    size_t history_capacity_ = 0;
    size_t write_idx_ = 0;
    bool is_buffer_full_ = false;
    float current_mean_ = 0.0f;
    float current_std_dev_ = 0.0f;

    // נכתבים מ-Thread ה-JNI/UI, נקראים על ה-Worker — חייבים להיות אטומיים
    std::atomic<float> sigma_multiplier_onset_{12.0f};
    std::atomic<float> sigma_multiplier_silence_{2.0f};

public:
    DynamicThresholdTracker(int sample_rate, int chunk_size, float tracking_window_seconds = 1.0f);
    // Worker-thread בלבד: איפוס הסטטיסטיקה הנלמדת (למשל אחרי שינוי קצב דגימה)
    void reconfigure(int sample_rate, int chunk_size, float tracking_window_seconds);
    void observe_background_noise(float chunk_rms);
    void recalculate_statistics();
    bool is_ready() const;
    float get_onset_threshold() const;
    float get_silence_threshold() const;
    float get_mean() const;
    float get_std_dev() const { return current_std_dev_; }
    void set_sigma_onset(float val) { sigma_multiplier_onset_.store(val, std::memory_order_relaxed); }
    void set_sigma_silence(float val) { sigma_multiplier_silence_.store(val, std::memory_order_relaxed); }
};

class LooperEngine {
private:
    // config_ שייך ל-Worker אחרי הבנייה; Threads אחרים מבקשים שינויים דרך pending_* בלבד
    EngineConfig config_;
    LockFreeRingBuffer<float> input_queue_;

    std::atomic<LooperState> current_state_{LooperState::CALIBRATING};
    std::atomic<bool> is_running_{true};
    std::thread worker_thread_;
    std::atomic<float> target_bpm_{120.0f};

    // סטטיסטיקת רעש דו-מרחבית: מבחן ההתקף רץ על האות המסונן (Pre-Emphasis),
    // ומבחני התהודה/שקט רצים על האות הגולמי. הסקאלות אינן ברות-השוואה,
    // ולכן כל מרחב חייב סטטיסטיקה משלו.
    DynamicThresholdTracker noise_tracker_;       // מרחב מסונן (טריגר)
    DynamicThresholdTracker raw_noise_tracker_;   // מרחב גולמי (רחב-סרט)

    // חוצץ כפול בסגנון RCU: קולבק הרמקול הוא הקורא היחיד (ללא נעילה);
    // כל הכותבים רצים על ה-Worker ומסונכרנים מול קוראי-Snapshot של JNI דרך buffer_mutex_.
    // מותר לכתוב לחוצץ רק אחרי שהקורא נצפה על החוצץ השני (Grace Period).
    std::vector<float> playback_buffers_[2];
    alignas(64) std::atomic<int> active_playback_idx_{0};
    alignas(64) std::atomic<size_t> playback_read_idx_{0};
    alignas(64) std::atomic<int> reader_observed_idx_{0};
    mutable std::mutex buffer_mutex_;

    std::vector<float> preroll_buffer_;   // Worker-thread בלבד
    size_t preroll_write_idx_{0};

    std::atomic<bool> request_record_start_{false};
    std::atomic<bool> request_record_stop_{false};
    std::atomic<bool> request_overdub_{false};
    std::atomic<bool> request_looping_{false};
    std::atomic<bool> request_clear_{false};

    // --- רב-מסלול (Multi-track) ---
    // כל אוברדאב נשמר כשכבה נפרדת (Worker-owned), והמיקס מחושב-מחדש אל חוצץ
    // ה-RCU בכל שינוי שכבות. פקודות מגיעות מ-JNI דרך אטומיים; ה-Worker הוא
    // המפרסם היחיד. מסלול 0 = הבסיס המוגן (נמחק רק ב-clear הכללי). request_delete_layer_
    // נושא את *אינדקס* השכבה למחיקה (>=1), או -1 כשאין בקשה.
    std::atomic<int> request_delete_layer_{-1};
    std::atomic<int> layer_count_{0};       // מראה לקריאת ה-UI (0 = אין לופ)

    // ביטול מחיקה (עומק 1). המחיקה הייתה בלתי-הפיכה *ובלתי-נשמעת מראש*: אין
    // דרך לדעת איזו שכבה היא "אוברדאב 2" לפני שמוחקים אותה. השכבה האחרונה
    // שנמחקה נשמרת בשלמותה (dry+samples+פרמטרים) עד לאירוע שמייתר אותה.
    std::atomic<bool> request_undo_delete_{false};
    std::atomic<bool> undo_available_{false};

    // האזנה (Solo/Mute) כמסכת-ביטים — ביט פר-שכבה, 1 = מושתקת בהאזנה.
    // ⚠ מדוע מסכה ולא set_layer_gain(i, 0) בלולאה: ערוץ ה-gain הוא תא-פקודה
    // *יחיד* בסמנטיקת Last-Write-Wins (תקין לגרירת סליידר, קטלני לאצווה) —
    // סולו על 4 שכבות היה משדר 4 פקודות שרק האחרונה שורדת עד שה-Worker קורא.
    // מסכה היא כתיבה אטומית אחת, ובנוסף היא *אינה נוגעת ב-gain של המשתמש* כלל:
    // אין מה לשמור ואין מה לשחזר, ולכן גם אין מרוץ-שחזור ואין אינדקסים מיושנים.
    // ה-Worker מאפס אותה בכל שינוי במספר השכבות (מחיקה/ביטול/אוברדאב מזיזים
    // אינדקסים, ומסכה מיושנת הייתה משתיקה את השכבה הלא-נכונה).
    std::atomic<uint32_t> layer_mute_mask_{0};

    // ספירה-לתוך (SYNC בלבד): המצב היחיד שבו הנגן התחייב לרשת — ובכל זאת
    // ההקלטה נפתחה על הצליל הראשון, כלומר תמיד חצי פעימה באיחור.
    std::atomic<bool>  request_count_in_{false};
    std::atomic<bool>  request_count_in_cancel_{false};
    std::atomic<float> count_in_bars_{2.0f};
    std::atomic<int>   count_in_beats_left_{0};   // מראה ל-UI (0 = לא סופרים)

    // הגדרת אורך הלופ בתיבות *בדיעבד*: הנגן מנגן חופשי ואז אומר "זה היה 8
    // תיבות", והרשת נגזרת מהמוזיקה במקום להיקבע לפניה. אינו נוגע באודיו —
    // רק בקצב ובמספר הפעימות (שמזינים את הקליק, הטיקים והסנכרון).
    std::atomic<float> request_set_bars_{-1.0f};

    // כיול-מחדש של רעש החדר. הסטטיסטיקה נלמדה פעם אחת ב-CALIBRATING ומעולם
    // לא עודכנה: מאוורר/מזגן שנדלק *אחרי* העלייה משאיר את כל הרצפות היחסיות
    // מכוילות לחדר שכבר לא קיים.
    std::atomic<bool> request_recalibrate_{false};

    // --- אפקטים פר-שכבה (פייז 3, Pro) ---
    // ערוצי פקודה index+value (Last-Write-Wins; אובדן ערכי-ביניים בגרירת סליידר
    // הוא תקין). הכותב = JNI; ה-Worker צורך ומרנדר-מחדש את השכבה+המיקס. -1 = אין.
    static constexpr int kMaxLayers = 16;   // בסיס + 15 אוברדאבים
    std::atomic<int>   req_layer_fx_idx_{-1};
    std::atomic<int>   req_layer_fx_kind_{0};      // 0=none 1=reverse 2=oct-up 3=oct-down
    std::atomic<int>   req_layer_gain_idx_{-1};
    std::atomic<float> req_layer_gain_val_{1.0f};
    std::atomic<int>   req_layer_reverb_idx_{-1};
    std::atomic<float> req_layer_reverb_val_{0.0f};
    // מראה מצב פר-שכבה לקריאת ה-UI (ה-Worker כותב בכל שינוי שכבות; JNI קורא).
    std::atomic<int>   layer_fx_[kMaxLayers]{};
    std::atomic<float> layer_gain_[kMaxLayers]{};
    std::atomic<float> layer_reverb_[kMaxLayers]{};

    // CLEAN (שער ספקטרלי, Pro): פקודת הכל-או-כלום על *כל* השכבות — ערוץ פר-שכבה
    // היה מאבד פקודות (Last-Write-Wins על תא יחיד), וכפתור ה-UI ממילא גלובלי.
    // ‎-1=אין בקשה · 0=כבה-לכולן · 1=הדלק-לכולן. layer_denoise_count_ = כמה שכבות
    // נקיות כרגע (מדווח ב-pollTelemetry[8]; שוויון ל-layer_count_ ⇒ הכפתור דולק).
    std::atomic<int> req_denoise_all_{-1};
    std::atomic<int> layer_denoise_count_{0};

    // בקשות קונפיגורציית חומרה: נרשמות כאן, מוחלות אך ורק על-ידי ה-Worker
    std::atomic<int> pending_sample_rate_{-1};
    std::atomic<float> pending_preroll_seconds_{-1.0f};

    // מסירת Import: מפוענח על Thread ה-JNI, מפורסם על-ידי ה-Worker
    std::vector<float> pending_import_;           // מוגן על-ידי buffer_mutex_
    std::atomic<bool> has_pending_import_{false};

    // --- שמירה/שחזור סשן רב-מסלולי (פורמט NTSN v1) ---
    // טעינה: מפוענח על JNI (כמו Import), מוחל על-ידי ה-Worker (בעל layers_ היחיד).
    // שמירה: ה-Worker לבדו רואה את layers_, ולכן הבקשה נרשמת כאן וה-JNI ממתין
    // (Poll עם Timeout) לתוצאה — onStop חייב סנכרוניות (teardown רץ אחריו).
    std::vector<SessionLayer> pending_session_;   // מוגן על-ידי buffer_mutex_
    int   pending_session_rate_ = 0;              // מוגן על-ידי buffer_mutex_
    float pending_session_bpm_ = 0.0f;            // מוגן על-ידי buffer_mutex_
    float pending_session_beats_ = 0.0f;          // מוגן על-ידי buffer_mutex_
    std::atomic<bool> has_pending_session_{false};
    std::string pending_save_path_;               // מוגן על-ידי buffer_mutex_
    std::atomic<bool> request_save_session_{false};
    std::atomic<int>  session_save_result_{0};    // 0=בתהליך · 1=הצלחה · -1=כשל

    std::atomic<float> estimated_bpm_{0.0f};
    std::atomic<float> loop_beats_{0.0f};   // מס' הפעימות בלופ הנוכחי (לרשת ה-UI)

    // [חדש] שמירת אונטולוגיית הזיהוי (0=Auto, 1=Tap, 2=BPM)
    std::atomic<int> detection_mode_{0};

    // --- טרנספורט: עצירה / השתקה ---
    // *לא* מצבי-מנוע אלא דגלים על נתיב הפלט בלבד. LooperState נוסף היה מחייב כל
    // צרכן (acquire_writable_slot, טלמטריה, אונבורדינג, כל ה-switch של ה-Worker)
    // להכיר מצב-נגינה שלישי — סיכון מיותר במכונת-מצבים מכוילת. הסמנטיקה:
    //   transport_stopped_ = הקורא לא מתקדם, הפלט אפס, ראש-הקריאה חונה ב-0 ⇒
    //                        "PLAY" מתחיל מראש הלופ (סמנטיקת דוושה, לא המשך).
    //   output_muted_      = הקורא ממשיך להתקדם (הפאזה נשמרת) והפלט אפס בלבד ⇒
    //                        המשתמש חוזר פנימה *בקצב*. המטרונום נשאר נשמע.
    // נכתבים מ-JNI, נקראים ב-Thread של ה-Output. ה-Worker מאפס אותם בכל אירוע
    // שמוליד לופ חדש (clear/import/session/בסיס חדש) — אחרת לופ חדש נולד אילם.
    std::atomic<bool> transport_stopped_{false};
    std::atomic<bool> output_muted_{false};

    // עוצמת ניטור (Master). *לא* נכללת ב-reset_transport: זו העדפת האזנה של
    // המשתמש ("שקט ב-23:00"), לא מצב פר-לופ. חלה על הפלט הסופי — כולל המטרונום —
    // כי זו עוצמת *היציאה של האפליקציה*, לא של שכבה. אינה נוגעת ב-layers_,
    // ולכן ייצוא/סשן/הקלטת אוברדאב יוצאים ברמה מלאה תמיד.
    std::atomic<float> master_volume_{1.0f};
    // מצב ההחלקה — Thread של ה-Output בלבד. בלי החלקה, גרירת סליידר ב-60fps
    // מייצרת מדרגת-גיין בכל גבול-קולבק ("זיפר") שנשמעת על צליל מוחזק.
    float master_volume_smoothed_{1.0f};

    // סגירה-אוטומטית של אוברדאב אחרי *סיבוב אחד* מלא (ברירת המחדל).
    // בלעדיה כל שכבה עולה שתי נקישות (הצתה + נעילה) — וזה הורג את הבטחת
    // ה"בלי-מגע" מהשכבה השנייה והלאה. כבוי = התנהגות "צובר עד שתיגע" הישנה.
    std::atomic<bool> overdub_auto_close_{true};

    // הצמדת *תחילת* האוברדאב לראש הלופ. יחד עם הסגירה-האוטומטית זה מוציא את
    // תזמון הנקישה מהמשוואה לגמרי: השכבה היא בדיוק סיבוב אחד, מיושרת לתיבה,
    // ולא משנה מתי בדיוק נגעת במסך. זה התיקון האמיתי ל"אני מאחר והשכבה מתחילה
    // פעימה אחרי" — לא אפשר לזהות "כוונה" באמצע נגינה, אבל אפשר להפוך את רגע
    // הנגיעה ללא-רלוונטי. כבוי = כניסה מיידית (ההתנהגות הישנה).
    std::atomic<bool> overdub_quantize_{true};
    // מראה ל-UI: הנגן ביקש שכבה והמנוע ממתין לראש הלופ. אינו מצב-מנוע (המצב
    // נשאר LOOPING) — רק דגל תצוגה, כמו הטרנספורט.
    std::atomic<bool> overdub_armed_{false};

    // התקדמות סגירת-הטייק (0..1) לחיווי ה-UI: המרבי מבין שני מסלולי הסגירה.
    // 1.0 = ההקלטה נסגרת עכשיו. אפס בכל מצב שאינו RECORDING-אוטומטי. הנגזרת
    // כאן היא *בדיוק* זו שמחליטה, ולא שחזור שלה ב-Kotlin — אחרת החיווי משקר.
    std::atomic<float> closure_progress_{0.0f};

    // פרמטרי כוונון: נכתבים מ-JNI, נקראים על ה-Worker — אטומיים.
    // כוונון 2026-07-12 (גרסה חסינת-Gain): כל הרצפות המוחלטות הפכו ל-Bootstrap
    // מזערי בלבד (הגנה מאבק דיגיטלי בחדר מת); ההפרדה האמיתית עברה למכפלות
    // יחסיות-לרעש (חסינות לרגישות מיקרופון) ולשער המחזוריות של YIN (חסין-Gain
    // מתמטית — CMND הוא יחס מנורמל). נמדד: הפרדת-רמה של פריטה רכה מול טרנזיינט
    // סביבתי מתמשך *בלתי אפשרית* בטווח ±12dB (סאסטיין פריטה 0.046-0.065 חופף
    // לאירוע רעש ב-Gain×4: 0.05-0.18).
    std::atomic<float> min_onset_rms_{0.0005f};  // Bootstrap במרחב המסונן (שלב א')
    // רצפת התהודה במרחב הגולמי (שלב ב'): max(רעש+2σ, mean×מכפלה, Bootstrap).
    // המבחין העיקרי נשאר *משך* התהודה (persistence 12) + מחזוריות YIN;
    // הרצפה רק מונעת רצפים על אדוות רעש.
    std::atomic<float> raw_sustain_floor_{0.0025f};      // Bootstrap בלבד
    std::atomic<float> sustain_rel_mult_{8.0f};          // מכפלת mean רעש גולמי (≈רצפה 0.008 בחדר הייחוס)
    std::atomic<float> onset_rise_ratio_{1.5f};
    // 12 חלונות (~128ms) של תהודה רצופה. ה-Preroll מכסה את זמן ההחלטה.
    std::atomic<int> onset_persistence_target_{12};

    // שער המחזוריות (YIN/CMND) בנקודת ההחלטה. **נמדד ונפסל כמבחין** (2026-07-12):
    // קומפרסור-AC = מנוע מסתובב + הרמוניות רשת ⇒ מחזורי *חזק* (0.966), חבטת-חדר
    // מעוררת תהודה דועכת ⇒ 0.92, בעוד סטראם קשה (6 מיתרים לא-הרמוניים + התקף)
    // ⇒ 0.699. ההפרדה הפוכה — אין סף שהורג AC בלי להרוג סטראם. נשאר כטלמטריה
    // בלבד (0 = כבוי). אל תפעילו בלי מבחין משלים.
    std::atomic<float> yin_gate_threshold_{0.0f};

    // וטו תוכן-מזערי ב-PROCESSING (מצבי Auto בלבד): ההרג האמיתי של מחלקת
    // ה-FP הרחב-סרט. טרנזיינט סביבתי (חבטה/התנעת קומפרסור) עובר את מבחני
    // הרמה/משך/מחזוריות אך אינו משאיר תוכן: נמדד 0.1-0.2s מול 0.94s לפריטה
    // הבודדת הרפה ביותר (שוליים ×4.7). משך אינו סוקל עם Gain ⇒ חסין-רגישות.
    std::atomic<float> min_musical_seconds_{0.6f};

    // נטישת-דרון בזמן RECORDING (מצבי Auto בלבד): רעש רחב-סרט *שאינו נגמר*
    // (שואב-אבק/מקלחת/גשם) עובר את הטריגר ואז לא סוגר לעולם — לא שקט ולא
    // היעדר-פעילות — והיה מגיע לרצפת ה-300s ומפרסם לופ-זבל. המבחין: נגינה
    // אמיתית מכילה *ארטיקולציות* (התקפים חדשים); בקורפוס הפער המקסימלי בין
    // ארטיקולציות בהקלטה לגיטימית = 5.57s (כולל זנב-עד-עצירה). 10s = שוליים
    // ×1.8. בנטישה: ההקלטה נזרקת וחוזרים ל-CALIBRATING עם איפוס מלא של
    // הסטטיסטיקה — הרעש החדש נלמד כרצפה (ריפוי-עצמי); בלי האיפוס, הסטטיסטיקה
    // הישנה שורדת ונכנסים ללולאת הקלטות-רפאים של 10s לנצח.
    std::atomic<float> drone_abort_seconds_{10.0f};

    // --- זיהוי שקט (offset) ---
    // סף השקט *מעוגן לרצפת הרעש* ולא לפיק האות: עיגון-לפיק (peak*0.04) גורם
    // ל"רדיפת מעטפת" — על צליל מתמשך ודועך לאט הסף נדבק לאות ואינו נחצה לעולם.
    // המבנה: max(רעש+2σ, mean×מכפלה, Bootstrap) — יחסי-לרעש, חסין-Gain.
    std::atomic<float> silence_abs_floor_{0.003f};   // Bootstrap שקט (חדר מת בלבד)
    std::atomic<float> silence_rel_mult_{12.0f};     // מכפלת mean רעש (≈רצפה 0.012 בחדר הייחוס)
    // סגירת לופ = *מוקדם מבין שני מסלולים* (כויל מול תיוגי ה-end בקורפוס):
    //  (א) שקט מוחלט למשך silence_hold — מהיר, לעצירות mute/סטקטו שמגיעות לשקט אמיתי.
    //  (ב) היעדר *פעילות* למשך activity_hold — לזנב הד/ריברב שנשאר מעל סף השקט.
    // "פעילות" = raw מעל שבריר משיא ההקלטה הנוכחית. נמדד בקורפוס: במהלך נגינה
    // (כולל דקרשנדו, אקורדים מוחזקים והמיטה המצלצלת שבין תווים) raw נשאר ≥0.11,
    // וזנב-סיום דועך מתחת לכל רמת נגינה תוך ~שנייה. רצפה יחסית-לסשן ⇒ חסינה
    // לעוצמת כניסה שונה בין מכשירים. זיהוי "התקף-חוזר" נפסל אמפירית: סטראם
    // בדקרשנדו (יחס-למעטפת 0.98) נמוך מאדוות ריברב (1.1-1.3) — הפרדה הפוכה.
    std::atomic<float> silence_hold_seconds_{1.5f};
    std::atomic<float> activity_ratio_{0.15f};        // שבריר השיא שנחשב "פעיל" (כ--16dB)
    std::atomic<float> activity_hold_seconds_{4.5f};  // משך היעדר-פעילות רצוף לסגירה
    // תקרה יחסית-לשיא-הטייק על רצפת השקט. הרצפה היחסית-לרעש (mean×12) נבנתה
    // לחדר-הייחוס; בחדר רועש mean גדל והרצפה מטפסת עד *לתוך* טווח הנגינה, ואז
    // פסאז' פיאניסימו (פינגרסטייל קלאסי) נספר כשקט וההקלטה נסגרת באמצע ביטוי.
    // אילוץ הסדר הנכון בין שני מסלולי הסגירה: רצפת-שקט ≤ רצפת-פעילות (0.15 מהשיא).
    // 0.08 = חצי מרצפת הפעילות ⇒ המסלול המהיר נשאר מהיר, בלי לבלוע נגינה שקטה.
    std::atomic<float> silence_peak_cap_ratio_{0.08f};
    // שער ההרפיה: התקרה לעיל מוחלת רק כשהטייק מוכח כנגינה (שיא/רעש מעל היחס
    // הזה). בלעדיו טייק שהוצת מרעש מרפה את רצפתו שלו ומפרסם לופ-זבל — נמדד
    // ונכשל בכל נקודת מטריצה בקורפוס השלילי. ראה LooperEngine.cpp.
    std::atomic<float> silence_peak_cap_min_snr_{20.0f};

    float calculate_rms(const std::vector<float>& chunk);
    size_t find_true_onset(const std::vector<float>& audio_data, size_t max_search_samples, float threshold);
    std::vector<float> extract_novelty_curve(const std::vector<float>& audio_data, int& out_chunk_size);
    float extract_beat_length_from_onsets(const std::vector<float>& audio_data, size_t analysis_samples,
                                          size_t* last_onset_samples = nullptr);
    float quantize_to_musical_phrase(float raw_beats);
    std::vector<float> apply_seam_fold(std::vector<float>& audio, size_t fold_samples = 2048);
    void process_audio_asynchronously();

    // מנגנון סינון תדרים לטריגר בלבד (Worker-thread בלבד)
    float pre_emphasis_state_{0.0f};
    float calculate_trigger_rms(const std::vector<float>& chunk);

    // מונה שמיטת-כניסה (Overrun): נכתב על Thread ה-RT ב-on_audio_callback כשהתור
    // מלא (בלתי-נגיש מעשית — חוצץ 5 שניות). uint32 מובטח Wait-Free בכל ה-ABIs
    // (בניגוד ל-64-bit שעלול לנעול ב-armeabi-v7a). נקרא מה-Worker (טלמטריה) ומ-JNI.
    std::atomic<uint32_t> input_overrun_count_{0};

    // פיצוי לייטנסי-אוברדאב בדגימות (הלוך-ושוב: פלט+קלט). נכתב מ-OboeBridge
    // בפתיחת הזרמים; נקרא על-ידי ה-Worker בכניסת אוברדאב. ראה set_overdub_latency_samples.
    std::atomic<int> overdub_comp_samples_{0};

    // חוצצי FFT לעקומת ה-Novelty — מוקצים *פעם אחת* בבנאי (NFFT קבוע, בלתי-תלוי
    // בקצב הדגימה) ונעשה בהם שימוש-חוזר בכל בנייית לופ. Worker-thread בלבד:
    // extract_novelty_curve הוא הקורא היחיד, ולכן אין נעילה. מבטל את kiss_fftr_alloc
    // ואת הקצאות הווקטורים מנתיב ה-PROCESSING (הימנעות מנעילת Heap מיותרת).
    static constexpr int kNoveltyNFFT = 1024;
    kiss_fftr_cfg novelty_fft_cfg_ = nullptr;
    std::vector<float>        novelty_hann_;       // חלון Hann מחושב-מראש (NFFT)
    std::vector<kiss_fft_scalar> novelty_time_in_; // כניסת זמן (NFFT)
    std::vector<kiss_fft_cpx>    novelty_freq_out_;// יציאת תדר (NFFT/2+1)
    std::vector<float>        novelty_prev_mag_;   // מגניטודת החלון הקודם (NFFT/2+1)

    // --- מטרונום / Count-in (מצב SYNC) ---
    // נשמע ב-IDLE (ספירה-לתוך + נעילת קצב) וב-LOOPING/OVERDUBBING (סנכרון אוברדאב),
    // אך *שותק בזמן RECORDING* — הקליק דולף מהרמקול למיקרופון, וקליקים מחזוריים
    // היו מזהמים גם את ההקלטה וגם את זיהוי השקט. מצב הקליק נוגע רק ב-Thread של
    // ה-Output (process_output_callback), הפרמטרים אטומיים.
    std::atomic<bool> metronome_user_enabled_{true};
    // (הריברב הגלובלי החי הוסר: כל הריברב הוא פר-שכבה, נצרב מראש ע"י ה-Worker
    // אל samples — נתיב ה-Output קורא מיקס מוכן בלבד ואינו מריץ DSP ריברב.)
    double metro_free_counter_{0.0};   // מונה חופשי (IDLE); בנגינה נעול למיקום הלופ
    long   metro_last_beat_{-1};
    float  click_env_{0.0f};
    double click_phase_{0.0};           // פאזת הקליק ב-*מחזורים* [0,1) (לא רדיאנים)
    float  click_freq_{1000.0f};
    // טבלת גל sin לקליק המטרונום — מחושבת פעם אחת בבנאי; מחליפה std::sin פר-דגימה
    // על Thread ה-Output. חזקה-של-2 → אינדוקס ב-mask. Worker/Output קורא בלבד.
    static constexpr int kClickTableSize = 1024;
    std::vector<float> click_sine_;
    void render_metronome(float* out, size_t num_frames, LooperState state,
                          bool playing, size_t loop_pos, size_t loop_size);

    // Sink טלמטריה אופציונלי (Harness בלבד; null במכשיר)
    std::atomic<TelemetrySink> telemetry_sink_{nullptr};
    std::atomic<void*> telemetry_user_{nullptr};

    // מונה רצף ההיסטרזיס (Worker-thread בלבד)
    int current_onset_streak_{0};

public:
    explicit LooperEngine(EngineConfig config = EngineConfig{});
    ~LooperEngine();

    std::atomic<float> current_rms_{0.0f};
    std::atomic<float> current_noise_std_dev_{0.0f};
    std::atomic<bool> transient_hit_flag_{false};

    // סה"כ דגימות-כניסה שנשמטו בגלל תור מלא (תצפית לקצה-מקרה פתולוגי; ~0 תמיד).
    uint32_t get_input_overrun_count() const {
        return input_overrun_count_.load(std::memory_order_relaxed);
    }

    void execute_overdub_command();
    void execute_loop_command();
    void execute_clear_command();

    // רב-מסלול: מחיקת שכבת-אוברדאב לפי אינדקס (>=1; 0=בסיס מוגן, מתעלמים).
    // רק רושם בקשה; ה-Worker מבצע ומחשב-מחדש את המיקס (מודל RCU).
    void delete_layer(int index) { request_delete_layer_.store(index, std::memory_order_relaxed); }
    // מס' השכבות הפעילות (בסיס + אוברדאבים). 0 = אין לופ. נקרא מ-JNI/UI.
    int get_layer_count() const { return layer_count_.load(std::memory_order_relaxed); }

    // --- אפקטים פר-שכבה (פייז 3): כתיבת פקודה + קריאת מצב-מראה ---
    // value נכתב לפני index (release) כדי שה-Worker יקרא ערך עקבי. חוקי לכל
    // אינדקס תקף (כולל הבסיס — "מוגן" נוגע רק למחיקה, לא לאפקטים).
    void set_layer_fx(int index, int kind) {
        req_layer_fx_kind_.store(kind, std::memory_order_relaxed);
        req_layer_fx_idx_.store(index, std::memory_order_release);
    }
    void set_layer_gain(int index, float gain) {
        req_layer_gain_val_.store(gain, std::memory_order_relaxed);
        req_layer_gain_idx_.store(index, std::memory_order_release);
    }
    void set_layer_reverb(int index, float wet) {
        req_layer_reverb_val_.store(wet, std::memory_order_relaxed);
        req_layer_reverb_idx_.store(index, std::memory_order_release);
    }
    // CLEAN: הדלקה/כיבוי של השער הספקטרלי על כל השכבות בבת-אחת (Worker מרנדר
    // ומפרסם חלק). מס' השכבות הנקיות זורם ל-UI דרך pollTelemetry[8].
    void set_denoise_all(bool on) { req_denoise_all_.store(on ? 1 : 0, std::memory_order_relaxed); }
    int get_layer_denoise_count() const { return layer_denoise_count_.load(std::memory_order_relaxed); }

    int   get_layer_fx(int index) const {
        return (index >= 0 && index < kMaxLayers) ? layer_fx_[index].load(std::memory_order_relaxed) : 0;
    }
    float get_layer_gain(int index) const {
        return (index >= 0 && index < kMaxLayers) ? layer_gain_[index].load(std::memory_order_relaxed) : 1.0f;
    }
    float get_layer_reverb(int index) const {
        return (index >= 0 && index < kMaxLayers) ? layer_reverb_[index].load(std::memory_order_relaxed) : 0.0f;
    }

    void execute_record_start_command();
    void execute_record_stop_command();
    void set_detection_mode(int mode);

    void set_onset_persistence(int chunks) { onset_persistence_target_.store(chunks, std::memory_order_relaxed); }
    void set_min_onset_rms(float val) { min_onset_rms_.store(val, std::memory_order_relaxed); }
    void set_raw_sustain_floor(float val) { raw_sustain_floor_.store(val, std::memory_order_relaxed); }
    void set_sustain_rel_mult(float m) { sustain_rel_mult_.store(m, std::memory_order_relaxed); }
    void set_silence_rel_mult(float m) { silence_rel_mult_.store(m, std::memory_order_relaxed); }
    void set_yin_gate_threshold(float t) { yin_gate_threshold_.store(t, std::memory_order_relaxed); }
    void set_min_musical_seconds(float s) { min_musical_seconds_.store(s, std::memory_order_relaxed); }
    void set_drone_abort_seconds(float s) { drone_abort_seconds_.store(s, std::memory_order_relaxed); }
    void set_onset_rise_ratio(float ratio) { onset_rise_ratio_.store(ratio, std::memory_order_relaxed); }
    void set_silence_abs_floor(float val) { silence_abs_floor_.store(val, std::memory_order_relaxed); }
    void set_silence_hold_seconds(float sec) { silence_hold_seconds_.store(sec, std::memory_order_relaxed); }
    void set_activity_hold_seconds(float sec) { activity_hold_seconds_.store(sec, std::memory_order_relaxed); }
    void set_activity_ratio(float ratio) { activity_ratio_.store(ratio, std::memory_order_relaxed); }
    void set_silence_peak_cap_ratio(float r) { silence_peak_cap_ratio_.store(r, std::memory_order_relaxed); }

    // --- טרנספורט (נקרא ב-Thread של ה-Output; נכתב מ-JNI) ---
    void set_transport_stopped(bool v) { transport_stopped_.store(v, std::memory_order_relaxed); }
    void set_output_muted(bool v) { output_muted_.store(v, std::memory_order_relaxed); }
    /** עוצמת ניטור לינארית 0..1 (עקומת הסליידר נקבעת ב-UI). */
    void set_master_volume(float v) {
        master_volume_.store(std::clamp(v, 0.0f, 1.0f), std::memory_order_relaxed);
    }
    float get_master_volume() const { return master_volume_.load(std::memory_order_relaxed); }
    bool is_transport_stopped() const { return transport_stopped_.load(std::memory_order_relaxed); }
    bool is_output_muted() const { return output_muted_.load(std::memory_order_relaxed); }

    void set_overdub_auto_close(bool v) { overdub_auto_close_.store(v, std::memory_order_relaxed); }
    bool get_overdub_auto_close() const { return overdub_auto_close_.load(std::memory_order_relaxed); }
    void set_overdub_quantize(bool v) { overdub_quantize_.store(v, std::memory_order_relaxed); }
    bool is_overdub_armed() const { return overdub_armed_.load(std::memory_order_relaxed); }

    /** ביטול המחיקה האחרונה (עומק 1). ה-Worker מבצע ומפרסם. */
    void undo_delete_layer() { request_undo_delete_.store(true, std::memory_order_relaxed); }
    bool is_undo_available() const { return undo_available_.load(std::memory_order_relaxed); }

    /** מסכת האזנה: ביט פר-שכבה, 1 = מושתקת. 0 = המיקס האמיתי. */
    void set_layer_mute_mask(uint32_t mask) {
        layer_mute_mask_.store(mask, std::memory_order_release);
    }
    uint32_t get_layer_mute_mask() const {
        return layer_mute_mask_.load(std::memory_order_relaxed);
    }

    /** ספירה-לתוך: bars תיבות של 4/4 בקצב היעד, ואז ההקלטה נפתחת מעצמה. */
    void start_count_in() { request_count_in_.store(true, std::memory_order_relaxed); }
    void cancel_count_in() { request_count_in_cancel_.store(true, std::memory_order_relaxed); }
    void set_count_in_bars(float bars) {
        count_in_bars_.store(std::clamp(bars, 1.0f, 8.0f), std::memory_order_relaxed);
    }
    int get_count_in_beats_left() const {
        return count_in_beats_left_.load(std::memory_order_relaxed);
    }

    /** הצהרה בדיעבד על אורך הלופ בתיבות (4/4). לא נוגע באודיו. */
    void set_loop_bars(float bars) {
        if (bars > 0.0f) request_set_bars_.store(bars, std::memory_order_relaxed);
    }

    /** למידה מחדש של רעש החדר (חוקי מ-IDLE/CALIBRATING; מתעלם בזמן נגינה). */
    void request_recalibrate() { request_recalibrate_.store(true, std::memory_order_relaxed); }

    // התקדמות סגירת הטייק (0..1) לחיווי; 0 כשלא רלוונטי.
    float get_closure_progress() const { return closure_progress_.load(std::memory_order_relaxed); }
    DynamicThresholdTracker& get_noise_tracker() { return noise_tracker_; }
    DynamicThresholdTracker& get_raw_noise_tracker() { return raw_noise_tracker_; }

    float get_estimated_bpm() const;
    float get_loop_beats() const;
    float get_loop_position() const;
    /** אורך הלופ בשניות (0 = אין לופ). נדרש כדי להצהיר על מספר תיבות בדיעבד
     *  גם על לופ *מיובא*, שאין לו קצב או פעימות מאומדים כלל. */
    float get_loop_seconds() const;

    bool export_to_wav(const char* filepath);
    bool import_from_wav(const char* filepath);

    // סשן רב-מסלולי (NTSN v1): שמירה/טעינה של *כל* השכבות — dry + gain/fx/reverb —
    // כך ששחזור מחזיר את מבנה השכבות המלא (מחיקה/עריכה פר-שכבה שורדות ריסטארט),
    // לא מיקס משוטח. save חוסם עד השלמת הכתיבה על-ידי ה-Worker (ראה שדות ה-pending);
    // load היא מסירה א-סינכרונית בסגנון Import (true = פוענח ונמסר).
    bool save_session(const char* filepath);
    bool load_session(const char* filepath);

    // מדווח למנוע את קצב הדגימה האמיתי של החומרה ואת הלייטנסי שלה.
    // רק רושם בקשה; ה-Worker מחיל אותה בבטחה בתחילת האיטרציה שלו.
    void update_hardware_config(int sample_rate, float latency_seconds);

    void on_audio_callback(const float* input_data, size_t num_frames);
    void process_output_callback(float* output_data, size_t num_frames);
    LooperState get_current_state() const;

    // --- ממשק Harness אופליין בלבד ---
    // חיבור Sink לטלמטריה פר-צ'אנק. חובה לקרוא *לפני* הזרמת אודיו ראשונה.
    void set_telemetry_sink(TelemetrySink sink, void* user) {
        telemetry_user_.store(user, std::memory_order_relaxed);
        telemetry_sink_.store(sink, std::memory_order_release);
    }
    // הזנה עם Backpressure: מחזירה כמה דגימות התקבלו בפועל (בניגוד ל-on_audio_callback
    // שחייב להישאר Wait-Free ולכן מפיל דגימות בשקט כשהתור מלא).
    size_t feed_audio_offline(const float* data, size_t num_samples);

    void set_target_bpm(float bpm) {
        target_bpm_.store(bpm, std::memory_order_relaxed);
    }

    void set_metronome_enabled(bool on) {
        metronome_user_enabled_.store(on, std::memory_order_relaxed);
    }

    // פיצוי לייטנסי-אוברדאב: הדגימה שמגיעה מהמיקרופון "עכשיו" נוגנה על-ידי
    // המשתמש כנגד מה ששמע לפני (לייטנסי-פלט + לייטנסי-קלט) — בלי פיצוי כל
    // אוברדאב נכתב באיחור סיסטמטי של סיבוב-הלוך-ושוב שלם ("פלאם" שמעי). נקבע
    // על-ידי ה-OboeBridge בעת פתיחת הזרמים; ה-Worker מחסיר אותו מנקודת-הכניסה
    // של האוברדאב. 0 = ללא פיצוי (מצב אופליין/Harness — אין שם רמקול↔מיקרופון).
    void set_overdub_latency_samples(int samples) {
        overdub_comp_samples_.store(std::max(0, samples), std::memory_order_relaxed);
    }

    // צילום מעטפת גל של הלופ הפעיל ל-bins שווים (Peak לכל bin). מוחזר מס' ה-bins.
    int get_loop_waveform(float* out, int max_bins);
};
