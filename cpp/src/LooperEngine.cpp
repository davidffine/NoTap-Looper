#include "../headers/LooperEngine.hpp"
#include "../headers/OctaveResample.hpp"
#include "../headers/PitchShift.hpp"
#include "../headers/NoiseGate.hpp"
#include "../includes/kissfft/kiss_fftr.h"
#include "../headers/miniaudio.h"
#include <iostream>
#include <fstream>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <algorithm>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

// פרמטרי הריברב — מקור אמת יחיד עבור צריבת הריברב הפר-שכבתית (bake_reverb_loop).
// כמות ה-WET פר-שכבה; האופי (חדר/ריסון) קבוע.
namespace {
    constexpr float REVERB_ROOM = 0.72f;
    constexpr float REVERB_DAMP = 0.45f;

    // שכבת רב-מסלול (Worker-owned בלבד). כל אוברדאב הוא Layer נפרד באורך
    // loop_length_ בדיוק (האינוריאנטה של המנוע). layers_[0] = הבסיס המוגן.
    //
    // מודל אפקטים לא-הרסני (פייז 3): dry = ההקלטה הפריסטינית; samples = הרינדור
    // (fx∘reverb על dry), וזה מה שהמיקס קורא. gain מוחל *בזמן המיקס* (חי, לא
    // נצרב). לכן אוקטבה↑ ואז ↓ ואז none חוזר ל-dry המקורי — אפס דגרדציה מצטברת.
    // כל טרנספורם כאן שומר-אורך: reverse (מדויק), octave_pitch (Phase-Vocoder),
    // reverb-bake (זנב Freeverb על סיבוב אחד) — כולם באורך loop_length_.
    struct Layer {
        std::vector<float> dry;       // ההקלטה הפריסטינית (אורך loop_length_)
        std::vector<float> samples;   // מרונדר = fx∘reverb∘clean(dry); המיקס קורא מכאן
        float gain = 1.0f;            // עוצמה חיה (מוחל במיקס, לא נצרב)
        int   fx = 0;                 // 0=none 1=reverse 2=oct-up 3=oct-down
        float reverb = 0.0f;          // כמות ריברב פר-שכבה, נצרבת ל-samples
        bool  denoise = false;        // CLEAN: שער ספקטרלי על ה-dry (הפיך; dry לא נגוע)
        std::vector<float> dry_denoised;  // מטמון — השער יקר (~2×FFT על הלופ); ריק = לא חושב
    };

    // --- קובץ סשן NTSN v2 (שמירת-אוטומט רב-מסלולית, פנימי-למכשיר) ---
    // header: magic "NTSN" · version u32 · sample_rate u32 · layer_count u32 ·
    //         loop_length u64 · bpm f32 · beats f32
    // per-layer: gain f32 · fx i32 · reverb f32 · denoise i32 (v2) · dry[loop_length] f32
    // dry נשמר כ-float גולמי (שחזור ביט-מדויק; samples מרונדרים מחדש בטעינה).
    // v2 מוסיף את דגל ה-CLEAN; v1 נדחה (מעולם לא רץ על מכשיר — אין קבצי v1 בעולם).
    // Little-endian בלבד — קובץ מקומי-למכשיר (ARM/x86 שניהם LE), לא פורמט חליפין.
    // כתיבה אטומית-בקירוב: tmp ואז rename, כך שקריסה באמצע לא משחיתה סשן קודם.
    constexpr char     kSessionMagic[4] = {'N','T','S','N'};
    constexpr uint32_t kSessionVersion  = 2;

    inline bool write_session_file(const std::string& path, const std::vector<Layer>& layers,
                                   size_t loop_len, int sr, float bpm, float beats) {
        if (layers.empty() || loop_len == 0) return false;
        for (const Layer& L : layers)
            if (L.dry.size() != loop_len) return false;   // אינוריאנטה לפני כל I/O
        const std::string tmp = path + ".tmp";
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        auto put = [&](const void* p, size_t n) { f.write(static_cast<const char*>(p), n); };
        uint32_t ver = kSessionVersion, srate = static_cast<uint32_t>(sr),
                 count = static_cast<uint32_t>(layers.size());
        uint64_t len64 = static_cast<uint64_t>(loop_len);
        put(kSessionMagic, 4); put(&ver, 4); put(&srate, 4); put(&count, 4);
        put(&len64, 8); put(&bpm, 4); put(&beats, 4);
        for (const Layer& L : layers) {
            int32_t fx32 = L.fx;
            int32_t dn32 = L.denoise ? 1 : 0;
            put(&L.gain, 4); put(&fx32, 4); put(&L.reverb, 4); put(&dn32, 4);
            put(L.dry.data(), loop_len * sizeof(float));
        }
        f.flush();
        bool ok = static_cast<bool>(f);
        f.close();
        if (!ok) { std::remove(tmp.c_str()); return false; }
        std::remove(path.c_str());   // rename אינו דורס בכל הפלטפורמות — מסירים קודם
        return std::rename(tmp.c_str(), path.c_str()) == 0;
    }

    // צריבת ריברב שומרת-אורך על סיבוב לופ יחיד (זהה לנתיב הייצוא): מחממים
    // Freeverb עד יציבות כדי שהזנב יעטוף את התפר, ואז לוכדים סיבוב אחד רטוב+יבש.
    inline std::vector<float> bake_reverb_loop(const std::vector<float>& loop, float wet, int sr) {
        FreeverbMono rv;
        rv.init(sr);
        rv.set_params(REVERB_ROOM, REVERB_DAMP);
        const size_t n = loop.size();
        if (n == 0) return loop;
        size_t warm_target = std::max(static_cast<size_t>(3 * sr), n);
        for (size_t warmed = 0; warmed < warm_target; warmed += n)
            for (size_t i = 0; i < n; ++i) rv.process(loop[i]);   // חימום (נזרק)
        std::vector<float> out(n);
        for (size_t i = 0; i < n; ++i) {
            float w = rv.process(loop[i]);
            out[i] = std::clamp(loop[i] + w * wet, -1.0f, 1.0f);
        }
        return out;
    }
}

// FTZ/DAZ הם מצב של רגיסטר ה-FP *של ה-Thread הנוכחי*.
// חייבים להיקרא מתוך ה-Thread המריץ עצמו — קריאה מה-Constructor מגינה על ה-Thread
// הלא נכון. באנדרואיד האמיתי (ARM) ענף ה-x86 לא מתקמפל כלל — חובה ענף FPCR/FPSCR.
void enable_denormal_flush_to_zero() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);   // FZ: Flush-to-Zero
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__arm__)
    uint32_t fpscr;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24);    // FZ: Flush-to-Zero
    __asm__ __volatile__("vmsr fpscr, %0" : : "r"(fpscr));
#endif
}

// מובהקות מחזוריות YIN (CMND): 1 = מחזורי לחלוטין, 0 = א-מחזורי.
// סקאלה-אינווריאנטי מתמטית — הכפלת האות ב-g מתבטלת ביחס d(τ)·τ/Σd, ולכן זהו
// המבחין היחיד שאינו תלוי ברגישות המיקרופון/עוצמת הכניסה. d(τ) מנורמל
// פר-דגימה כי טווח האינטגרציה מתכווץ עם τ בחלון קבוע.
static float yin_periodicity(const float* x, int W, int tau_min, int tau_max) {
    if (tau_max >= W - 1) tau_max = W - 2;
    if (tau_min < 2) tau_min = 2;
    if (tau_min >= tau_max || W < 64) return 0.0f;
    float best = 1.0f;
    double running = 0.0;
    for (int tau = 1; tau <= tau_max; ++tau) {
        double d = 0.0;
        const int span = W - tau;
        for (int i = 0; i < span; ++i) {
            float diff = x[i] - x[i + tau];
            d += static_cast<double>(diff) * diff;
        }
        d /= span;
        running += d;
        if (tau >= tau_min && running > 1e-30) {
            float cmnd = static_cast<float>(d * tau / running);
            if (cmnd < best) best = cmnd;
        }
    }
    return 1.0f - best;
}

std::string state_to_string(LooperState state) {
    switch (state) {
        case LooperState::CALIBRATING: return "CALIBRATING";
        case LooperState::IDLE:        return "IDLE";
        case LooperState::RECORDING:   return "RECORDING";
        case LooperState::PROCESSING:  return "PROCESSING";
        case LooperState::LOOPING:     return "LOOPING";
        case LooperState::OVERDUBBING: return "OVERDUBBING";
        default:                       return "UNKNOWN";
    }
}

DynamicThresholdTracker::DynamicThresholdTracker(int sample_rate, int chunk_size, float tracking_window_seconds) {
    reconfigure(sample_rate, chunk_size, tracking_window_seconds);
}

void DynamicThresholdTracker::reconfigure(int sample_rate, int chunk_size, float tracking_window_seconds) {
    history_capacity_ = static_cast<size_t>(sample_rate * tracking_window_seconds) / chunk_size;
    if (history_capacity_ < 10) history_capacity_ = 10;
    noise_history_.assign(history_capacity_, 0.0f);
    write_idx_ = 0;
    is_buffer_full_ = false;
    current_mean_ = 0.0f;
    current_std_dev_ = 0.0f;
}

void DynamicThresholdTracker::observe_background_noise(float chunk_rms) {
    noise_history_[write_idx_] = chunk_rms;
    write_idx_++;
    if (write_idx_ >= history_capacity_) {
        write_idx_ = 0;
        is_buffer_full_ = true;
    }
    recalculate_statistics();
}

void DynamicThresholdTracker::recalculate_statistics() {
    size_t count = is_buffer_full_ ? history_capacity_ : write_idx_;
    if (count < 2) return;
    float sum = std::accumulate(noise_history_.begin(), noise_history_.begin() + count, 0.0f);
    current_mean_ = sum / count;
    float variance_sum = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        float diff = noise_history_[i] - current_mean_;
        variance_sum += diff * diff;
    }
    current_std_dev_ = std::sqrt(variance_sum / count);
    // רצפת ה-σ חייבת להיות *יחסית לממוצע*, לא קבוע מוחלט: הקבוע הקודם (1e-4)
    // קיבע את סף ההתקף במכשיר חלש — ב-Gain×0.25 הסף נתקע ב-~0.0013 בעוד פריטה
    // רכה מסוננת מגיעה רק ל-~0.0009-0.0014 → עיוורון מובנה (נמדד בקורפוס).
    // mean×0.25 סוקל עם ה-Gain (אינווריאנטיות מדויקת); 1e-5 עוגן לחדר מת דיגיטלית.
    current_std_dev_ = std::max({current_std_dev_, current_mean_ * 0.25f, 1e-5f});
}

bool DynamicThresholdTracker::is_ready() const { return is_buffer_full_ || write_idx_ > (history_capacity_ / 2); }
float DynamicThresholdTracker::get_onset_threshold() const {
    return current_mean_ + (sigma_multiplier_onset_.load(std::memory_order_relaxed) * current_std_dev_);
}
float DynamicThresholdTracker::get_silence_threshold() const {
    return current_mean_ + (sigma_multiplier_silence_.load(std::memory_order_relaxed) * current_std_dev_);
}
float DynamicThresholdTracker::get_mean() const { return current_mean_; }

LooperEngine::LooperEngine(EngineConfig config) :
        config_(config),
        input_queue_(config.sample_rate * 5),
        noise_tracker_(config.sample_rate, config.chunk_size, 1.0f),
        raw_noise_tracker_(config.sample_rate, config.chunk_size, 1.0f)
{
    // הקצאת חוצצי ה-FFT של עקומת ה-Novelty פעם אחת (NFFT קבוע, בלתי-תלוי בקצב
    // הדגימה) — מבטל kiss_fftr_alloc ואת הקצאות הווקטורים מנתיב ה-PROCESSING.
    // חייב לקרות *לפני* הרצת ה-Worker (המשתמש היחיד) — אין Race.
    novelty_fft_cfg_ = kiss_fftr_alloc(kNoveltyNFFT, 0, nullptr, nullptr);
    novelty_time_in_.assign(kNoveltyNFFT, 0.0f);
    novelty_freq_out_.resize(kNoveltyNFFT / 2 + 1);
    novelty_prev_mag_.assign(kNoveltyNFFT / 2 + 1, 0.0f);
    novelty_hann_.resize(kNoveltyNFFT);
    {
        const float PI_F = 3.14159265358979323846f;
        for (int j = 0; j < kNoveltyNFFT; ++j)
            novelty_hann_[j] = 0.5f * (1.0f - std::cos(2.0f * PI_F * j / (kNoveltyNFFT - 1)));
    }

    // טבלת גל ה-sin של קליק המטרונום — מחושבת פעם אחת (מחליפה std::sin פר-דגימה).
    click_sine_.resize(kClickTableSize);
    {
        const double TWO_PI = 6.283185307179586;
        for (int i = 0; i < kClickTableSize; ++i)
            click_sine_[i] = static_cast<float>(std::sin(TWO_PI * i / kClickTableSize));
    }

    worker_thread_ = std::thread(&LooperEngine::process_audio_asynchronously, this);
}

LooperEngine::~LooperEngine() {
    is_running_.store(false, std::memory_order_relaxed);
    if (worker_thread_.joinable()) worker_thread_.join();
    // רק אחרי שה-Worker (המשתמש היחיד) נעצר — שחרור חוצץ ה-FFT.
    if (novelty_fft_cfg_) { kiss_fftr_free(novelty_fft_cfg_); novelty_fft_cfg_ = nullptr; }
}

void LooperEngine::execute_overdub_command() { request_overdub_.store(true, std::memory_order_relaxed); }
void LooperEngine::execute_loop_command() { request_looping_.store(true, std::memory_order_relaxed); }
void LooperEngine::execute_clear_command() { request_clear_.store(true, std::memory_order_relaxed); }

// [חדש] פונקציית כתיבת הסטטוס
void LooperEngine::set_detection_mode(int mode) {
    detection_mode_.store(mode, std::memory_order_relaxed);
    std::cout << "[STATE] Detection mode updated to: " << mode << std::endl;
}

float LooperEngine::get_estimated_bpm() const {
    return estimated_bpm_.load(std::memory_order_relaxed);
}

float LooperEngine::get_loop_beats() const {
    return loop_beats_.load(std::memory_order_relaxed);
}

float LooperEngine::get_loop_position() const {
    // נעילה קצרה מול ה-Worker: מונעת קריאת size() של וקטור שנמצא באמצע move/assign
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    int active_idx = active_playback_idx_.load(std::memory_order_acquire);
    const auto& active_buffer = playback_buffers_[active_idx];
    size_t buffer_size = active_buffer.size();

    if (buffer_size == 0) return 0.0f;

    size_t current_idx = playback_read_idx_.load(std::memory_order_relaxed);
    return static_cast<float>(current_idx) / static_cast<float>(buffer_size);
}

float LooperEngine::get_loop_seconds() const {
    // אותה נעילה קצרה כמו get_loop_position: הווקטור עשוי להיות באמצע swap.
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    size_t n = playback_buffers_[active_playback_idx_.load(std::memory_order_acquire)].size();
    if (n == 0 || config_.sample_rate <= 0) return 0.0f;
    return static_cast<float>(n) / static_cast<float>(config_.sample_rate);
}

void LooperEngine::update_hardware_config(int sample_rate, float latency_seconds) {
    // רישום בלבד — ה-Worker מחיל את השינוי בבטחה בתחילת האיטרציה שלו.
    // שינוי ישיר של config_ או resize של preroll_buffer_ מכאן הוא Data Race.
    if (sample_rate > 0) pending_sample_rate_.store(sample_rate, std::memory_order_relaxed);
    pending_preroll_seconds_.store(latency_seconds + 0.02f, std::memory_order_relaxed);
}

void LooperEngine::on_audio_callback(const float* input_data, size_t num_frames) {
    // Wait-Free: דחיפה ללא נעילה; שמיטה שקטה כשהתור מלא (המדיניות היחידה שאינה
    // חוסמת את Thread ה-RT). סופרים את הנשמטות למונה אטומי — הנתיב היחיד שמעדכן
    // אותו הוא זה שכמעט אף פעם לא נלקח (חוצץ 5 שניות), ולכן העלות היא אפס בפועל.
    size_t dropped = 0;
    for (size_t i = 0; i < num_frames; ++i)
        if (!input_queue_.push(input_data[i])) ++dropped;
    if (dropped)
        input_overrun_count_.fetch_add(static_cast<uint32_t>(dropped), std::memory_order_relaxed);
}

