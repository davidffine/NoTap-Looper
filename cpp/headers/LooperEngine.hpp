#pragma once

#include "LockFreeRingBuffer.hpp"
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <cmath>
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

// מיקס אוברדאב עם ברך רכה: לינארי לחלוטין עד KNEE, דחיסה אסימפטוטית ל-±1.0 מעליו.
// הנוסחה הקודמת x/(1+|x|) דחסה *בכל* עוצמה — כל מעבר אוברדאב הנחית את הלופ הקיים
// (0.3→0.23) גם עם כניסה שקטה, דגרדציה מצטברת בכל סיבוב.
inline float mix_and_soft_clip(float existing_sample, float new_sample) {
    float sum = existing_sample + new_sample;
    const float KNEE = 0.9f;
    float mag = std::abs(sum);
    if (mag <= KNEE) return sum;
    float over = mag - KNEE;
    float compressed = KNEE + (1.0f - KNEE) * (over / (over + (1.0f - KNEE)));
    return sum > 0.0f ? compressed : -compressed;
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
    std::atomic<int>  request_effect_{0};   // 0=none 1=reverse 2=octave-up 3=octave-down

    // בקשות קונפיגורציית חומרה: נרשמות כאן, מוחלות אך ורק על-ידי ה-Worker
    std::atomic<int> pending_sample_rate_{-1};
    std::atomic<float> pending_preroll_seconds_{-1.0f};

    // מסירת Import: מפוענח על Thread ה-JNI, מפורסם על-ידי ה-Worker
    std::vector<float> pending_import_;           // מוגן על-ידי buffer_mutex_
    std::atomic<bool> has_pending_import_{false};

    std::atomic<float> estimated_bpm_{0.0f};
    std::atomic<float> loop_beats_{0.0f};   // מס' הפעימות בלופ הנוכחי (לרשת ה-UI)

    // [חדש] שמירת אונטולוגיית הזיהוי (0=Auto, 1=Tap, 2=BPM)
    std::atomic<int> detection_mode_{0};

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

    float calculate_rms(const std::vector<float>& chunk);
    size_t find_true_onset(const std::vector<float>& audio_data, size_t max_search_samples, float threshold);
    std::vector<float> extract_novelty_curve(const std::vector<float>& audio_data, int& out_chunk_size);
    float extract_beat_length_from_onsets(const std::vector<float>& audio_data, size_t analysis_samples,
                                          size_t* last_onset_samples = nullptr);
    float quantize_to_musical_phrase(float raw_beats);
    std::vector<float> apply_seam_fold(std::vector<float>& audio, size_t fold_samples = 2048);
    std::vector<float> apply_loop_effect(const std::vector<float>& src, int fx);
    void process_audio_asynchronously();

    // מנגנון סינון תדרים לטריגר בלבד (Worker-thread בלבד)
    float pre_emphasis_state_{0.0f};
    float calculate_trigger_rms(const std::vector<float>& chunk);

    // --- מטרונום / Count-in (מצב SYNC) ---
    // נשמע ב-IDLE (ספירה-לתוך + נעילת קצב) וב-LOOPING/OVERDUBBING (סנכרון אוברדאב),
    // אך *שותק בזמן RECORDING* — הקליק דולף מהרמקול למיקרופון, וקליקים מחזוריים
    // היו מזהמים גם את ההקלטה וגם את זיהוי השקט. מצב הקליק נוגע רק ב-Thread של
    // ה-Output (process_output_callback), הפרמטרים אטומיים.
    std::atomic<bool> metronome_user_enabled_{true};
    // ריברב על ה-Output (לא-הרסני): הלופ המוקלט נשאר יבש; הרטוב מתווסף בנגינה בלבד.
    // reverb_wet_ = כמות הרטוב 0..1 (0 = יבש/כבוי), נשלט בכפתור.
    std::atomic<float> reverb_wet_{0.0f};
    FreeverbMono reverb_;
    double metro_free_counter_{0.0};   // מונה חופשי (IDLE); בנגינה נעול למיקום הלופ
    long   metro_last_beat_{-1};
    float  click_env_{0.0f};
    double click_phase_{0.0};
    float  click_freq_{1000.0f};
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

    void execute_overdub_command();
    void execute_loop_command();
    void execute_clear_command();

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
    DynamicThresholdTracker& get_noise_tracker() { return noise_tracker_; }
    DynamicThresholdTracker& get_raw_noise_tracker() { return raw_noise_tracker_; }

    float get_estimated_bpm() const;
    float get_loop_beats() const;
    float get_loop_position() const;

    bool export_to_wav(const char* filepath);
    bool import_from_wav(const char* filepath);

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

    void set_reverb_wet(float wet) {
        reverb_wet_.store(std::clamp(wet, 0.0f, 1.0f), std::memory_order_relaxed);
    }

    // אפקטים חיים על הלופ (0=none 1=reverse 2=octave-up 3=octave-down). מוחל
    // על-ידי ה-Worker (מפרסם יחיד) כדי לשמור על מודל ה-RCU.
    void request_effect(int fx) { request_effect_.store(fx, std::memory_order_relaxed); }
    // צילום מעטפת גל של הלופ הפעיל ל-bins שווים (Peak לכל bin). מוחזר מס' ה-bins.
    int get_loop_waveform(float* out, int max_bins);
};
