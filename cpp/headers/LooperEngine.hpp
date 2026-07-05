#pragma once

#include "LockFreeRingBuffer.hpp"
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <cmath>

enum class LooperState {
    CALIBRATING,
    IDLE,
    RECORDING,
    PROCESSING,
    LOOPING,
    OVERDUBBING
};

std::string state_to_string(LooperState state);

enum class LooperMode {
    RHYTHMIC_GRID,
    CIRCULAR_REPETITION
};

struct EngineConfig {
    int sample_rate = 44100;
    int chunk_size = 512;
    float preroll_seconds = 0.15f;
    float silence_duration = 0.8f;
    LooperMode mode = LooperMode::RHYTHMIC_GRID;
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
    float spectral_flux;          // [חדש] Spectral Flux זורם (novelty פר-צ'אנק)
    float flux_threshold;         // [חדש] הסף האדפטיבי של ה-Flux (חציון + k·MAD)
    size_t published_loop_samples; // [חדש] אורך לופ שפורסם בצ'אנק זה (0 אם לא פורסם)
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

    // בקשות קונפיגורציית חומרה: נרשמות כאן, מוחלות אך ורק על-ידי ה-Worker
    std::atomic<int> pending_sample_rate_{-1};
    std::atomic<float> pending_preroll_seconds_{-1.0f};

    // מסירת Import: מפוענח על Thread ה-JNI, מפורסם על-ידי ה-Worker
    std::vector<float> pending_import_;           // מוגן על-ידי buffer_mutex_
    std::atomic<bool> has_pending_import_{false};

    std::atomic<float> estimated_bpm_{0.0f};

    // [חדש] שמירת אונטולוגיית הזיהוי (0=Auto, 1=Tap, 2=BPM)
    std::atomic<int> detection_mode_{0};

    // פרמטרי כוונון: נכתבים מ-JNI, נקראים על ה-Worker — אטומיים.
    // הערכים כוילו אמפירית מול הקורפוס (2026-07-05): ראה [[notap-dsp-roadmap]].
    std::atomic<float> min_onset_rms_{0.002f};   // רצפת ההתקף במרחב המסונן (שלב א')
    // רצפת התהודה במרחב הגולמי (שלב ב') — *מנותקת* מ-min_onset.
    // פריטה רכה וטרנזיינט סביבתי חופפים ברמת ה-RMS הגולמית, ולכן המבחין האמיתי
    // הוא *משך* התהודה: מיתר מנוגן שומר אנרגיה לאורך עשרות חלונות, נקישה/טרנזיינט
    // דועך תוך 1-2 חלונות. רצפה נמוכה (0.025) חיונית כדי לתפוס מיתר E פתוח דק
    // ברמה חלשה (Oboe Unprocessed מחליש את הכניסה פי ~2-4 מול הקלטת ייחוס);
    // persistence גבוה (12) מפצה ודוחה את הטרנזיינטים הסביבתיים לפי משך בלבד.
    std::atomic<float> raw_sustain_floor_{0.025f};
    std::atomic<float> onset_rise_ratio_{1.5f};
    // 12 חלונות (~128ms) של תהודה רצופה. ה-Preroll מכסה את זמן ההחלטה.
    std::atomic<int> onset_persistence_target_{12};

    // --- זיהוי שקט (offset) ---
    // סף השקט *מעוגן לרצפת הרעש* ולא לפיק האות: עיגון-לפיק (peak*0.04) גורם
    // ל"רדיפת מעטפת" — על צליל מתמשך ודועך לאט הסף נדבק לאות ואינו נחצה לעולם.
    std::atomic<float> silence_abs_floor_{0.012f};  // רצפת שקט מוחלטת (~-38dB)
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
    std::vector<float> extract_novelty_curve(const std::vector<float>& audio_data, int env_sr, int& out_chunk_size);
    float extract_beat_length_from_onsets(const std::vector<float>& audio_data, size_t analysis_samples);
    float quantize_to_musical_phrase(float raw_beats);
    std::vector<float> apply_zero_crossing_crossfade(std::vector<float>& audio, size_t crossfade_samples = 256);
    void process_audio_asynchronously();

    // מנגנון סינון תדרים לטריגר בלבד (Worker-thread בלבד)
    float pre_emphasis_state_{0.0f};
    float calculate_trigger_rms(const std::vector<float>& chunk);

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
    void set_onset_rise_ratio(float ratio) { onset_rise_ratio_.store(ratio, std::memory_order_relaxed); }
    void set_silence_abs_floor(float val) { silence_abs_floor_.store(val, std::memory_order_relaxed); }
    void set_silence_hold_seconds(float sec) { silence_hold_seconds_.store(sec, std::memory_order_relaxed); }
    void set_activity_hold_seconds(float sec) { activity_hold_seconds_.store(sec, std::memory_order_relaxed); }
    void set_activity_ratio(float ratio) { activity_ratio_.store(ratio, std::memory_order_relaxed); }
    DynamicThresholdTracker& get_noise_tracker() { return noise_tracker_; }
    DynamicThresholdTracker& get_raw_noise_tracker() { return raw_noise_tracker_; }

    float get_estimated_bpm() const;
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
};