void LooperEngine::process_output_callback(float* output_data, size_t num_frames) {
    LooperState state = current_state_.load(std::memory_order_relaxed);
    bool playing = (state == LooperState::LOOPING || state == LooperState::OVERDUBBING);
    // טרנספורט: שני דגלים בלבד על נתיב הפלט (ראה LooperEngine.hpp).
    const bool stopped = transport_stopped_.load(std::memory_order_relaxed);
    const bool muted   = output_muted_.load(std::memory_order_relaxed);

    size_t loop_pos = 0, loop_size = 0;   // ל-lock המטרונום בזמן נגינה

    if (!playing) {
        for (size_t i = 0; i < num_frames; ++i) output_data[i] = 0.0f;
    } else {
        int active_idx = active_playback_idx_.load(std::memory_order_acquire);
        // איתות Grace: ה-Worker רשאי לכתוב לחוצץ הלא-פעיל רק אחרי שנצפינו כאן על
        // הפעיל. חייב לרוץ *גם* בעצירה — אחרת הפרסום נחסם והמנוע קופא בזמן STOP.
        reader_observed_idx_.store(active_idx, std::memory_order_release);
        const auto& active_buffer = playback_buffers_[active_idx];

        if (active_buffer.empty() || stopped) {
            for (size_t i = 0; i < num_frames; ++i) output_data[i] = 0.0f;
            // חניית ראש-הקריאה בראש הלופ: ה-Output הוא הכותב היחיד של האינדקס,
            // ולכן ה"ריווינד" חייב לקרות כאן ולא ב-Thread של ה-UI.
            if (stopped) playback_read_idx_.store(0, std::memory_order_relaxed);
        } else {
            size_t buffer_size = active_buffer.size();
            size_t current_idx = playback_read_idx_.load(std::memory_order_relaxed);
            if (current_idx >= buffer_size) [[unlikely]] current_idx = 0;
            loop_pos = current_idx; loop_size = buffer_size;
            for (size_t i = 0; i < num_frames; ++i) {
                // MUTE משתיק את הפלט אך *ממשיך להתקדם* — הפאזה נשמרת, וההסרה
                // מחזירה את המשתמש פנימה בדיוק בקצב (זו כל הנקודה מול STOP).
                output_data[i] = muted ? 0.0f : active_buffer[current_idx];
                current_idx++;
                if (current_idx >= buffer_size) [[unlikely]] current_idx = 0;
            }
            playback_read_idx_.store(current_idx, std::memory_order_relaxed);
        }
    }

    // (אין DSP נוסף על נתיב הפלט: הריברב פר-שכבה נצרב מראש לתוך המיקס על-ידי
    // ה-Worker. הקולבק קורא חוצץ מוכן ומוסיף מטרונום בלבד — מינימום עבודה ב-RT.)
    // בעצירה playing=false ⇒ שער המטרונום סוגר מעצמו (עצירה = שקט מוחלט);
    // בהשתקה הוא נשאר פתוח — הקליק הוא הרפרנס שמחזיר את הנגן בקצב.
    render_metronome(output_data, num_frames, state, playing && !stopped, loop_pos, loop_size);

    // --- עוצמת ניטור (Master), אחרונה בשרשרת ---
    // חלה על *כל* היציאה כולל הקליק: זו עוצמת האפליקציה, ולולא כן הורדת הלופ
    // הייתה הופכת את המטרונום לדומיננטי. מוחלקת בחד-קוטבי (~15ms) כדי שגרירת
    // הסליידר לא תייצר מדרגות-גיין. כשהעוצמה מלאה והחלקתה התכנסה — אפס עבודה.
    const float mv_target = master_volume_.load(std::memory_order_relaxed);
    if (mv_target != 1.0f || master_volume_smoothed_ != 1.0f) {
        const float sr = static_cast<float>(config_.sample_rate);
        const float a = 1.0f - std::exp(-1.0f / (0.015f * (sr > 0.0f ? sr : 48000.0f)));
        for (size_t i = 0; i < num_frames; ++i) {
            master_volume_smoothed_ += (mv_target - master_volume_smoothed_) * a;
            output_data[i] *= master_volume_smoothed_;
        }
        // קיבוע מדויק: החד-קוטבי מתקרב אסימפטוטית בלבד, והשארית הייתה מחזיקה
        // את המסלול היקר הזה דלוק לנצח אחרי חזרה ל-100%.
        if (std::abs(mv_target - master_volume_smoothed_) < 1e-4f)
            master_volume_smoothed_ = mv_target;
    }
}

// מטרונום/Count-in למצב SYNC. רץ על ה-Thread של ה-Output. בזמן נגינה גזור הקצב
// *ממיקום הלופ* (נעילה מושלמת ללא סחיפה); ב-IDLE השתמש במונה חופשי (ספירה-לתוך).
void LooperEngine::render_metronome(float* out, size_t num_frames, LooperState state,
                                    bool playing, size_t loop_pos, size_t loop_size) {
    if (detection_mode_.load(std::memory_order_relaxed) != 2) return;          // מצב SYNC בלבד
    if (!metronome_user_enabled_.load(std::memory_order_relaxed)) return;      // כיבוי משתמש
    if (!(state == LooperState::IDLE || playing)) return;                      // לא ב-REC/CAL/PROC

    float bpm = target_bpm_.load(std::memory_order_relaxed);
    if (bpm < 20.0f || bpm > 400.0f) return;
    double beat_period = (60.0 / bpm) * static_cast<double>(config_.sample_rate);
    if (beat_period < 1.0) return;
    const float click_decay = std::exp(-1.0f / (0.018f * config_.sample_rate));

    for (size_t i = 0; i < num_frames; ++i) {
        double pos;
        if (playing && loop_size > 0) {
            pos = static_cast<double>((loop_pos + i) % loop_size);   // נעול ללופ
        } else {
            pos = metro_free_counter_;
            metro_free_counter_ += 1.0;
        }
        long beat = static_cast<long>(pos / beat_period);
        if (beat != metro_last_beat_) {
            metro_last_beat_ = beat;
            bool downbeat = (beat % 4 == 0);       // 4/4 — פעימה 1 מודגשת
            click_freq_ = downbeat ? 1600.0f : 1000.0f;
            click_env_  = downbeat ? 0.32f : 0.20f;
            click_phase_ = 0.0;
        }
        if (click_env_ > 0.001f) {
            // גל sin מטבלה עם אינטרפולציה לינארית — מחליף std::sin פר-דגימה.
            // click_phase_ ב-[0,1) מייצג מחזור; מדד הטבלה = phase · גודל.
            float t = static_cast<float>(click_phase_) * kClickTableSize;
            int i0 = static_cast<int>(t);
            float frac = t - static_cast<float>(i0);
            i0 &= (kClickTableSize - 1);
            int i1 = (i0 + 1) & (kClickTableSize - 1);
            float s = (click_sine_[i0] + frac * (click_sine_[i1] - click_sine_[i0])) * click_env_;
            out[i] = std::clamp(out[i] + s, -1.0f, 1.0f);
            click_phase_ += click_freq_ / static_cast<double>(config_.sample_rate);   // מחזורים/דגימה
            if (click_phase_ >= 1.0) click_phase_ -= 1.0;                             // עטיפה ([0,1))
            click_env_ *= click_decay;
        }
    }
}

LooperState LooperEngine::get_current_state() const { return current_state_.load(std::memory_order_relaxed); }

float LooperEngine::calculate_rms(const std::vector<float>& chunk) {
    if (chunk.empty()) return 0.0f;
    float sum_squares = std::inner_product(chunk.begin(), chunk.end(), chunk.begin(), 0.0f);
    return std::sqrt(sum_squares / chunk.size());
}

size_t LooperEngine::find_true_onset(const std::vector<float>& audio_data, size_t max_search_samples, float threshold) {
    for (size_t i = 0; i < max_search_samples && i < audio_data.size(); ++i) {
        if (std::abs(audio_data[i]) > threshold) {
            size_t back_scan = i;
            float min_val = std::abs(audio_data[back_scan]);
            while (back_scan > 0 && (i - back_scan) < 2000) {
                back_scan--;
                float current_val = std::abs(audio_data[back_scan]);
                if (current_val > min_val) return back_scan + 1;
                min_val = current_val;
                if (min_val < threshold * 0.05f) break;
            }
            return back_scan;
        }
    }
    return 0;
}

// עקומת Novelty במרחב התדר (Spectral Flux על חלונות קופצים) — משמשת את
// אומדן הטמפו בלבד. (ה-Flux הזורם פר-צ'אנק הוסר: נמדד ונפסל כמבחין התקף.)
std::vector<float> LooperEngine::extract_novelty_curve(const std::vector<float>& audio_data, int& out_chunk_size) {
    // גודל חלון ה-FFT. קובע את הרזולוציה בתדר מול הזמן.
    const int NFFT = kNoveltyNFFT;
    const int HOP_SIZE = NFFT / 2; // חפיפה של 50%

    // קביעת קצב הדגימה הווירטואלי של העקומה
    out_chunk_size = HOP_SIZE;

    std::vector<float> novelty;
    if (audio_data.size() < static_cast<size_t>(NFFT)) return novelty;

    novelty.reserve(audio_data.size() / HOP_SIZE);

    // חוצצי ה-FFT (config, חלון Hann, כניסה, יציאה) מוקצים בבנאי ונעשה בהם
    // שימוש-חוזר — כאן רק מאפסים את מגניטודת-החלון-הקודם, שמצטברת פר-קריאה.
    // ה-Worker הוא הקורא היחיד, ולכן אין צורך בסנכרון.
    std::fill(novelty_prev_mag_.begin(), novelty_prev_mag_.end(), 0.0f);

    // מעבר על האודיו בחלונות קופצים
    for (size_t i = 0; i + NFFT <= audio_data.size(); i += HOP_SIZE) {
        // 1. החלת חלון (Hann Window) למניעת זליגת תדרים בקצוות
        for (int j = 0; j < NFFT; ++j) {
            novelty_time_in_[j] = audio_data[i + j] * novelty_hann_[j];
        }

        // 2. ביצוע התמרת פורייה
        kiss_fftr(novelty_fft_cfg_, novelty_time_in_.data(), novelty_freq_out_.data());

        float current_flux = 0.0f;

        // 3. חישוב ה-Spectral Flux (המשוואה המדוברת)
        for (int k = 0; k <= NFFT / 2; ++k) {
            // חישוב מגניטודה של המספר המרוכב
            float real = novelty_freq_out_[k].r;
            float imag = novelty_freq_out_[k].i;
            float magnitude = std::sqrt(real * real + imag * imag);

            // חישוב ההפרש בין החלון הנוכחי לקודם
            float diff = magnitude - novelty_prev_mag_[k];

            // Half-wave rectification (הוספת אנרגיה חדשה בלבד)
            if (diff > 0.0f) {
                current_flux += diff;
            }

            novelty_prev_mag_[k] = magnitude;
        }

        novelty.push_back(current_flux);
    }

    return novelty;
}

float LooperEngine::quantize_to_musical_phrase(float raw_beats) {
    float nearest_int = std::round(raw_beats);
    if (nearest_int < 2.0f) return 2.0f;
    float candidates[] = { std::round(raw_beats / 4.0f) * 4.0f, std::round(raw_beats / 2.0f) * 2.0f, nearest_int };
    float best_match = nearest_int;
    float min_penalty = 1e9f;
    for (float candidate : candidates) {
        if (candidate < 2.0f) continue;
        float deviation = std::abs(raw_beats - candidate);
        float penalty = deviation;
        if (std::fmod(candidate, 4.0f) == 0.0f) penalty *= 0.5f;
        else if (std::fmod(candidate, 2.0f) == 0.0f) penalty *= 0.8f;
        else penalty *= 1.5f;
        if (penalty < min_penalty) { min_penalty = penalty; best_match = candidate; }
    }
    return best_match;
}

// אומדן אורך פעימה באוטוקורלציה של עקומת ה-Novelty.
// מחליף את היסטוגרמת הזוגות הדלילה: האוטוקורלציה משתמשת בעקומה *כולה*
// (חסינה לפספוס/עודף פיקים), ההצבעה ההרמונית פותרת את דו-המשמעות האוקטבית
// (פעימה מול חצי/כפל פעימה), ואינטרפולציה פרבולית נותנת רזולוציה תת-צ'אנקית
// (הצ'אנק לבדו = ±1.4% שגיאת טמפו שמצטברת לשניות בלופים ארוכים).
float LooperEngine::extract_beat_length_from_onsets(const std::vector<float>& audio_data, size_t analysis_samples,
                                                    size_t* last_onset_samples) {
    const float fallback = (60.0f / 120.0f) * config_.sample_rate;
    auto give_up = [&]() { estimated_bpm_.store(120.0f, std::memory_order_relaxed);
                           if (last_onset_samples) *last_onset_samples = 0; return fallback; };

    if (analysis_samples > audio_data.size()) analysis_samples = audio_data.size();
    if (analysis_samples < static_cast<size_t>(config_.sample_rate)) return give_up();

    // Novelty רק על הקטע המוזיקלי (זנב הד/שקט מדלל את הקורלציה)
    std::vector<float> segment(audio_data.begin(), audio_data.begin() + analysis_samples);
    int hop = 0;
    std::vector<float> novelty = extract_novelty_curve(segment, hop);
    if (hop == 0 || novelty.size() < 32) return give_up();
    const int N = static_cast<int>(novelty.size());

    // ההתקף האחרון: הפיק המשמעותי האחרון בעקומת ה-Novelty. מפריד בין סיום
    // "מוכה" (פיק סמוך לקצה → המדידה מדויקת) לסיום "מצלצל" (הפיק האחרון הרחק
    // מהקצה, השאר דעיכה → המדידה קצרה ויש להשלים את התיבה).
    if (last_onset_samples) {
        float nov_mean = std::accumulate(novelty.begin(), novelty.end(), 0.0f) / N;
        float onset_thr = nov_mean * 1.5f;
        int last_peak = 0;
        for (int i = 1; i < N - 1; ++i)
            if (novelty[i] > onset_thr && novelty[i] >= novelty[i - 1] && novelty[i] > novelty[i + 1])
                last_peak = i;
        *last_onset_samples = static_cast<size_t>(last_peak) * hop;
    }

    // הסרת מגמה איטית (ממוצע-נע ~0.5s): משאירה את מבנה הפעימות בלבד,
    // בלי שהמעטפת האיטית של הביצוע תזלוג לאוטוקורלציה.
    float env_sr = static_cast<float>(config_.sample_rate) / hop;
    int trend_win = std::max(4, static_cast<int>(env_sr * 0.5f));
    std::vector<float> detrended(N);
    {
        double running = 0.0;
        std::vector<double> prefix(N + 1, 0.0);
        for (int i = 0; i < N; ++i) { running += novelty[i]; prefix[i + 1] = running; }
        for (int i = 0; i < N; ++i) {
            int lo = std::max(0, i - trend_win / 2);
            int hi = std::min(N, i + trend_win / 2 + 1);
            detrended[i] = novelty[i] - static_cast<float>((prefix[hi] - prefix[lo]) / (hi - lo));
        }
    }

    // טווח החיפוש: 240 עד 45 BPM, מורחב ×2 לצורך ההצבעה ההרמונית
    int min_lag = std::max(2, static_cast<int>(std::floor((60.0f / 240.0f) * env_sr)));
    int max_lag = static_cast<int>(std::ceil((60.0f / 45.0f) * env_sr));
    int ext_lag = std::min(2 * max_lag + 1, N - 2);
    if (max_lag > ext_lag) max_lag = ext_lag;
    if (max_lag <= min_lag + 2) return give_up();

    // אוטוקורלציה מנורמלת ולא-מוטה (חלוקה ב-N-lag)
    double ac0 = 0.0;
    for (int i = 0; i < N; ++i) ac0 += static_cast<double>(detrended[i]) * detrended[i];
    ac0 /= N;
    if (ac0 < 1e-12) return give_up();
    std::vector<float> ac(ext_lag + 1, 0.0f);
    for (int lag = min_lag; lag <= ext_lag; ++lag) {
        double sum = 0.0;
        for (int i = 0; i + lag < N; ++i) sum += static_cast<double>(detrended[i]) * detrended[i + lag];
        ac[lag] = static_cast<float>((sum / (N - lag)) / ac0);
    }

    auto ac_at = [&](float lag) -> float {
        if (lag < min_lag || lag > ext_lag - 1) return 0.0f;
        int i = static_cast<int>(lag);
        float frac = lag - i;
        return ac[i] * (1.0f - frac) + ac[i + 1] * frac;
    };

    // בחירת המועמד: מקסימום מקומי בטווח הבסיסי, עם חיזוק הרמוני —
    // פעימה אמיתית נתמכת גם על ידי הכפולות שלה (2L, 3L).
    int best = -1;
    float best_score = -1e9f;
    for (int lag = min_lag + 1; lag < max_lag; ++lag) {
        if (ac[lag] <= ac[lag - 1] || ac[lag] < ac[lag + 1]) continue;   // לא פיק
        float score = ac[lag] + 0.5f * ac_at(2.0f * lag) + 0.25f * ac_at(3.0f * lag);
        if (score > best_score) { best_score = score; best = lag; }
    }
    if (best < 0) return give_up();

    // עידון פרבולי סביב הפיק — רזולוציה תת-צ'אנקית
    float refined = static_cast<float>(best);
    {
        float y0 = ac[best - 1], y1 = ac[best], y2 = ac[best + 1];
        float denom = y0 - 2.0f * y1 + y2;
        if (std::abs(denom) > 1e-12f) {
            float delta = 0.5f * (y0 - y2) / denom;
            if (delta > -0.5f && delta < 0.5f) refined += delta;
        }
    }

    // ירידה לרשת העדינה (Tatum) כשהיא חזקה באמת: יחידה עדינה מקסימלית מבטיחה
    // שכל אורך-פרייז שלם ניתן לייצוג (7 פעימות אינו כפולה של פעימה-כפולה).
    while (refined / 2.0f >= min_lag && ac_at(refined / 2.0f) >= 0.8f * ac_at(refined)) {
        refined /= 2.0f;
    }

    float beat_samples = refined * hop;
    float calculated_bpm = 60.0f / (beat_samples / config_.sample_rate);
    estimated_bpm_.store(calculated_bpm, std::memory_order_relaxed);
    return beat_samples;
}

void LooperEngine::execute_record_start_command() {
    request_record_start_.store(true, std::memory_order_relaxed);
}

void LooperEngine::execute_record_stop_command() {
    request_record_stop_.store(true, std::memory_order_relaxed);
}

// תפר קצה→ראש בקיפול — אותו עיקרון כמו קיפול-הזנב של PROCESSING שנמדד נקי,
// אבל עם שני לקחים שנמדדו ביוקר על חיתוך TAP באמצע נגינה חזקה:
//  (1) המשך חייב להיות *ארוך* — קיפול 256 דגימות (5.8ms) של תוכן חזק אל ראש
//      שקט = פרץ רחב-סרט (נמדד: click_ratio 18 — גרוע מה-Fade-לאפס הישן, 4.6).
//      2048 דגימות (~43ms) קוראות באוזן כדעיכה מוזיקלית, ועדיין מתחת לדיוק
//      התזמון האנושי של ההקשה (הקיצור הדטרמיניסטי נבלע ברעש המשתמש).
//  (2) הדעיכה בצורת קוסינוס — נגזרת אפס בשני קצוות ה-Fade; שיפוע לינארי
//      פותח ב"ברך" שמרוחה כאנרגיה רחבת-סרט.
// שומר-הרציפות מדלג על כל טיפול כשהתפר כבר רציף (לופ מיובא מוכן — הישן
// היה הורס אותו עם Fade-לאפס).
std::vector<float> LooperEngine::apply_seam_fold(std::vector<float>& audio, size_t fold_samples) {
    if (audio.size() <= fold_samples * 4) return audio;
    const size_t N = audio.size();

    // שומר רציפות: אם קפיצת התפר אינה חריגה מצעד טיפוסי בין דגימות סמוכות
    // בסביבת התפר — הלופ כבר חלק; כל טיפול רק יזיק.
    {
        const size_t G = 1024;
        double mean_step = 0.0;
        for (size_t i = 0; i < G; ++i) {
            mean_step += std::abs(audio[i + 1] - audio[i]);                  // ראש
            mean_step += std::abs(audio[N - 1 - i] - audio[N - 2 - i]);      // זנב
        }
        mean_step /= (2.0 * G);
        if (std::abs(audio[0] - audio[N - 1]) <= 3.0 * mean_step + 1e-6) return audio;
    }

    const float PI = 3.14159265358979323846f;
    const size_t cut = N - fold_samples;
    std::vector<float> result(audio.begin(), audio.begin() + cut);
    for (size_t i = 0; i < fold_samples; ++i) {
        float progress = static_cast<float>(i) / static_cast<float>(fold_samples);
        float fade = 0.5f * (1.0f + std::cos(progress * PI));   // 1→0, נגזרת אפס בקצוות
        result[i] = std::clamp(result[i] + audio[cut + i] * fade, -1.0f, 1.0f);
    }
    return result;
}

// (apply_loop_effect — האפקט הגלובלי-לכל-הלופ — נמחק: מאז שלוח-האפקטים הראשי
// עבר לעריכת השכבה הנוכחית, לא נותר לו שום קורא ב-UI. האוקטבה משנת-האורך
// ההיסטורית קיימת ב-git; האוקטבה החיה היא octave_pitch שומרת-האורך פר-שכבה.)

int LooperEngine::get_loop_waveform(float* out, int max_bins) {
    if (!out || max_bins <= 0) return 0;
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    const auto& buf = playback_buffers_[active_playback_idx_.load(std::memory_order_acquire)];
    size_t n = buf.size();
    if (n == 0) return 0;
    for (int b = 0; b < max_bins; ++b) {
        size_t start = static_cast<size_t>(static_cast<double>(b) / max_bins * n);
        size_t end   = static_cast<size_t>(static_cast<double>(b + 1) / max_bins * n);
        if (end > n) end = n;
        float peak = 0.0f;
        for (size_t i = start; i < end; ++i) peak = std::max(peak, std::fabs(buf[i]));
        out[b] = peak;
    }
    return max_bins;
}

void LooperEngine::process_audio_asynchronously() {
    // הגנת דנורמלים — חייבת לרוץ על ה-Thread הזה עצמו
    enable_denormal_flush_to_zero();

    std::vector<float> local_chunk;
    local_chunk.reserve(config_.chunk_size);

    // ה-Preroll חייב לכסות את *חלון ההחלטה*: אנו מחליטים רק אחרי persistence
    // חלונות תהודה, כלומר ההקלטה מתחילה ~75ms אחרי ההתקף. אם ה-Preroll קצר
    // מכך (במכשיר לייטנסי החומרה הוא ~28ms בלבד), ההתקף עצמו יאבד. לכן
    // מרצפים את ה-Preroll ל-max(לייטנסי-חומרה, חלון-ההחלטה + מרווח).
    auto compute_preroll_samples = [&]() -> size_t {
        size_t from_latency = static_cast<size_t>(config_.preroll_seconds * config_.sample_rate);
        size_t decision_cover = static_cast<size_t>(
            onset_persistence_target_.load(std::memory_order_relaxed) + 3) * config_.chunk_size;
        size_t s = std::max(from_latency, decision_cover);
        return s < 1 ? 1 : s;
    };
    size_t max_preroll_samples = compute_preroll_samples();
    preroll_buffer_.assign(max_preroll_samples, 0.0f);
    preroll_write_idx_ = 0;

    std::vector<float> recorded_audio;
    recorded_audio.reserve(config_.sample_rate * 60);
    size_t samples_since_articulation = 0;   // מונה נטישת-הדרון (Auto בלבד)

    // --- מודל רב-מסלול (Worker-owned) ---
    // layers_ הוא מקור-האמת: הבסיס (0) + כל אוברדאב כשכבה. המיקס מחושב-מחדש
    // ומתפרסם לחוצץ ה-RCU בכל שינוי (בסיס/אוברדאב/מחיקה/אפקט). קורא-הרמקול
    // ממשיך לקרוא חוצץ *יחיד* מעורב-מראש — נתיב ה-Output לא משתנה כלל.
    std::vector<Layer> layers_;
    size_t loop_length_ = 0;              // נקבע בעת קיבוע הבסיס; אינוריאנטה לכל שכבה
    std::vector<float> committed_mix_;    // סכום גולמי של השכבות המקובעות (בסיס למוניטור אוברדאב)
    std::vector<float> new_layer_;        // שכבת האוברדאב שבתהליך
    std::vector<float> mix_temp_;         // סקראץ' לרינדור לפני פרסום (מזעור זמן-נעילה)
    size_t overdub_idx = 0;
    bool overdub_publish_pending = false;
    // אוברדאב מוצמד-לראש-הלופ: ממתין מרגע הנקישה עד העטיפה הבאה.
    bool overdub_armed = false;
    size_t armed_prev_play_idx = 0;   // לזיהוי העטיפה (ירידת ראש-הקריאה)

    // ביטול-מחיקה בעומק 1 (Worker-owned). מוחזק בשלמותו כדי שהשחזור יחזיר את
    // השכבה *כפי שהייתה* — כולל fx/gain/reverb/clean, לא רק את האודיו.
    Layer undo_layer_;
    int   undo_layer_index_ = -1;

    // ספירה-לתוך (SYNC): דגימות שנותרו עד פתיחת ההקלטה.
    size_t count_in_remaining = 0;

    // ווטואים רצופים על תוכן-מזערי — מונה הריפוי-העצמי (ראה PROCESSING).
    int consecutive_vetoes = 0;

    // --- פרסום חלק (נטול-קליק) בשני שלבים לעריכות שכבה שומרות-אורך ---
    // מחיקה/אפקט/עוצמה מחליפים את המיקס בבת-אחת; בלי טיפול, בנקודת הקריאה נוצרת
    // מדרגת-תוכן (הפרש בין המיקס הישן לחדש) — קליק רחב-סרט כשמוחקים שכבה חזקה
    // באמצע צליל. שלב 1 מפרסם חוצץ שמשמר את התוכן הישן סביב ראש-הקריאה ונמזג
    // לחדש קדימה ממנו; שלב 2 (אחרי שהקורא חלף על אזור-הטלאי) מפרסם את המיקס
    // הטהור — אחרת הטלאי (תוכן ישן) היה מתנגן שוב בכל סיבוב.
    std::vector<float> pure_mix_pending;  // מטען שלב-2 (המיקס הטהור); ריק = אין ממתין
    int pure_publish_countdown = 0;       // צ'אנקים עד שלב 2

    // מסכת ההאזנה שהוחלה בפועל (Worker-owned). כל מרנדר מדלג על שכבה ממוסכת,
    // בלי לגעת ב-gain שלה — ה-gain נשאר של המשתמש בכל רגע.
    uint32_t active_mask = 0;
    int last_mirrored_layer_count = -1;
    auto masked = [&](int k) { return (active_mask >> k) & 1u; };

    // סכום גולמי (ללא ברך) של כל השכבות המקובעות → committed_mix_ (בסיס מוניטור אוברדאב).
    auto render_committed_raw = [&]() {
        committed_mix_.assign(loop_length_, 0.0f);
        for (int k = 0; k < static_cast<int>(layers_.size()); ++k) {
            const Layer& L = layers_[k];
            if (masked(k) || L.gain == 0.0f || L.samples.size() != loop_length_) continue;
            for (size_t i = 0; i < loop_length_; ++i) committed_mix_[i] += L.samples[i] * L.gain;
        }
    };
    // מיקס עם ברך-רכה מתוך *וקטור-שכבות נתון* באורך len → dst (לפרסום אפקט גלובלי).
    auto render_from = [&](const std::vector<Layer>& src, size_t len, std::vector<float>& dst) {
        dst.assign(len, 0.0f);
        for (const Layer& L : src) {
            if (L.gain == 0.0f || L.samples.size() != len) continue;
            for (size_t i = 0; i < len; ++i) dst[i] += L.samples[i] * L.gain;
        }
        for (size_t i = 0; i < len; ++i) dst[i] = soft_clip_knee(dst[i]);
    };
    // מיקס מלא (ברך-רכה) של השכבות הנוכחיות תוך דילוג על אינדקס אחד → dst (לפרסום מחיקה).
    auto render_full_mix_excluding = [&](int exclude, std::vector<float>& dst) {
        dst.assign(loop_length_, 0.0f);
        for (int k = 0; k < static_cast<int>(layers_.size()); ++k) {
            if (k == exclude || masked(k)) continue;
            const Layer& L = layers_[k];
            if (L.gain == 0.0f || L.samples.size() != loop_length_) continue;
            for (size_t i = 0; i < loop_length_; ++i) dst[i] += L.samples[i] * L.gain;
        }
        for (size_t i = 0; i < loop_length_; ++i) dst[i] = soft_clip_knee(dst[i]);
    };
    // בניית שכבה טרייה מאודיו גולמי: dry ו-samples זהים (אין fx עדיין), gain=1.
    auto make_layer = [](std::vector<float> audio) -> Layer {
        Layer L;
        L.samples = audio;             // העתקה
        L.dry = std::move(audio);      // אותו תוכן — dry הפריסטיני
        L.gain = 1.0f; L.fx = 0; L.reverb = 0.0f; L.denoise = false;
        return L;
    };
    // רינדור-מחדש של samples מתוך dry: clean(dry) → fx → reverb (שומר-אורך תמיד).
    // ה-CLEAN קודם ל-fx בכוונה: אוקטבת ה-Phase-Vocoder מורחת רעש רחב-סרט במיוחד,
    // וריברב על אות נקי נשמע נקי. השער יקר (~2×FFT על הלופ) — נשמר במטמון
    // dry_denoised ומחושב פעם אחת; dry עצמו לעולם לא נגוע (הפיכות מלאה).
    auto render_layer = [&](Layer& L) {
        const std::vector<float>* src = &L.dry;
        if (L.denoise) {
            if (L.dry_denoised.size() != loop_length_)
                L.dry_denoised = notap_dsp::denoise_loop(L.dry);
            if (L.dry_denoised.size() == loop_length_) src = &L.dry_denoised;
        } else if (!L.dry_denoised.empty()) {
            L.dry_denoised.clear();
            L.dry_denoised.shrink_to_fit();   // כבוי → שחרור המטמון (זיכרון לופ שלם)
        }
        std::vector<float> base;
        switch (L.fx) {
            case 1: base.assign(src->rbegin(), src->rend()); break;        // reverse (מדויק)
            case 2: base = notap_dsp::octave_pitch(*src, true);  break;    // +אוקטבה (PV שומר-אורך)
            case 3: base = notap_dsp::octave_pitch(*src, false); break;    // -אוקטבה
            default: base = *src; break;
        }
        if (base.size() != loop_length_) base = *src;   // הגנה: fx נכשל/אורך לא תואם
        if (L.reverb > 0.001f) base = bake_reverb_loop(base, L.reverb, config_.sample_rate);
        L.samples = std::move(base);
    };
    // מראה-מצב לכל השכבות (ספירה + fx/gain/reverb/clean) לקריאת ה-UI דרך JNI.
    auto sync_layer_mirror = [&]() {
        int n = static_cast<int>(layers_.size());
        // כל שינוי במספר השכבות מזיז אינדקסים (מחיקה/ביטול-מחיקה/אוברדאב חדש),
        // ומסכת האזנה מיושנת הייתה משתיקה פתאום את השכבה הלא-נכונה. איפוס על
        // *כל* שינוי-ספירה הוא הכלל הפשוט והבטוח היחיד.
        if (n != last_mirrored_layer_count) {
            last_mirrored_layer_count = n;
            active_mask = 0;
            layer_mute_mask_.store(0, std::memory_order_relaxed);
        }
        int denoised = 0;
        for (int i = 0; i < n && i < kMaxLayers; ++i) {
            layer_fx_[i].store(layers_[i].fx, std::memory_order_relaxed);
            layer_gain_[i].store(layers_[i].gain, std::memory_order_relaxed);
            layer_reverb_[i].store(layers_[i].reverb, std::memory_order_relaxed);
            if (layers_[i].denoise) ++denoised;
        }
        layer_denoise_count_.store(denoised, std::memory_order_relaxed);
        layer_count_.store(n, std::memory_order_release);   // פרסום אחרון → מצב עקבי לקורא
    };

    // מעטפת פיק דועכת — נשמרת לטלמטריה ולאבחון בלבד (סף השקט כבר לא תלוי בה)
    float peak_envelope = 0.0f;
    float env_release_coef = 1.0f;
    size_t max_record_samples = 0;
    size_t silence_samples_count = 0;      // מסלול א': שקט מוחלט רצוף
    size_t inactivity_count = 0;           // מסלול ב': זמן מאז שהפעילות המוזיקלית פסקה
    size_t trailing_non_musical_samples = 0; // הזנב הלא-מוזיקלי בעת סגירה (לקוונטיזציה)
    size_t last_published_loop_samples = 0;  // אורך הלופ שפורסם (לטלמטריה בלבד)
    float session_peak_raw = 0.0f;           // שיא ה-raw של ההקלטה הנוכחית (רצפת פעילות יחסית)
    float prev_trigger_volume = 0.0f;
    int publish_defer_count = 0;
    int buffer_release_countdown = 0;
    uint64_t total_samples_processed = 0;   // שעון-דגימות דטרמיניסטי לטלמטריה
    float sample;

    const float ENV_RELEASE_SECONDS = 1.5f;   // קבוע הזמן של שחרור המעטפת (טלמטריה)
    const float MAX_RECORD_SECONDS = 300.0f;  // רשת ביטחון נגד הקלטה אינסופית

    // --- שער המחזוריות (YIN) ---
    // רץ על חלון 2048 הדגימות האחרונות של ה-Preroll — עמוק בתוך התהודה, אחרי
    // ההתקף (persistence 12 ≈ 6144 דגימות זמינות). f0 בטווח 70Hz-1kHz מכסה
    // גיטרה בכל כיוונון סביר. עלות ~1.4M מכפלות *בנקודת החלטה בלבד* — זניח.
    const int YIN_W = 2048;
    std::vector<float> yin_win(YIN_W);
    float current_yin = -1.0f;   // -1 = לא חושב בצ'אנק זה (טלמטריה)
    auto compute_yin_from_preroll = [&]() -> float {
        size_t W = std::min<size_t>(YIN_W, max_preroll_samples);
        for (size_t i = 0; i < W; ++i) {
            size_t idx = (preroll_write_idx_ + max_preroll_samples - W + i) % max_preroll_samples;
            yin_win[i] = preroll_buffer_[idx];
        }
        int tau_min = std::max(2, config_.sample_rate / 1000);
        int tau_max = std::max(tau_min + 8, config_.sample_rate / 70);
        return yin_periodicity(yin_win.data(), static_cast<int>(W), tau_min, tau_max);
    };

    // חוסם DC חד-קוטבי (~10Hz) על כל דגימת כניסה. InputPreset::Unprocessed עלול
    // לספק הטיית DC, והיא מרעילה את כל הסטטיסטיקה היחסית-לרעש: raw-RMS ≥ DC תמיד,
    // ולכן ממוצע הרעש לומד את *ההטיה* וכל הרצפות (×8, ×12) נבנות עליה.
    // נמדד: DC של 0.5% FS שובר עצירות (13/15 + 2 סרק); 2% FS קטסטרופלי —
    // recall 7/15, עצירות 1/15. ב-10Hz הגיטרה נקייה (E=82Hz: ‎-0.06dB) והתכנסות
    // צעד-ההטיה ~16ms — בתוך תקופת הכיול. גם ההקלטות/ייצוא יוצאים נקיי-DC.
    float dc_x1 = 0.0f, dc_y1 = 0.0f, dc_R = 1.0f;
    auto recompute_time_constants = [&]() {
        float chunk_seconds = static_cast<float>(config_.chunk_size) / static_cast<float>(config_.sample_rate);
        env_release_coef = std::exp(-chunk_seconds / ENV_RELEASE_SECONDS);
        max_record_samples = static_cast<size_t>(MAX_RECORD_SECONDS * config_.sample_rate);
        dc_R = 1.0f - (2.0f * 3.14159265358979323846f * 10.0f) / static_cast<float>(config_.sample_rate);
    };
    recompute_time_constants();

    // מותר לכתוב לחוצץ נגינה רק כשהקורא לא יכול להיות בתוכו:
    // או שאין נגינה כלל, או שהקורא נצפה לאחרונה על החוצץ *השני*.
    // מונה הדחיות חוסם המתנה אינסופית אם זרם הרמקול נתקע (ואז ממילא אין Race).
    auto acquire_writable_slot = [&]() -> int {
        int active = active_playback_idx_.load(std::memory_order_relaxed);
        LooperState s = current_state_.load(std::memory_order_relaxed);
        bool reader_running = (s == LooperState::LOOPING || s == LooperState::OVERDUBBING);
        if (!reader_running ||
            reader_observed_idx_.load(std::memory_order_acquire) == active ||
            ++publish_defer_count > 32) {
            publish_defer_count = 0;
            return 1 - active;
        }
        return -1;
    };

    // פרסום מיקס-שכבות מוכן (mix_temp_) אל חוצץ ה-RCU. reset_read=true מאפס את
    // מיקום הקריאה (אחרי שינוי אורך — אוקטבה/מחיקה-שמשנה-אורך); false שומר עליו
    // (מחיקה/אפקט שומרי-אורך → הלופ ממשיך בלי קפיצה). מחזיר false אם ה-Slot לא
    // פנוי עדיין (המתן וקרא שוב). ה-Worker הוא המפרסם היחיד — מודל ה-RCU נשמר.
    auto publish_mix_temp = [&](bool reset_read) -> bool {
        int slot = acquire_writable_slot();
        if (slot < 0) return false;
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            playback_buffers_[slot].swap(mix_temp_);   // O(1); mix_temp_ יתמלא מחדש בשימוש הבא
        }
        if (reset_read) playback_read_idx_.store(0, std::memory_order_relaxed);
        active_playback_idx_.store(slot, std::memory_order_release);
        return true;
    };

    // ביטול שלב-2 ממתין — בכל אירוע שהופך אותו לישן (Clear/Import/בסיס חדש/אוברדאב).
    auto cancel_pending_pure = [&]() {
        pure_mix_pending.clear();
        pure_publish_countdown = 0;
    };

    // איפוס טרנספורט: חובה בכל אירוע שמוליד לופ *חדש* (בסיס חדש/Import/Session)
    // או מוחק את הקיים. בלעדיו, משתמש שעצר לופ ואז ניקה והקליט מחדש היה מקבל
    // לופ חדש שנולד אילם — והכפתור היחיד שמסביר זאת כבר לא על המסך.
    // מיקס מלא של השכבות הנוכחיות *בתוספת* שכבה נתונה — לפרסום ביטול-מחיקה
    // בלי לגעת ב-layers_ לפני שהפרסום הצליח (אותו חוזה כמו מסלול המחיקה).
    auto render_full_mix_including = [&](const Layer& extra, std::vector<float>& dst) {
        dst.assign(loop_length_, 0.0f);
        for (int k = 0; k < static_cast<int>(layers_.size()); ++k) {
            const Layer& L = layers_[k];
            if (masked(k) || L.gain == 0.0f || L.samples.size() != loop_length_) continue;
            for (size_t i = 0; i < loop_length_; ++i) dst[i] += L.samples[i] * L.gain;
        }
        if (extra.gain != 0.0f && extra.samples.size() == loop_length_)
            for (size_t i = 0; i < loop_length_; ++i) dst[i] += extra.samples[i] * extra.gain;
        for (size_t i = 0; i < loop_length_; ++i) dst[i] = soft_clip_knee(dst[i]);
    };

    // ביטול-המחיקה שייך ללופ שממנו נמחקה השכבה. כל אירוע שמחליף את הלופ
    // (clear/import/session/בסיס חדש) הופך אותו לחסר-משמעות — ומסוכן, כי האורך
    // כבר לא תואם.
    auto drop_undo = [&]() {
        undo_layer_ = Layer{};
        undo_layer_index_ = -1;
        undo_available_.store(false, std::memory_order_relaxed);
    };

    auto reset_transport = [&]() {
        transport_stopped_.store(false, std::memory_order_relaxed);
        output_muted_.store(false, std::memory_order_relaxed);
        // בקשת שכבה ממתינה שייכת ללופ שממנו נולדה; לופ חדש/מחיקה מבטלים אותה,
        // אחרת השכבה הייתה נכנסת מעצמה לתוך לופ אחר לגמרי.
        overdub_armed = false;
        overdub_armed_.store(false, std::memory_order_relaxed);
        drop_undo();
        count_in_remaining = 0;
        count_in_beats_left_.store(0, std::memory_order_relaxed);
    };

    // פרסום נטול-קליק של mix_temp_ (שלב 1) עבור עריכות שומרות-אורך במצב LOOPING.
    //   [rd, rd+MARGIN):        תוכן ישן — הקורא יימצא כאן ברגע ההחלפה (מדרגה=0)
    //   [rd+MARGIN, +FADE):     מיזוג קוסינוס ישן→חדש (אפס-נגזרת בקצוות)
    //   שאר החוצץ:              המיקס החדש
    // שלב 2 מתוזמן ב-pure_publish_countdown. לופ קצר מכפל-הטלאי → פרסום רגיל
    // (הטלאי היה עוטף את עצמו). הקורא לעולם לא נחסם — כל המיזוג נבנה כאן, ב-Worker.
    auto publish_mix_smoothed = [&]() -> bool {
        const size_t len = loop_length_;
        const size_t MARGIN = 4 * static_cast<size_t>(config_.chunk_size);  // מרווח התקדמות-קורא + דחיית-Slot
        const size_t FADE = 1024;                                           // ‎~21ms @48k
        int active = active_playback_idx_.load(std::memory_order_relaxed);
        // ה-Worker הוא הממוטט היחיד של playback_buffers_ — קריאה מכאן בטוחה ללא נעילה
        const std::vector<float>& cur = playback_buffers_[active];
        if (cur.size() != len || mix_temp_.size() != len || len < 2 * (MARGIN + FADE)) {
            return publish_mix_temp(/*reset_read=*/false);   // בלי החלקה — קצר/לא-תואם
        }
        pure_mix_pending = mix_temp_;   // מטען שלב-2: המיקס הטהור, לפני המיזוג-במקום
        const size_t rd = playback_read_idx_.load(std::memory_order_relaxed) % len;
        for (size_t k = 0; k < MARGIN; ++k) {
            size_t j = (rd + k) % len;
            mix_temp_[j] = cur[j];
        }
        const float PI_F = 3.14159265358979323846f;
        for (size_t k = 0; k < FADE; ++k) {
            size_t j = (rd + MARGIN + k) % len;
            float w = 0.5f * (1.0f - std::cos(PI_F * static_cast<float>(k) / FADE));  // 0→1
            mix_temp_[j] = cur[j] * (1.0f - w) + mix_temp_[j] * w;
        }
        if (publish_mix_temp(/*reset_read=*/false)) {
            // שלב 2 אחרי שהקורא ודאי חלף על הטלאי (+שוליים)
            pure_publish_countdown =
                static_cast<int>((MARGIN + FADE) / config_.chunk_size) + 6;
            return true;
        }
        cancel_pending_pure();   // הפרסום נדחה — הקורא זז; נחשב מחדש בניסיון הבא
        return false;
    };

    while (is_running_.load(std::memory_order_relaxed)) {

        // --- החלת בקשות קונפיגורציית חומרה (נרשמות מ-Threads אחרים, מוחלות רק כאן) ---
        int new_sr = pending_sample_rate_.exchange(-1, std::memory_order_relaxed);
        float new_preroll = pending_preroll_seconds_.exchange(-1.0f, std::memory_order_relaxed);
        if (new_sr > 0 && new_sr != config_.sample_rate) {
            config_.sample_rate = new_sr;
            noise_tracker_.reconfigure(new_sr, config_.chunk_size, 1.0f);
            raw_noise_tracker_.reconfigure(new_sr, config_.chunk_size, 1.0f);
            recompute_time_constants();
            // קצב דגימה חדש מבטל את תוקף הסטטיסטיקה — לומדים את החדר מחדש
            LooperState s = current_state_.load(std::memory_order_relaxed);
            if (s == LooperState::CALIBRATING || s == LooperState::IDLE) {
                current_state_.store(LooperState::CALIBRATING, std::memory_order_release);
            }
            if (new_preroll <= 0.0f) new_preroll = config_.preroll_seconds; // חישוב מחדש של ה-Preroll בקצב החדש
            std::cout << "[DSP] Engine reconfigured to hardware sample rate: " << new_sr << std::endl;
        }
        if (new_preroll > 0.0f) {
            config_.preroll_seconds = new_preroll;
            max_preroll_samples = compute_preroll_samples();
            preroll_buffer_.assign(max_preroll_samples, 0.0f);
            preroll_write_idx_ = 0;
            std::cout << "[DSP] Dynamic Preroll calibrated to: "
                      << (static_cast<float>(max_preroll_samples) / config_.sample_rate)
                      << "s (hw " << config_.preroll_seconds << "s + decision window)." << std::endl;
        }

        // --- הצהרה בדיעבד על אורך הלופ בתיבות ---
        // לא נוגע באודיו: הלופ הוא מה שהנגן ניגן. משתנים רק הקצב ומספר הפעימות,
        // שמזינים את הקליק (נעול למיקום הלופ), את טיקי-הפעימה בטבעת ואת מצב
        // SYNC. זו התשובה ל"למה אני חייב להקיש קצב *לפני* שניגנתי".
        float req_bars = request_set_bars_.exchange(-1.0f, std::memory_order_relaxed);
        if (req_bars > 0.0f && loop_length_ > 0) {
            float beats = req_bars * 4.0f;
            float secs = static_cast<float>(loop_length_) / static_cast<float>(config_.sample_rate);
            loop_beats_.store(beats, std::memory_order_relaxed);
            if (secs > 0.01f) {
                float bpm = beats * 60.0f / secs;
                estimated_bpm_.store(bpm, std::memory_order_relaxed);
                target_bpm_.store(bpm, std::memory_order_relaxed);   // הקליק חייב להסכים
                std::cout << "[DSP] Loop declared " << req_bars << " bars -> "
                          << bpm << " BPM." << std::endl;
            }
        }

        // --- כיול-מחדש ידני של רעש החדר ---
        // חוקי רק כשאין אודיו בתעופה: ב-RECORDING היינו זורקים טייק חי, וב-LOOPING
        // המעבר ל-CALIBRATING היה משתיק את ההשמעה (הקורא פעיל רק ב-LOOP/OVERDUB).
        if (request_recalibrate_.exchange(false, std::memory_order_relaxed)) {
            LooperState s_now = current_state_.load(std::memory_order_relaxed);
            if (s_now == LooperState::IDLE || s_now == LooperState::CALIBRATING) {
                noise_tracker_.reconfigure(config_.sample_rate, config_.chunk_size, 1.0f);
                raw_noise_tracker_.reconfigure(config_.sample_rate, config_.chunk_size, 1.0f);
                current_onset_streak_ = 0;
                count_in_remaining = 0;   // the count belonged to the old room model
                count_in_beats_left_.store(0, std::memory_order_relaxed);
                current_state_.store(LooperState::CALIBRATING, std::memory_order_release);
                std::cout << "[DSP] Re-learning the room on request." << std::endl;
            }
        }

        // --- מסירת לופ מיובא (פוענח על JNI, מתפרסם רק כאן) ---
        if (has_pending_import_.load(std::memory_order_acquire)) {
            int slot = acquire_writable_slot();
            if (slot >= 0) {
                // הלופ המיובא הוא הבסיס המוגן (מסלול 0). מתחילים היסטוריית שכבות
                // חדשה, ואז מפרסמים אותו כמיקס (זהה לבסיס כי הוא השכבה היחידה).
                layers_.clear();
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    layers_.push_back(make_layer(std::move(pending_import_)));
                    pending_import_.clear();
                }
                loop_length_ = layers_[0].samples.size();
                sync_layer_mirror();
                cancel_pending_pure();   // מיקס ממתין מהלופ הקודם — ישן
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    playback_buffers_[slot] = layers_[0].samples;
                }
                has_pending_import_.store(false, std::memory_order_release);
                playback_read_idx_.store(0, std::memory_order_relaxed);
                active_playback_idx_.store(slot, std::memory_order_release);
                reset_transport();
                current_state_.store(LooperState::LOOPING, std::memory_order_release);
                std::cout << "[I/O] Imported loop injected to DSP." << std::endl;
            }
            // slot == -1: הקורא עוד לא נצפה — ננסה שוב באיטרציה הבאה
        }

        // --- שמירת סשן (בקשה מ-JNI; רק ה-Worker רואה את layers_) ---
        // רץ בכל מצב: הכתיבה (עשרות ms) חוסמת רק את ה-Worker — הקורא ממשיך לנגן
        // מהחוצץ המפורסם, ותור הכניסה (5s) סופג את ההשהיה. אוברדאב שבתהליך אינו
        // נשמר (רק שכבות מקובעות) — סמנטיקה של "מה שנעול נשמר".
        if (request_save_session_.exchange(false, std::memory_order_relaxed)) {
            std::string save_path;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                save_path = pending_save_path_;
            }
            bool ok = write_session_file(save_path, layers_, loop_length_, config_.sample_rate,
                                         estimated_bpm_.load(std::memory_order_relaxed),
                                         loop_beats_.load(std::memory_order_relaxed));
            session_save_result_.store(ok ? 1 : -1, std::memory_order_release);
            if (ok) std::cout << "[I/O] Session saved (" << layers_.size() << " layers)." << std::endl;
        }

        // --- החלת סשן שנטען (פוענח על JNI, מוחל ומפורסם רק כאן) ---
        // משחזר את *מבנה השכבות המלא*: dry לכל שכבה + gain/fx/reverb, עם רינדור
        // מחדש (render_layer צורב fx/reverb מה-dry) — לא מיקס משוטח. קצב-דגימה
        // שונה מהחומרה הנוכחית נדחה (אין ריסמפול; כמעט-בלתי-אפשרי באותו מכשיר).
        if (has_pending_session_.load(std::memory_order_acquire)) {
            int slot = acquire_writable_slot();
            if (slot >= 0) {
                std::vector<SessionLayer> sess;
                int sess_rate; float sess_bpm, sess_beats;
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    sess.swap(pending_session_);
                    sess_rate = pending_session_rate_;
                    sess_bpm = pending_session_bpm_;
                    sess_beats = pending_session_beats_;
                }
                has_pending_session_.store(false, std::memory_order_release);
                bool ok = (sess_rate == config_.sample_rate) && !sess.empty();
                bool started = false;   // האם נגענו ב-layers_ (להבחנת ניקוי מדחייה נקייה)
                if (ok) {
                    started = true;
                    layers_.clear();
                    loop_length_ = sess[0].dry.size();
                    for (SessionLayer& sl : sess) {
                        if (sl.dry.size() != loop_length_) { ok = false; break; }
                        Layer L;
                        L.dry = std::move(sl.dry);
                        L.gain = std::clamp(sl.gain, 0.0f, 2.0f);
                        L.fx = sl.fx;
                        L.reverb = std::clamp(sl.reverb, 0.0f, 1.0f);
                        L.denoise = sl.denoise;
                        render_layer(L);   // samples = clean/fx/reverb(dry) — צריבה טרייה
                        layers_.push_back(std::move(L));
                    }
                }
                if (ok && !layers_.empty()) {
                    cancel_pending_pure();
                    sync_layer_mirror();
                    render_full_mix_excluding(-1, mix_temp_);
                    {
                        std::lock_guard<std::mutex> lock(buffer_mutex_);
                        playback_buffers_[slot].swap(mix_temp_);
                    }
                    playback_read_idx_.store(0, std::memory_order_relaxed);
                    active_playback_idx_.store(slot, std::memory_order_release);
                    estimated_bpm_.store(sess_bpm, std::memory_order_relaxed);
                    loop_beats_.store(sess_beats, std::memory_order_relaxed);
                    reset_transport();
                    current_state_.store(LooperState::LOOPING, std::memory_order_release);
                    std::cout << "[I/O] Session restored (" << layers_.size() << " layers)." << std::endl;
                } else if (started) {
                    // נכשל *אחרי* שהתחלנו לבנות — layers_ במצב חלקי; מנקים לגמרי.
                    layers_.clear();
                    loop_length_ = 0;
                    sync_layer_mirror();
                    std::cout << "[I/O] Session rejected mid-build (broken lengths)." << std::endl;
                } else {
                    // דחייה נקייה (קצב זר) — שום דבר לא נגוע; המצב הקיים נשאר.
                    std::cout << "[I/O] Session rejected (sample-rate mismatch)." << std::endl;
                }
            }
            // slot == -1: הקורא עוד לא נצפה — ננסה שוב באיטרציה הבאה
        }

        if (request_clear_.exchange(false, std::memory_order_relaxed)) {
            reset_transport();
            recorded_audio.clear();
            layers_.clear();
            loop_length_ = 0;
            committed_mix_.clear();
            new_layer_.clear();
            overdub_publish_pending = false;
            cancel_pending_pure();
            sync_layer_mirror();
            current_onset_streak_ = 0;
            silence_samples_count = 0;
            inactivity_count = 0;
            trailing_non_musical_samples = 0;
            session_peak_raw = 0.0f;
            peak_envelope = 0.0f;
            prev_trigger_volume = 0.0f;
            estimated_bpm_.store(0.0f, std::memory_order_relaxed);
            loop_beats_.store(0.0f, std::memory_order_relaxed);
            // קודם עוצרים את הקורא (מעבר ל-IDLE), ומשחררים את החוצצים רק אחרי
            // תקופת חסד בזמן-אמת — כדי שקולבק שנמצא באמצע קריאה לא יישמט מתחתיו.
            current_state_.store(LooperState::IDLE, std::memory_order_release);
            buffer_release_countdown = 8;   // 8 צ'אנקים ≈ 85ms של זמן אמת
            continue;
        }

        // צבירה לגודל צ'אנק *קבוע*: הצ'אנק הוא יחידת ההחלטה הסטטיסטית של המנוע.
        // עיבוד צ'אנקים חלקיים (כפי שקרה קודם) הופך את היחידה לתלוית-תזמון:
        // Oboe מוסר Bursts של ~96–192 פריימים, כך שה-RMS, ה-σ והמשמעות של
        // persistence היו משתנים עם גודל ה-Burst — וכל כוונון במעבדה (צ'אנקים
        // של 512) היה מפסיק להיות תקף במכשיר. local_chunk נשמר בין איטרציות
        // עד שמתמלא במלואו; שום דגימה לא אובדת.
        while (local_chunk.size() < static_cast<size_t>(config_.chunk_size) && input_queue_.pop(sample)) {
            float blocked = sample - dc_x1 + dc_R * dc_y1;   // חוסם ה-DC
            dc_x1 = sample;
            dc_y1 = blocked;
            local_chunk.push_back(blocked);
        }

        if (local_chunk.size() < static_cast<size_t>(config_.chunk_size)) { std::this_thread::yield(); continue; }
        total_samples_processed += local_chunk.size();

        // שחרור דחוי של חוצצי הנגינה אחרי Clear (נספר בצ'אנקים == זמן אמת)
        if (buffer_release_countdown > 0 && --buffer_release_countdown == 0) {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            playback_buffers_[0].clear();
            playback_buffers_[1].clear();
            playback_read_idx_.store(0, std::memory_order_relaxed);
        }

        // חישוב כפול: עוצמה פיזית גולמית (להקלטה ולתצוגה), ועוצמה מסוננת (להחלטה אלגוריתמית)
        float raw_volume = calculate_rms(local_chunk);
        float trigger_volume = calculate_trigger_rms(local_chunk);
        current_yin = -1.0f;   // יחושב רק בצ'אנקים של רצף/החלטה

        bool telemetry_active = (telemetry_sink_.load(std::memory_order_relaxed) != nullptr);

        current_rms_.store(raw_volume, std::memory_order_relaxed);
        current_noise_std_dev_.store(noise_tracker_.get_std_dev(), std::memory_order_relaxed);

        LooperState state = current_state_.load(std::memory_order_relaxed);

        // חיווי הסגירה חי רק בזמן הקלטה אוטומטית. בכל מצב אחר הוא חייב להתאפס,
        // אחרת טבעת הספירה-לאחור נשארת תקועה על המסך אחרי שהלופ כבר פורסם.
        if (state != LooperState::RECORDING)
            closure_progress_.store(0.0f, std::memory_order_relaxed);

        switch (state) {
            case LooperState::CALIBRATING: {
                // המערכת לומדת את רעש הרקע בשני המרחבים: המסונן (להתקף) והגולמי (לתהודה ושקט)
                noise_tracker_.observe_background_noise(trigger_volume);
                raw_noise_tracker_.observe_background_noise(raw_volume);
                if (noise_tracker_.is_ready() && raw_noise_tracker_.is_ready()) {
                    current_state_.store(LooperState::IDLE, std::memory_order_release);
                }
                break;
            }

            case LooperState::IDLE: {
                // 1. שמירת ההיסטוריה הפיזית בזיכרון מעגלי (מבטיח שלא נאבד את ההתקף בזמן שאנחנו ממתינים להוכחת כוונה)
                for (float s : local_chunk) {
                    preroll_buffer_[preroll_write_idx_] = s;
                    preroll_write_idx_ = (preroll_write_idx_ + 1) % max_preroll_samples;
                }

                int mode = detection_mode_.load(std::memory_order_relaxed);

                // --- ספירה-לתוך (SYNC בלבד) ---
                // הקליק כבר נשמע ב-IDLE במצב SYNC, ולכן הספירה היא רק חלון זמן
                // שבסופו ההקלטה נפתחת מעצמה. יישור מדויק-לדגימה מיותר: PROCESSING
                // גוזר את הראש ל-Onset האמיתי וממילא מצמיד לתיבות שלמות.
                if (request_count_in_cancel_.exchange(false, std::memory_order_relaxed)) {
                    count_in_remaining = 0;
                    count_in_beats_left_.store(0, std::memory_order_relaxed);
                }
                if (request_count_in_.exchange(false, std::memory_order_relaxed)) {
                    if (mode == 2) {
                        float bpm_ci = target_bpm_.load(std::memory_order_relaxed);
                        if (bpm_ci < 20.0f) bpm_ci = 120.0f;
                        float beat_ci = (60.0f / bpm_ci) * config_.sample_rate;
                        count_in_remaining = static_cast<size_t>(
                            count_in_bars_.load(std::memory_order_relaxed) * 4.0f * beat_ci);
                    }
                }
                bool count_in_fired = false;
                // Leaving SYNC abandons the count: the click that gives it meaning
                // only sounds in that mode, so it would become a silent wait.
                if (count_in_remaining > 0 && mode != 2) {
                    count_in_remaining = 0;
                    count_in_beats_left_.store(0, std::memory_order_relaxed);
                }
                if (count_in_remaining > 0) {
                    size_t step = local_chunk.size();
                    count_in_remaining = (count_in_remaining > step) ? count_in_remaining - step : 0;
                    float bpm_ci = target_bpm_.load(std::memory_order_relaxed);
                    if (bpm_ci < 20.0f) bpm_ci = 120.0f;
                    float beat_ci = (60.0f / bpm_ci) * config_.sample_rate;
                    count_in_beats_left_.store(
                        static_cast<int>(std::ceil(count_in_remaining / std::max(1.0f, beat_ci))),
                        std::memory_order_relaxed);
                    if (count_in_remaining == 0) count_in_fired = true;
                }

                // (כיול-מחדש אוטומטי מבוסס-חלון נבנה כאן ונמחק אחרי מדידה: הלמידה
                //  ב-IDLE כבר רציפה — ראה הבלוק המשוער-רקע בסוף ה-case הזה — והיא
                //  מטפלת בכיוון ה"חדר נעשה שקט" תוך ~2 שניות. הכיוון ההפוך אינו
                //  ניתן לתיקון מ-IDLE כי רעש חדש פותח רצף ומוציא אותנו מ-IDLE
                //  מיד. הריפוי לכיוון הזה יושב על הווטו ב-PROCESSING.)

                // --- אלגוריתם היסטרזיס א-סימטרי מחמיר ---

                if (current_onset_streak_ == 0) {
                    // שלב א': מבחן התקף חריף. טרנזיינט מוגדר על-ידי *נגזרת* האנרגיה (Flux),
                    // לא על-ידי רמתה: רמה בלבד נורית גם על היס/שפשוף מתמשך בתדר גבוה.
                    // לכן דורשים גם רמה מעל הסטטיסטיקה וגם עלייה חדה מול החלון הקודם.
                    float onset_threshold = std::max(noise_tracker_.get_onset_threshold(),
                                                     min_onset_rms_.load(std::memory_order_relaxed));
                    bool level_ok = trigger_volume > onset_threshold;
                    bool rising = trigger_volume > prev_trigger_volume * onset_rise_ratio_.load(std::memory_order_relaxed);
                    if (level_ok && rising) {
                        current_onset_streak_ = 1; // החלון הראשון נתפס
                        transient_hit_flag_.store(true, std::memory_order_relaxed); // טלמטריה ל-UI
                    }
                } else {
                    // שלב ב': מבחן תוחלת החיים (Sustain). מחפש מסת תהודה לאורך זמן.
                    // התהודה הגולמית נמדדת מול סטטיסטיקת הרעש *הגולמית* — השוואה מול
                    // הסטטיסטיקה המסוננת היא שגיאת יחידות (הסקאלות אינן ברות-השוואה).
                    // הסף יחסי-לרעש (mean×מכפלה) ולא מוחלט: רצפה מוחלטת קושרת את
                    // הזיהוי לרגישות מיקרופון אחת. המבחין בין פריטה לטרנזיינט סביבתי
                    // הוא משך (persistence) + מחזוריות (YIN) — לא רמה.
                    float raw_mean = raw_noise_tracker_.get_mean();
                    float sustain_threshold = std::max({raw_noise_tracker_.get_silence_threshold(),
                                                        raw_mean * sustain_rel_mult_.load(std::memory_order_relaxed),
                                                        raw_sustain_floor_.load(std::memory_order_relaxed)});

                    bool is_sustaining = (raw_volume > sustain_threshold);

                    if (is_sustaining) {
                        current_onset_streak_++; // האנרגיה ממשיכה להתקיים בעולם
                    } else {
                        current_onset_streak_ = 0; // האנרגיה מתה מהר מדי. זו הייתה נגיעה אקראית. מוחקים.
                    }
                }

                // מדידת YIN שוטפת לאורך הרצף — אופליין בלבד (איסוף התפלגויות לכוונון)
                if (telemetry_active && current_onset_streak_ > 0) {
                    current_yin = compute_yin_from_preroll();
                }

                // 2. קבלת החלטה
                // המכונה תאשר הקלטה רק אם הרצף שרד את persistence החלונות במלואם.
                // ספירה פעילה מקפיאה את הטריגר האקוסטי: הנגן אמור להיכנס *על*
                // הפעימה, וצליל מוקדם היה מבטל את הספירה שהוא עצמו ביקש.
                bool auto_triggered = (mode != 1) && (count_in_remaining == 0) &&
                    (current_onset_streak_ >= onset_persistence_target_.load(std::memory_order_relaxed));

                // שער המחזוריות: רצף שעבר את מבחני הרמה והמשך אך אינו מחזורי — רעש
                // רחב-סרט מתמשך (קומפרסור/חבטה מהדהדת), לא מיתר. נמדד: הפרדת-רמה של
                // המחלקה הזו מפריטה רכה בלתי-אפשרית בטווח ±12dB; מחזוריות חסינת-Gain.
                float yin_gate = yin_gate_threshold_.load(std::memory_order_relaxed);
                if (auto_triggered && yin_gate > 0.0f) {
                    if (current_yin < 0.0f) current_yin = compute_yin_from_preroll();
                    if (current_yin < yin_gate) {
                        auto_triggered = false;
                        current_onset_streak_ = 0;   // א-מחזורי — איפוס מלא, פריטה אמיתית תבנה רצף חדש
                    }
                }

                bool manual_triggered = (mode == 1) && request_record_start_.exchange(false, std::memory_order_relaxed);

                if (auto_triggered || manual_triggered || count_in_fired) {
                    current_onset_streak_ = 0;
                    count_in_beats_left_.store(0, std::memory_order_relaxed);
                        recorded_audio.clear();

                    // שאיבת הזמן האבוד: אנו מושכים את האודיו מה-Preroll, כך שכל 4 החלונות שעליהם התלבטנו
                    // (כולל הטרנזיינט הראשוני) מוכנסים במלואם להקלטה הסופית ללא קטיעות.
                    //
                    // ⚠ למעט אחרי ספירה-לתוך: אין שם התקף להציל (אנחנו פותחים על
                    // הפעימה, לא בתגובה לצליל), וה-Preroll מכיל דווקא את *הקליק
                    // האחרון* שדלף מהרמקול למיקרופון — טרנזיינט חד ש-find_true_onset
                    // היה מזהה כתחילת הלופ, ומצמיד את הלולאה לקליק במקום לגיטרה.
                    if (!count_in_fired) {
                        for (size_t i = 0; i < max_preroll_samples; ++i) {
                            size_t read_idx = (preroll_write_idx_ + i) % max_preroll_samples;
                            recorded_audio.push_back(preroll_buffer_[read_idx]);
                        }
                    }

                    peak_envelope = raw_volume;
                    session_peak_raw = raw_volume;    // רצפת הפעילות נבנית מהסשן הזה בלבד
                    silence_samples_count = 0;
                    inactivity_count = 0;
                    trailing_non_musical_samples = 0;
                    samples_since_articulation = 0;   // ההתקף הפותח הוא הארטיקולציה הראשונה
                    current_state_.store(LooperState::RECORDING, std::memory_order_release);
                } else {
                    // 3. עדכון סביבה דינמי — אך *רק* כשהצ'אנק באמת נראה כמו רקע.
                    // באג קריטי שתוקן: התקף אמיתי שלא הצית מיד את שלב א' (למשל פריטה
                    // רכה עם קליק חלש) נלמד כ"רעש", הסף ברח כלפי מעלה מהר מהאות עצמו,
                    // וההתקף הפך בלתי-ניתן-לזיהוי לצמיתות. הקפאת הלמידה בכל אנרגיה מעל
                    // חצי סף התהודה האפקטיבי (יחסי-לרעש, כמו הסף עצמו) מונעת את ההרעלה.
                    float raw_mean_bg = raw_noise_tracker_.get_mean();
                    float effective_sustain = std::max({raw_noise_tracker_.get_silence_threshold(),
                                                        raw_mean_bg * sustain_rel_mult_.load(std::memory_order_relaxed),
                                                        raw_sustain_floor_.load(std::memory_order_relaxed)});
                    bool looks_like_background = (raw_volume < effective_sustain * 0.5f);
                    if (current_onset_streak_ == 0 && looks_like_background) {
                        noise_tracker_.observe_background_noise(trigger_volume);
                        raw_noise_tracker_.observe_background_noise(raw_volume);
                    }
                }
                break;
            }

            case LooperState::RECORDING: {
                recorded_audio.insert(recorded_audio.end(), local_chunk.begin(), local_chunk.end());

                // מעטפת פיק דועכת: התקפה מיידית, שחרור אקספוננציאלי.
                // המקסימום הכל-זמני קיבע את סף השקט לרגע הכי רועש בהקלטה,
                // וגרם לעצירה כוזבת בביצוע שנפתח חזק ונרגע לדינמיקה שקטה.
                peak_envelope = std::max(raw_volume, peak_envelope * env_release_coef);

                int mode = detection_mode_.load(std::memory_order_relaxed);

                if (mode == 1) {
                    // מצב Tap & Trim — הנגן קבע את הסוף בעצמו; אין זנב לא-מוזיקלי
                    closure_progress_.store(0.0f, std::memory_order_relaxed);   // אין ספירה-לאחור ידנית
                    if (request_record_stop_.exchange(false, std::memory_order_relaxed)) {
                        trailing_non_musical_samples = 0;
                        current_state_.store(LooperState::PROCESSING, std::memory_order_release);
                    }
                } else {
                    // מצב Auto Silence — שני מסלולי סגירה, סמנטיקה אחידה:
                    // "פעילות מוזיקלית" = אנרגיה גולמית מעל רצפה יחסית-לסשן.
                    //
                    // לקח שנמדד ביוקר: זיהוי "התקף-חוזר" (בכל וריאציה — יחס לצ'אנק
                    // קודם, יחס למעטפת דועכת) אינו ניתן להפרדה: סטראם בדקרשנדו לעולם
                    // אינו עולה על מעטפת הסטראם הקודם החזק (יחס 0.98), בעוד אדוות
                    // ריברב מגיעה ל-1.1-1.3 — ההפרדה הפוכה. לעומת זאת, ה-raw במהלך
                    // נגינה (כולל בין תווים — המיטה המצלצלת) נשאר ≥0.11, וזנב-סיום
                    // דועך מתחת לכל רמת נגינה תוך ~שנייה. לכן "פעילות" נמדדת ב-raw
                    // מול שבריר מהשיא של ההקלטה עצמה — חסין לרווחי-נגינה, לאקורדים
                    // מוחזקים, לדקרשנדו, ולעוצמת כניסה משתנה בין מכשירים.
                    session_peak_raw = std::max(session_peak_raw, raw_volume);
                    float sil_floor = std::max({raw_noise_tracker_.get_silence_threshold(),
                                                raw_noise_tracker_.get_mean() * silence_rel_mult_.load(std::memory_order_relaxed),
                                                silence_abs_floor_.load(std::memory_order_relaxed)});
                    // תקרה יחסית-לשיא: הרצפה היחסית-לרעש מכוילת לחדר-הייחוס, ובחדר
                    // רועש (mean גבוה) היא מטפסת אל *תוך* טווח הנגינה — ואז פסאז'
                    // פיאניסימו נספר כשקט וההקלטה נסגרת באמצע ביטוי. חוסמים אותה
                    // מתחת לרצפת-הפעילות (0.15 מהשיא) כדי שסדר שני המסלולים יישמר:
                    // "שקט" חייב להיות שקט יותר מ"חוסר-פעילות", לא להתלכד איתו.
                    //
                    // ⚠ תנאי-השער אינו קוסמטי — נמדד: בלעדיו הקורפוס השלילי נשבר
                    // (ac_silence/silence2 מפרסמים לופ בכל נקודת מטריצה). הסיבה:
                    // בטייק שהוצת מרעש, session_peak *הוא* הרעש, התקרה צונחת מתחת
                    // לרמת הרעש, מסלול-השקט לעולם לא נסגר, והסגירה האיטית משאירה
                    // מספיק "תוכן" כדי לעקוף את וטו התוכן-המזערי. לכן מרפים את
                    // הרצפה רק כשהטייק *מוכח* כנגינה: יחס שיא-לרעש גבוה. בקורפוס
                    // גיטרה ‎≫100, בטייק רעש ‎<10 — הפרדה בסדר גודל, לא כיול עדין.
                    float noise_mean_now = raw_noise_tracker_.get_mean();
                    bool proven_instrument = noise_mean_now > 0.0f &&
                        session_peak_raw > noise_mean_now *
                            silence_peak_cap_min_snr_.load(std::memory_order_relaxed);
                    if (proven_instrument) {
                        float peak_cap = session_peak_raw *
                                         silence_peak_cap_ratio_.load(std::memory_order_relaxed);
                        sil_floor = std::max(silence_abs_floor_.load(std::memory_order_relaxed),
                                             std::min(sil_floor, peak_cap));
                    }
                    float activity_floor = std::max(sil_floor,
                        session_peak_raw * activity_ratio_.load(std::memory_order_relaxed));
                    size_t silence_target = static_cast<size_t>(
                        silence_hold_seconds_.load(std::memory_order_relaxed) * config_.sample_rate);
                    size_t inactivity_target = static_cast<size_t>(
                        activity_hold_seconds_.load(std::memory_order_relaxed) * config_.sample_rate);

                    // מסלול א' (מהיר): שקט מוחלט רצוף — לעצירות mute/סטקטו
                    if (raw_volume < sil_floor) silence_samples_count += local_chunk.size();
                    else silence_samples_count = 0;

                    // מסלול ב' (איטי): היעדר פעילות — לזנב הד/ריברב שנשאר מעל סף השקט
                    if (raw_volume >= activity_floor) inactivity_count = 0;
                    else inactivity_count += local_chunk.size();

                    // חיווי סגירה ל-UI: המרבי מבין שני המסלולים, מחושב מאותם מונים
                    // *שמחליטים* בפועל (לא שחזור ב-Kotlin — שחזור היה משקר בדיוק
                    // ברגע שהמשתמש צריך לסמוך עליו). ‎1.0 = נסגר בצ'אנק הזה.
                    if (silence_target > 0 || inactivity_target > 0) {
                        float p_sil = silence_target ?
                            static_cast<float>(silence_samples_count) / static_cast<float>(silence_target) : 0.0f;
                        float p_act = inactivity_target ?
                            static_cast<float>(inactivity_count) / static_cast<float>(inactivity_target) : 0.0f;
                        closure_progress_.store(std::clamp(std::max(p_sil, p_act), 0.0f, 1.0f),
                                                std::memory_order_relaxed);
                    }

                    // סגירה על המוקדם מבין השניים. הזנב הלא-מוזיקלי לקוונטיזציה הוא
                    // הזמן מאז שהפעילות פסקה — תחילת הדעיכה ≈ הסיום המוזיקלי, ולכן
                    // אין צורך בתיקוני "+פעימה" הוריסטיים.
                    if (silence_samples_count >= silence_target ||
                        inactivity_count >= inactivity_target) {
                        trailing_non_musical_samples = std::max(silence_samples_count, inactivity_count);
                        current_state_.store(LooperState::PROCESSING, std::memory_order_release);
                    }

                    // נטישת-דרון: רעש מתמשך שלא נגמר (שואב/מקלחת/גשם) לא סוגר באף
                    // מסלול — אין שקט ואין היעדר-פעילות — אבל גם אין בו *ארטיקולציות*
                    // (התקפים חדשים במרחב המסונן). בקורפוס: פער-ארטיקולציות מרבי
                    // בהקלטה לגיטימית 5.57s ⇒ 10s = שוליים ×1.8. הנגן שקט 10s = ממילא
                    // אין מוזיקה לשמור. ההקלטה נזרקת, והכיול מאופס במלואו כדי שהרעש
                    // החדש יילמד כרצפה (בלי האיפוס — לולאת הקלטות-רפאים אינסופית).
                    bool articulated = trigger_volume >
                                           std::max(noise_tracker_.get_onset_threshold(),
                                                    min_onset_rms_.load(std::memory_order_relaxed)) &&
                                       trigger_volume > prev_trigger_volume *
                                           onset_rise_ratio_.load(std::memory_order_relaxed);
                    if (articulated) samples_since_articulation = 0;
                    else samples_since_articulation += local_chunk.size();
                    if (samples_since_articulation >
                        static_cast<size_t>(drone_abort_seconds_.load(std::memory_order_relaxed) *
                                            config_.sample_rate)) {
                        std::cout << "[DSP] Drone abort: no articulation for "
                                  << drone_abort_seconds_.load(std::memory_order_relaxed)
                                  << "s — discarding take, relearning the room." << std::endl;
                        recorded_audio.clear();
                        noise_tracker_.reconfigure(config_.sample_rate, config_.chunk_size, 1.0f);
                        raw_noise_tracker_.reconfigure(config_.sample_rate, config_.chunk_size, 1.0f);
                        current_state_.store(LooperState::CALIBRATING, std::memory_order_release);
                        break;
                    }
                }

                // רשת ביטחון: הקלטה שלא נסגרת לעולם לא תרוקן את זיכרון המכשיר
                if (recorded_audio.size() >= max_record_samples) {
                    std::cout << "[DSP] Max recording length reached — forcing PROCESSING." << std::endl;
                    trailing_non_musical_samples = 0;
                    current_state_.store(LooperState::PROCESSING, std::memory_order_release);
                }
                break;
            }

            case LooperState::PROCESSING: {
                // סף במרחב הגולמי — האודיו המוקלט הוא גולמי, לא מסונן
                size_t onset_index = find_true_onset(recorded_audio, max_preroll_samples,
                    std::max(raw_noise_tracker_.get_onset_threshold(), min_onset_rms_.load(std::memory_order_relaxed)));

                if (onset_index > 0 && onset_index < recorded_audio.size()) {
                    recorded_audio.erase(recorded_audio.begin(), recorded_audio.begin() + onset_index);
                }

                if (recorded_audio.size() <= trailing_non_musical_samples) {
                    current_state_.store(LooperState::IDLE, std::memory_order_release);
                    break;
                }

                // תוכן מוזיקלי = הכל פחות הזנב הלא-מוזיקלי. הזנב נמדד מהרגע שבו
                // הפעילות פסקה (raw צנח מתחת לרצפה היחסית) — שהוא בקירוב טוב הסיום
                // המוזיקלי עצמו, ולכן אין צורך בתיקונים הוריסטיים נוספים.
                float actual_playing_samples = static_cast<float>(recorded_audio.size() - trailing_non_musical_samples);
                int assembly_mode = detection_mode_.load(std::memory_order_relaxed);

                // וטו תוכן-מזערי (מצבי Auto בלבד): טרנזיינט סביבתי שהצית הקלטה
                // (חבטה/התנעת קומפרסור) עובר רמה+משך+מחזוריות אך מתפוגג בלי להשאיר
                // תוכן מוזיקלי. נמדד: FP = 0.1-0.2s תוכן; הפריטה הבודדת הרפה ביותר
                // בקורפוס = 0.94s (שוליים ×4.7). משך לא סוקל עם Gain ⇒ חסין-רגישות.
                // TAP (mode 1) ריבוני — המשתמש קבע את הגבולות בעצמו.
                if (assembly_mode != 1 &&
                    actual_playing_samples <
                        min_musical_seconds_.load(std::memory_order_relaxed) * config_.sample_rate) {
                    std::cout << "[DSP] Take vetoed: only "
                              << (actual_playing_samples / config_.sample_rate)
                              << "s of musical content — non-musical transient, back to IDLE." << std::endl;
                    recorded_audio.clear();
                    // ריפוי-עצמי לכיוון "החדר נעשה רועש". הלמידה ב-IDLE מוקפאת
                    // בכל אנרגיה מעל חצי סף התהודה (הגנת בריחת-הרצפה), ולכן רעש
                    // חדש *מעל* הסף לעולם לא נלמד — הוא רק מצית טייקים שהווטו
                    // זורק, שוב ושוב. שני ווטואים רצופים הם ההוכחה שהמודל של
                    // החדר שגוי (ולא סתם חבטה חד-פעמית), ואז לומדים אותו מחדש —
                    // בדיוק ההיגיון של נטישת-הדרון, על מחלקת-כשל אחרת.
                    if (++consecutive_vetoes >= 2) {
                        consecutive_vetoes = 0;
                        noise_tracker_.reconfigure(config_.sample_rate, config_.chunk_size, 1.0f);
                        raw_noise_tracker_.reconfigure(config_.sample_rate, config_.chunk_size, 1.0f);
                        current_onset_streak_ = 0;
                        std::cout << "[DSP] Two vetoed takes in a row — re-learning the room."
                                  << std::endl;
                        current_state_.store(LooperState::CALIBRATING, std::memory_order_release);
                        break;
                    }
                    current_state_.store(LooperState::IDLE, std::memory_order_release);
                    break;
                }
                consecutive_vetoes = 0;   // טייק אמיתי ⇒ מודל החדר תקף
                size_t ideal_length;

                if (assembly_mode == 2) {
                    // --- מצב SYNC (שעון): הקצב נמסר על-ידי הנגן, ולכן ההצמדה לתיבות
                    // *סמכותית* (בניגוד ל-AUTO שבו הקצב מאומד ולא-ודאי → ההצמדה נסוגה).
                    // ההתקף מגדיר את פעימה 1; ה-Auto-Silence מגדיר סוף גס; אנו מצמידים
                    // למספר שלם של תיבות (4/4) בקצב שנקבע ⇒ אורך מדויק-לדגימה שמסתנכרן.
                    float bpm = target_bpm_.load(std::memory_order_relaxed);
                    if (bpm < 20.0f) bpm = 120.0f;
                    float beat_length = (60.0f / bpm) * config_.sample_rate;
                    float exact_beats = actual_playing_samples / beat_length;
                    const float BEATS_PER_BAR = 4.0f;
                    float bars = std::round(exact_beats / BEATS_PER_BAR);
                    if (bars < 1.0f) bars = 1.0f;
                    float target_musical_beats = bars * BEATS_PER_BAR;
                    ideal_length = static_cast<size_t>(target_musical_beats * beat_length);
                    estimated_bpm_.store(bpm, std::memory_order_relaxed);
                    loop_beats_.store(target_musical_beats, std::memory_order_relaxed);
                    std::cout << "[DSP] Clock loop: " << bars << " bars @ " << bpm << " BPM (played "
                              << exact_beats << " beats) -> "
                              << (static_cast<float>(ideal_length) / config_.sample_rate) << "s" << std::endl;
                } else {
                    // --- מצב AUTO: קצב מאומד + הצמדת-פרייז עם רצועת חצי-פעימה ---
                    size_t last_onset_samples = 0;
                    float beat_length = extract_beat_length_from_onsets(
                        recorded_audio, static_cast<size_t>(actual_playing_samples), &last_onset_samples);
                    float exact_beats = actual_playing_samples / beat_length;
                    float last_onset_beats = static_cast<float>(last_onset_samples) / beat_length;
                    float target_musical_beats = quantize_to_musical_phrase(exact_beats);
                    loop_beats_.store(target_musical_beats, std::memory_order_relaxed);   // רשת ה-UI
                    float quantized_length = target_musical_beats * beat_length;
                    float deviation = std::abs(quantized_length - actual_playing_samples);
                    bool grid_accepted = (deviation <= 0.6f * beat_length);
                    ideal_length = static_cast<size_t>(
                        grid_accepted ? quantized_length : actual_playing_samples);
                    std::cout << "[DSP] Loop assembly: beat=" << (beat_length / config_.sample_rate)
                              << "s (" << estimated_bpm_.load(std::memory_order_relaxed) << " BPM)"
                              << " playing=" << (actual_playing_samples / config_.sample_rate)
                              << "s exact_beats=" << exact_beats
                              << " last_onset_beats=" << last_onset_beats
                              << " -> " << target_musical_beats
                              << " grid=" << (grid_accepted ? "ACCEPTED" : "REJECTED")
                              << " final=" << (static_cast<float>(ideal_length) / config_.sample_rate) << "s" << std::endl;
                }

                std::vector<float> final_loop;
                final_loop.reserve(ideal_length);
                for (size_t i = 0; i < ideal_length && i < recorded_audio.size(); ++i) {
                    final_loop.push_back(recorded_audio[i]);
                }

                bool tail_folded = false;
                if (final_loop.size() < ideal_length) {
                    final_loop.resize(ideal_length, 0.0f);
                } else if (recorded_audio.size() > ideal_length) {
                    // קיפול הזנב לראש הלופ. ה-Fade חל על *הזנב בלבד* — הכפלת הסכום
                    // כולו (הבאג הקודם) הנחיתה את פתיחת הלופ עצמה מ-100% לאפס לאורך
                    // עד חצי לופ, ויצרה את "צניחת העוצמה וחזרתה" שנשמעה בכל סיבוב.
                    // הקיפול גם מספק את רציפות התפר: tail[0] הוא ההמשך הפיזי המדויק
                    // של הדגימה האחרונה בלופ, ותחילת הלופ (אחרי חיתוך ל-Onset) היא ~0.
                    size_t tail_length = recorded_audio.size() - ideal_length;
                    tail_length = std::min(tail_length, ideal_length);
                    for (size_t i = 0; i < tail_length; ++i) {
                        float tail_fade = 1.0f - (static_cast<float>(i) / static_cast<float>(tail_length));
                        float mixed = final_loop[i] + recorded_audio[ideal_length + i] * tail_fade;
                        final_loop[i] = std::clamp(mixed, -1.0f, 1.0f);
                    }
                    tail_folded = true;
                }

                // קיפול-קצה מופעל רק כשאין זנב מקופל (TAP / ריפוד): עם זנב — התפר
                // רציף מעצם הבנייה. שומר-הרציפות שבתוך apply_seam_fold מדלג ממילא
                // כשהקצה כבר חלק (למשל ריפוד אפסים אל ראש שקט).
                if (!tail_folded) {
                    final_loop = apply_seam_fold(final_loop);
                }

                if (!final_loop.empty()) {
                    last_published_loop_samples = final_loop.size();
                    // הבסיס המוגן (מסלול 0) — פותח היסטוריית שכבות חדשה. loop_length_
                    // ננעל כאן; כל אוברדאב עתידי חייב אורך זהה. המיקס (=הבסיס, שכבה
                    // יחידה) מתפרסם לחוצץ ה-RCU. הקורא רדום ב-PROCESSING → מוענק מיידית.
                    loop_length_ = final_loop.size();
                    layers_.clear();
                    layers_.push_back(make_layer(std::move(final_loop)));
                    sync_layer_mirror();
                    cancel_pending_pure();   // בסיס חדש — כל מיקס ממתין ישן
                    int slot = acquire_writable_slot();
                    if (slot < 0) slot = 1 - active_playback_idx_.load(std::memory_order_relaxed);
                    {
                        std::lock_guard<std::mutex> lock(buffer_mutex_);
                        playback_buffers_[slot] = layers_[0].samples;
                    }
                    playback_read_idx_.store(0, std::memory_order_relaxed);
                    active_playback_idx_.store(slot, std::memory_order_release);
                    reset_transport();   // בסיס חדש נולד מנגן, תמיד
                    current_state_.store(LooperState::LOOPING, std::memory_order_release);
                } else {
                    current_state_.store(LooperState::IDLE, std::memory_order_release);
                }
                break;
            }

            case LooperState::LOOPING: {
                // --- שלב 2 של פרסום חלק: ברגע שהקורא חלף על הטלאי — פרסום המיקס הטהור ---
                // (אחרת אזור-הטלאי, שמכיל תוכן ישן, היה מתנגן שוב בכל סיבוב.)
                if (!pure_mix_pending.empty()) {
                    if (pure_mix_pending.size() != loop_length_) {
                        cancel_pending_pure();               // ישן — האורך השתנה מתחתיו
                    } else if (pure_publish_countdown > 0) {
                        --pure_publish_countdown;
                    } else {
                        mix_temp_ = pure_mix_pending;        // העתקה — נשמר עד הצלחה
                        if (publish_mix_temp(/*reset_read=*/false)) cancel_pending_pure();
                        // slot תפוס: ננסה שוב בצ'אנק הבא
                    }
                }

                // --- מחיקת שכבת-אוברדאב (רב-מסלול, חינמי) ---
                // אינדקס >=1 בלבד: מסלול 0 הוא הבסיס המוגן. אורך הלופ אינו משתנה
                // ⇒ הקורא ממשיך מאותו מיקום בלי קפיצה; הפרסום החלק מוחק את מדרגת-התוכן.
                int del = request_delete_layer_.load(std::memory_order_relaxed);
                if (del >= 1 && del < static_cast<int>(layers_.size())) {
                    render_full_mix_excluding(del, mix_temp_);
                    if (publish_mix_smoothed()) {
                        request_delete_layer_.store(-1, std::memory_order_relaxed);
                        // שמירה לביטול *לפני* המחיקה — השכבה השלמה, לא רק האודיו,
                        // כדי ששחזור יחזיר גם fx/gain/reverb/clean כפי שהיו.
                        undo_layer_ = layers_[del];
                        undo_layer_index_ = del;
                        undo_available_.store(true, std::memory_order_relaxed);
                        layers_.erase(layers_.begin() + del);
                        sync_layer_mirror();
                    }
                    // slot תפוס: נשאיר את הבקשה, ננסה שוב בצ'אנק הבא (mix_temp_ מרונדר מחדש)
                } else if (del >= 0) {
                    request_delete_layer_.store(-1, std::memory_order_relaxed);  // 0=בסיס / מחוץ-לטווח → התעלם
                }

                // --- ביטול המחיקה האחרונה ---
                // אותו חוזה כמו המחיקה: מרנדרים את היעד, ורק אם הפרסום הצליח
                // נוגעים ב-layers_. הפרסום החלק מוחק את מדרגת-התוכן, כך שהחזרת
                // שכבה באמצע צליל אינה נשמעת כקליק.
                if (request_undo_delete_.load(std::memory_order_relaxed)) {
                    bool ok = undo_layer_index_ >= 1 && loop_length_ > 0 &&
                              undo_layer_.samples.size() == loop_length_ &&
                              static_cast<int>(layers_.size()) < kMaxLayers;
                    if (ok) {
                        render_full_mix_including(undo_layer_, mix_temp_);
                        if (publish_mix_smoothed()) {
                            int at = std::min<int>(undo_layer_index_, static_cast<int>(layers_.size()));
                            layers_.insert(layers_.begin() + at, std::move(undo_layer_));
                            request_undo_delete_.store(false, std::memory_order_relaxed);
                            drop_undo();
                            sync_layer_mirror();
                            std::cout << "[DSP] Layer restored at " << at << "." << std::endl;
                        }
                        // slot תפוס: נשארים בבקשה, ננסה שוב בצ'אנק הבא
                    } else {
                        request_undo_delete_.store(false, std::memory_order_relaxed);
                    }
                }

                // --- אפקטים פר-שכבה (פייז 3, Pro): fx / gain / reverb ---
                // כל ערוץ Last-Write-Wins ו-*אידמפוטנטי* (SET, לא toggle): לכן מותר
                // לשנות את layers_ ישירות (Worker-owned; הקורא לעולם לא נוגע ב-layers_,
                // רק ב-playback_buffers_). אם הפרסום נדחה (Slot תפוס) הבקשה נשארת,
                // ובצ'אנק הבא אותה פעולה חוזרת בדיוק — בלי צורך בעותק שכבות.
                int lfx_idx = req_layer_fx_idx_.load(std::memory_order_acquire);
                if (lfx_idx >= 0 && lfx_idx < static_cast<int>(layers_.size())) {
                    int old_fx = layers_[lfx_idx].fx;
                    layers_[lfx_idx].fx = req_layer_fx_kind_.load(std::memory_order_relaxed);
                    render_layer(layers_[lfx_idx]);            // dry→samples לפי ה-fx החדש
                    if (layers_[lfx_idx].samples.size() == loop_length_) {
                        render_full_mix_excluding(-1, mix_temp_);
                        if (publish_mix_smoothed()) {
                            req_layer_fx_idx_.store(-1, std::memory_order_relaxed);
                            sync_layer_mirror();
                        }
                        // slot תפוס: הבקשה נשארת; ננסה שוב (אידמפוטנטי)
                    } else {
                        layers_[lfx_idx].fx = old_fx; render_layer(layers_[lfx_idx]);  // שחזור
                        req_layer_fx_idx_.store(-1, std::memory_order_relaxed);
                    }
                } else if (lfx_idx >= 0) {
                    req_layer_fx_idx_.store(-1, std::memory_order_relaxed);
                }

                // reverb: נצרב בבייק Freeverb יקר → ה-UI שולח *בשחרור הסליידר* בלבד.
                int lrv_idx = req_layer_reverb_idx_.load(std::memory_order_acquire);
                if (lrv_idx >= 0 && lrv_idx < static_cast<int>(layers_.size())) {
                    float old_rv = layers_[lrv_idx].reverb;
                    layers_[lrv_idx].reverb = std::clamp(req_layer_reverb_val_.load(std::memory_order_relaxed), 0.0f, 1.0f);
                    render_layer(layers_[lrv_idx]);
                    if (layers_[lrv_idx].samples.size() == loop_length_) {
                        render_full_mix_excluding(-1, mix_temp_);
                        if (publish_mix_smoothed()) {
                            req_layer_reverb_idx_.store(-1, std::memory_order_relaxed);
                            sync_layer_mirror();
                        }
                    } else {
                        layers_[lrv_idx].reverb = old_rv; render_layer(layers_[lrv_idx]);
                        req_layer_reverb_idx_.store(-1, std::memory_order_relaxed);
                    }
                } else if (lrv_idx >= 0) {
                    req_layer_reverb_idx_.store(-1, std::memory_order_relaxed);
                }

                // --- מסכת האזנה (Solo/Mute) ---
                // אימוץ המסכה החדשה מותנה בהצלחת הפרסום: אחרת המרנדרים היו
                // ממשיכים לעבוד לפי מסכה שמעולם לא נשמעה.
                uint32_t want_mask = layer_mute_mask_.load(std::memory_order_acquire);
                if (want_mask != active_mask) {
                    uint32_t prev_mask = active_mask;
                    active_mask = want_mask;
                    render_full_mix_excluding(-1, mix_temp_);
                    if (!publish_mix_smoothed()) active_mask = prev_mask;   // ננסה שוב
                }

                // gain: חי וזול (מוחל במיקס בלבד — אין רינדור-שכבה). מפורסם חלק —
                // גרירה שולחת מדרגות בדידות; המיזוג הופך אותן לרציפות (אנטי-Zipper).
                int lg_idx = req_layer_gain_idx_.load(std::memory_order_acquire);
                if (lg_idx >= 0 && lg_idx < static_cast<int>(layers_.size())) {
                    layers_[lg_idx].gain = std::clamp(req_layer_gain_val_.load(std::memory_order_relaxed), 0.0f, 2.0f);
                    render_full_mix_excluding(-1, mix_temp_);
                    if (publish_mix_smoothed()) {
                        req_layer_gain_idx_.store(-1, std::memory_order_relaxed);
                        sync_layer_mirror();
                    }
                } else if (lg_idx >= 0) {
                    req_layer_gain_idx_.store(-1, std::memory_order_relaxed);
                }

                // CLEAN לכל השכבות (Pro): הדלקה ראשונה מחשבת את השער לכל שכבה
                // (~עשרות ms לשכבה, על ה-Worker — ההשמעה ממשיכה מהחוצץ המפורסם);
                // חזרות/כיבוי זולים (מטמון dry_denoised). אידמפוטנטי ⇒ ניסיון-חוזר
                // בטוח כשה-Slot תפוס, כמו שאר ערוצי הפקודה.
                int dn = req_denoise_all_.load(std::memory_order_acquire);
                if (dn >= 0 && !layers_.empty()) {
                    bool on = (dn == 1);
                    for (Layer& L : layers_) { L.denoise = on; render_layer(L); }
                    render_full_mix_excluding(-1, mix_temp_);
                    if (publish_mix_smoothed()) {
                        req_denoise_all_.store(-1, std::memory_order_relaxed);
                        sync_layer_mirror();
                        std::cout << "[DSP] CLEAN " << (on ? "on" : "off")
                                  << " (" << layers_.size() << " layers)." << std::endl;
                    }
                } else if (dn >= 0) {
                    req_denoise_all_.store(-1, std::memory_order_relaxed);
                }

                // כניסה לאוברדאב: מקפיאים את סכום השכבות הקיימות (committed_mix_)
                // ופותחים שכבה *חדשה* ריקה. המוניטור = committed_mix_ + new_layer_,
                // מתפרסם בכל השלמת סיבוב. כלום לא נכתב לחוצץ שהרמקול קורא ממנו.
                auto enter_overdub = [&]() {
                    render_committed_raw();
                    new_layer_.assign(loop_length_, 0.0f);
                    // מיקס-ממתין (שלב 2) מתייתר: מוניטור האוברדאב יפרסם מיקסים
                    // מצטברים חדשים ממילא, ופרסום ה"טהור" הישן היה דורס אותם.
                    cancel_pending_pure();
                    // --- פיצוי לייטנסי הלוך-ושוב ---
                    // הדגימה שמגיעה מהמיקרופון "עכשיו" נוגנה על-ידי המשתמש כנגד
                    // מה ששמע לפני (לייטנסי-פלט + לייטנסי-קלט). בלי הפיצוי כל
                    // אוברדאב נכתב באיחור סיסטמטי של ~15-40ms — "פלאם" שמעי מול
                    // הבסיס. ההיסט קבוע לכל משך האוברדאב ⇒ עיגון-כניסה מספיק.
                    // (שיארית זמן-התור של ה-Worker ~0-1 צ'אנק נותרת — ג'יטר קטן
                    // שאין לו מדידה זולה; הפיצוי מסיר את הרכיב הדטרמיניסטי.)
                    // הנוסחה זהה בכניסה מיידית ובכניסה מוצמדת: היא נגזרת מהראש-קריאה
                    // *ברגע הכניסה*, ולכן ההצמדה אינה משנה בה דבר.
                    size_t comp = static_cast<size_t>(
                        std::max(0, overdub_comp_samples_.load(std::memory_order_relaxed))) % loop_length_;
                    overdub_idx = (playback_read_idx_.load(std::memory_order_relaxed) % loop_length_
                                   + loop_length_ - comp) % loop_length_;
                    overdub_publish_pending = false;
                    current_state_.store(LooperState::OVERDUBBING, std::memory_order_release);
                };

                if (request_overdub_.exchange(false, std::memory_order_relaxed)) {
                    if (overdub_armed) {
                        // נגיעה שנייה בזמן המתנה = ביטול. בלי זה המשתמש "לכוד"
                        // עד ראש הלופ הבא ללא דרך לחזור בו.
                        overdub_armed = false;
                        overdub_armed_.store(false, std::memory_order_relaxed);
                        std::cout << "[DSP] Armed overdub cancelled." << std::endl;
                    } else if (loop_length_ > 0 && !layers_.empty() &&
                               static_cast<int>(layers_.size()) < kMaxLayers) {
                        if (overdub_quantize_.load(std::memory_order_relaxed)) {
                            overdub_armed = true;
                            overdub_armed_.store(true, std::memory_order_relaxed);
                            armed_prev_play_idx = playback_read_idx_.load(std::memory_order_relaxed) % loop_length_;
                        } else {
                            enter_overdub();
                        }
                    }
                }

                // המתנה לראש הלופ: העטיפה מזוהה מירידת ראש-הקריאה. גרעיניות
                // הצ'אנק (~11ms) אינה פוגעת ביישור — enter_overdub עוגן בראש-הקריאה
                // האמיתי של רגע הכניסה, לא בהנחה שהוא אפס.
                // מוניטור שהושתק/נעצר מבטל שכבה ממתינה. קריטי ולא קוסמטי: בעצירה
                // ראש-הקריאה *מוצמד לאפס* ע"י קולבק הפלט, ומבחן העטיפה
                // (now < prev) היה נדלק מיד — האוברדאב היה נכנס דווקא כשאין מה
                // לשמוע, ומקליט את הנגן מול שקט. אותו היגיון להשתקה.
                if (overdub_armed &&
                    (transport_stopped_.load(std::memory_order_relaxed) ||
                     output_muted_.load(std::memory_order_relaxed))) {
                    overdub_armed = false;
                    overdub_armed_.store(false, std::memory_order_relaxed);
                    std::cout << "[DSP] Armed overdub cancelled (monitor silent)." << std::endl;
                }

                if (overdub_armed && loop_length_ > 0) {
                    size_t now_idx = playback_read_idx_.load(std::memory_order_relaxed) % loop_length_;
                    if (now_idx < armed_prev_play_idx) {
                        overdub_armed = false;
                        overdub_armed_.store(false, std::memory_order_relaxed);
                        enter_overdub();
                    } else {
                        armed_prev_play_idx = now_idx;
                    }
                }
                break;
            }

            case LooperState::OVERDUBBING: {
                const bool auto_close = overdub_auto_close_.load(std::memory_order_relaxed);
                if (loop_length_ > 0 && new_layer_.size() == loop_length_) {
                    for (float s : local_chunk) {
                        // בסגירה-אוטומטית מפסיקים לצבור *ברגע* העטיפה: המשך הצ'אנק
                        // שייך כבר לסיבוב הבא, וצבירתו הייתה מכפילה ~10ms על ראש
                        // הלופ (פלאם בתפר). במצב הידני אין עטיפה-סופית מוגדרת ולכן
                        // ההתנהגות נשארת בדיוק כשהייתה.
                        if (auto_close && overdub_publish_pending) break;
                        new_layer_[overdub_idx] += s;   // צבירה גולמית לשכבה; ה-clip קורה במיקס
                        overdub_idx++;
                        if (overdub_idx >= loop_length_) {
                            overdub_idx = 0;
                            overdub_publish_pending = true;   // נשמע בסיבוב הבא
                        }
                    }
                }

                // סגירה אוטומטית אחרי סיבוב אחד מלא: overdub_publish_pending נדלק
                // בדיוק כשה-idx עטף — כלומר נצבר loop_length_ דגימות מנקודת הכניסה,
                // סיבוב שלם. זה מבטל את *הנקישה השנייה* לכל שכבה (הנקישה שהרגה את
                // הבטחת ה"בלי-מגע" מהשכבה השנייה והלאה). הבקשה הידנית עדיין גוברת,
                // כך שכיבוי ההגדרה מחזיר בדיוק את ההתנהגות הקודמת.
                bool exit_requested = request_looping_.load(std::memory_order_relaxed) ||
                                      (overdub_publish_pending && auto_close);

                if (overdub_publish_pending || exit_requested) {
                    // מוניטור/פרסום = committed_mix_ (גולמי) + new_layer_, עם ברך-רכה.
                    mix_temp_.assign(loop_length_, 0.0f);
                    for (size_t i = 0; i < loop_length_; ++i)
                        mix_temp_[i] = soft_clip_knee(committed_mix_[i] + new_layer_[i]);
                    if (publish_mix_temp(/*reset_read=*/false)) {   // אורך זהה — בלי קפיצה
                        overdub_publish_pending = false;
                        if (exit_requested) {
                            request_looping_.store(false, std::memory_order_relaxed);
                            // קיבוע השכבה החדשה כשכבה נפרדת (dry+samples = הדאב, בלי fx).
                            // כניסת-האוברדאב חסמה בתקרה, ולכן מובטח מקום כאן.
                            Layer nl = make_layer(new_layer_);
                            // ירושת CLEAN: אם כל השכבות הקיימות נקיות — גם הדאב החדש
                            // (הטוגל מייצג "נקה את המיקס", לא רגע-בזמן). הפרסום המעודכן
                            // הוא Best-Effort; Slot תפוס ⇒ הפקודה הגלובלית תרפא בהמשך.
                            bool inherit_clean = !layers_.empty() &&
                                std::all_of(layers_.begin(), layers_.end(),
                                            [](const Layer& L) { return L.denoise; });
                            if (inherit_clean) { nl.denoise = true; render_layer(nl); }
                            layers_.push_back(std::move(nl));
                            sync_layer_mirror();
                            if (inherit_clean) {
                                render_full_mix_excluding(-1, mix_temp_);
                                if (!publish_mix_smoothed())
                                    req_denoise_all_.store(1, std::memory_order_relaxed);
                            }
                            new_layer_.clear();
                            new_layer_.shrink_to_fit();
                            current_state_.store(LooperState::LOOPING, std::memory_order_release);
                        }
                    }
                    // slot תפוס: נדחה; new_layer_ ממשיך לצבור, דבר לא אובד
                }
                break;
            }
        }

        // --- טלמטריה למחקר כוונון (Harness אופליין; null במכשיר → עלות ענף בודד) ---
        if (TelemetrySink sink = telemetry_sink_.load(std::memory_order_relaxed)) {
            ChunkTelemetry t;
            t.time_seconds = static_cast<double>(total_samples_processed) / config_.sample_rate;
            t.state_before = state;
            t.state_after = current_state_.load(std::memory_order_relaxed);
            t.raw_rms = raw_volume;
            t.trigger_rms = trigger_volume;
            t.rise_ratio = trigger_volume / std::max(prev_trigger_volume, 1e-9f);
            float min_onset = min_onset_rms_.load(std::memory_order_relaxed);
            t.onset_level_threshold = std::max(noise_tracker_.get_onset_threshold(), min_onset);
            float t_raw_mean = raw_noise_tracker_.get_mean();
            t.sustain_threshold = std::max({raw_noise_tracker_.get_silence_threshold(),
                                            t_raw_mean * sustain_rel_mult_.load(std::memory_order_relaxed),
                                            raw_sustain_floor_.load(std::memory_order_relaxed)});
            t.silence_threshold = std::max({raw_noise_tracker_.get_silence_threshold(),
                                            t_raw_mean * silence_rel_mult_.load(std::memory_order_relaxed),
                                            silence_abs_floor_.load(std::memory_order_relaxed)});
            t.peak_envelope = peak_envelope;
            t.onset_streak = current_onset_streak_;
            t.noise_mean_trigger = noise_tracker_.get_mean();
            t.noise_std_trigger = noise_tracker_.get_std_dev();
            t.noise_mean_raw = raw_noise_tracker_.get_mean();
            t.noise_std_raw = raw_noise_tracker_.get_std_dev();
            t.yin_confidence = current_yin;
            t.published_loop_samples = last_published_loop_samples;
            last_published_loop_samples = 0;   // מדווח פעם אחת, בצ'אנק הפרסום
            t.input_overrun_count = input_overrun_count_.load(std::memory_order_relaxed);
            sink(t, telemetry_user_.load(std::memory_order_relaxed));
        }

        // נקודת הייחוס של מבחן העלייה לצ'אנק הבא
        prev_trigger_volume = trigger_volume;
        local_chunk.clear();
    }
}

size_t LooperEngine::feed_audio_offline(const float* data, size_t num_samples) {
    size_t accepted = 0;
    while (accepted < num_samples && input_queue_.push(data[accepted])) {
        ++accepted;
    }
    return accepted;
}

bool LooperEngine::export_to_wav(const char* filepath) {
    // צילום מצב תחת נעילה קצרה — ואז כתיבת הקובץ האיטית מחוץ לנעילה,
    // כדי שה-Worker לא יחליף/יכתוב את הווקטור באמצע ה-I/O
    std::vector<float> snapshot;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        int active_idx = active_playback_idx_.load(std::memory_order_acquire);
        snapshot = playback_buffers_[active_idx];
    }

    if (snapshot.empty()) {
        return false;
    }

    // המיקס המפורסם כבר מכיל *הכל*: אפקטים פר-שכבה, ריברב פר-שכבה (נצרב על-ידי
    // ה-Worker אל samples) ועוצמות. שום צריבה נוספת כאן — הייצוא זהה להשמעה
    // בהגדרה. (הבייק-בייצוא הישן של הריברב הגלובלי נמחק יחד עם הנתיב הגלובלי;
    // אילו נשאר, היה מוסיף ריברב *כפול* על שכבות שכבר נצרבו.)

    // ייצוא כ-PCM 16-bit: פורמט האודיו האוניברסלי. WAV IEEE-float (הפורמט הקודם)
    // אינו מזוהה תמיד כאודיו על-ידי סורק המדיה של אנדרואיד → הקובץ "נעלם" מבוררי
    // הקבצים המבוססי-MediaStore ולא ניתן לבחור אותו. 16-bit נתמך בכל מקום, חצי
    // גודל, וללא אובדן איכות נשמע לגיטרה.
    std::vector<ma_int16> pcm16(snapshot.size());
    ma_pcm_f32_to_s16(pcm16.data(), snapshot.data(), snapshot.size(), ma_dither_mode_triangle);

    ma_encoder_config config = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 1, config_.sample_rate);
    ma_encoder encoder;

    if (ma_encoder_init_file(filepath, &config, &encoder) != MA_SUCCESS) {
        std::cerr << "[I/O Error] Failed to initialize WAV encoder." << std::endl;
        return false;
    }

    // שפיכת הזיכרון לקובץ
    ma_encoder_write_pcm_frames(&encoder, pcm16.data(), pcm16.size(), nullptr);
    ma_encoder_uninit(&encoder);

    std::cout << "[I/O] Successfully exported loop to: " << filepath << std::endl;
    return true;
}

bool LooperEngine::import_from_wav(const char* filepath) {
    // הגדרת המפענח: אנחנו כופים עליו להמיר כל קובץ אודיו שייכנס לפורמט שהמנוע שלנו מבין
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, config_.sample_rate);
    ma_decoder decoder;

    if (ma_decoder_init_file(filepath, &config, &decoder) != MA_SUCCESS) {
        std::cerr << "[I/O Error] Failed to load or decode audio file." << std::endl;
        return false;
    }

    ma_uint64 frame_count;
    ma_decoder_get_length_in_pcm_frames(&decoder, &frame_count);

    std::vector<float> loaded_audio(frame_count);
    ma_uint64 frames_read = 0;
    ma_decoder_read_pcm_frames(&decoder, loaded_audio.data(), frame_count, &frames_read);
    ma_decoder_uninit(&decoder);

    if (frames_read == 0) return false;
    loaded_audio.resize(frames_read); // הידוק הזיכרון במקרה של שגיאת אורך

    // קיפול-קצה על קובץ מיובא *רק אם תפרו אינו רציף* (שומר-הרציפות שבפנים):
    // לופ מוכן-מראש עובר ללא נגיעה; חיתוך גס מקבל תפר בסגנון הקיפול שנמדד נקי.
    loaded_audio = apply_seam_fold(loaded_audio);

    // אין רשת פעימות לאודיו מיובא שרירותי — לא הרצנו עליו אומדן קצב.
    loop_beats_.store(0.0f, std::memory_order_relaxed);
    estimated_bpm_.store(0.0f, std::memory_order_relaxed);

    // --- מסירה ל-Worker במקום פרסום ישיר ---
    // רק ה-Worker רשאי לפרסם חוצצים: פרסום משני Threads שובר את מודל ה-RCU
    // (שני מפרסמים עלולים לבחור את אותו Slot ולדרוס זה את זה).
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        pending_import_ = std::move(loaded_audio);
    }
    has_pending_import_.store(true, std::memory_order_release);

    std::cout << "[I/O] Successfully decoded loop — handed off to DSP worker." << std::endl;
    return true;
}

bool LooperEngine::save_session(const char* filepath) {
    // רק ה-Worker רואה את layers_ — רושמים בקשה וממתינים (Poll) לתוצאה.
    // הסנכרוניות הכרחית: onStop → onCleared (פירוק מלא) רצים ברצף, וכתיבה
    // א-סינכרונית הייתה מתחרה במנוע שנהרס. ה-Worker מגיב תוך איטרציה (~µs-ms
    // כשאין אודיו, צ'אנק אחד כשיש); ה-Timeout הוא רשת ביטחון בלבד.
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        pending_save_path_ = filepath;
    }
    session_save_result_.store(0, std::memory_order_relaxed);
    request_save_session_.store(true, std::memory_order_release);
    for (int i = 0; i < 2000; ++i) {                       // עד ~2s
        int r = session_save_result_.load(std::memory_order_acquire);
        if (r != 0) return r > 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool LooperEngine::load_session(const char* filepath) {
    // פענוח על Thread ה-JNI (כמו Import), אימות מבני, ומסירה ל-Worker — המפרסם
    // היחיד. אימות התאמת קצב-הדגימה נעשה אצל ה-Worker (config_ בבעלותו).
    std::ifstream f(filepath, std::ios::binary);
    if (!f) return false;
    char magic[4]; uint32_t ver = 0, srate = 0, count = 0; uint64_t len64 = 0;
    float bpm = 0.0f, beats = 0.0f;
    auto get = [&](void* p, size_t n) { f.read(static_cast<char*>(p), n); };
    get(magic, 4); get(&ver, 4); get(&srate, 4); get(&count, 4);
    get(&len64, 8); get(&bpm, 4); get(&beats, 4);
    if (!f || std::memcmp(magic, kSessionMagic, 4) != 0 || ver != kSessionVersion) return false;
    // גבולות שפיות: עד תקרת השכבות, ואורך עד ~400s בקצב המוצהר (רצפת ההקלטה 300s)
    if (count < 1 || count > static_cast<uint32_t>(kMaxLayers)) return false;
    if (srate < 8000 || srate > 192000) return false;
    if (len64 < 1 || len64 > static_cast<uint64_t>(srate) * 400ULL) return false;

    std::vector<SessionLayer> sess(count);
    for (uint32_t k = 0; k < count; ++k) {
        int32_t fx32 = 0, dn32 = 0;
        get(&sess[k].gain, 4); get(&fx32, 4); get(&sess[k].reverb, 4); get(&dn32, 4);
        sess[k].fx = (fx32 >= 0 && fx32 <= 3) ? fx32 : 0;
        sess[k].denoise = (dn32 == 1);
        sess[k].dry.resize(static_cast<size_t>(len64));
        get(sess[k].dry.data(), static_cast<size_t>(len64) * sizeof(float));
        if (!f) return false;   // קובץ קטום/מושחת — נכשל נקי, שום דבר לא נמסר
    }
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        pending_session_ = std::move(sess);
        pending_session_rate_ = static_cast<int>(srate);
        pending_session_bpm_ = bpm;
        pending_session_beats_ = beats;
    }
    has_pending_session_.store(true, std::memory_order_release);
    std::cout << "[I/O] Session decoded (" << count << " layers) — handed off to DSP worker." << std::endl;
    return true;
}

float LooperEngine::calculate_trigger_rms(const std::vector<float>& chunk) {
    if (chunk.empty()) return 0.0f;
    float sum_squares = 0.0f;
    for (float sample : chunk) {
        // סינון תדרים נמוכים - מעלים רעשי גוף ומזגן
        float filtered = sample - 0.95f * pre_emphasis_state_;
        pre_emphasis_state_ = sample;
        sum_squares += filtered * filtered;
    }
    return std::sqrt(sum_squares / chunk.size());
}
