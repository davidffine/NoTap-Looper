#include "../headers/LooperEngine.hpp"
#include "../includes/kissfft/kiss_fftr.h"
#include "../headers/miniaudio.h"
#include <iostream>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <algorithm>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

// פרמטרי הריברב — מקור אמת יחיד, כדי שהעיבוד החי וה-Bake ל-Export יישמעו זהים.
namespace {
    constexpr float REVERB_ROOM = 0.72f;
    constexpr float REVERB_DAMP = 0.45f;
    constexpr float REVERB_WET  = 0.55f;
}

// FTZ/DAZ הם מצב של רגיסטר ה-FP *של ה-Thread הנוכחי*.
// חייבים להיקרא מתוך ה-Worker עצמו — קריאה מה-Constructor מגינה על ה-Thread הלא נכון.
// באנדרואיד האמיתי (ARM) הענף של x86 לא מתקמפל כלל, ולכן חובה ענף FPCR/FPSCR ייעודי.
static void enable_denormal_flush_to_zero() {
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
    const float ABSOLUTE_MIN_NOISE = 0.0001f;
    current_std_dev_ = std::max(current_std_dev_, ABSOLUTE_MIN_NOISE);
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
    reverb_.init(config_.sample_rate);
    reverb_.set_params(REVERB_ROOM, REVERB_DAMP);   // "produced" default
    worker_thread_ = std::thread(&LooperEngine::process_audio_asynchronously, this);
}

LooperEngine::~LooperEngine() {
    is_running_.store(false, std::memory_order_relaxed);
    if (worker_thread_.joinable()) worker_thread_.join();
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

void LooperEngine::update_hardware_config(int sample_rate, float latency_seconds) {
    // רישום בלבד — ה-Worker מחיל את השינוי בבטחה בתחילת האיטרציה שלו.
    // שינוי ישיר של config_ או resize של preroll_buffer_ מכאן הוא Data Race.
    if (sample_rate > 0) pending_sample_rate_.store(sample_rate, std::memory_order_relaxed);
    pending_preroll_seconds_.store(latency_seconds + 0.02f, std::memory_order_relaxed);
}

void LooperEngine::on_audio_callback(const float* input_data, size_t num_frames) {
    for (size_t i = 0; i < num_frames; ++i) input_queue_.push(input_data[i]);
}

void LooperEngine::process_output_callback(float* output_data, size_t num_frames) {
    LooperState state = current_state_.load(std::memory_order_relaxed);
    bool playing = (state == LooperState::LOOPING || state == LooperState::OVERDUBBING);

    size_t loop_pos = 0, loop_size = 0;   // ל-lock המטרונום בזמן נגינה

    if (!playing) {
        for (size_t i = 0; i < num_frames; ++i) output_data[i] = 0.0f;
    } else {
        int active_idx = active_playback_idx_.load(std::memory_order_acquire);
        // איתות Grace: ה-Worker רשאי לכתוב לחוצץ הלא-פעיל רק אחרי שנצפינו כאן על הפעיל.
        reader_observed_idx_.store(active_idx, std::memory_order_release);
        const auto& active_buffer = playback_buffers_[active_idx];

        if (active_buffer.empty()) {
            for (size_t i = 0; i < num_frames; ++i) output_data[i] = 0.0f;
        } else {
            size_t buffer_size = active_buffer.size();
            size_t current_idx = playback_read_idx_.load(std::memory_order_relaxed);
            if (current_idx >= buffer_size) [[unlikely]] current_idx = 0;
            loop_pos = current_idx; loop_size = buffer_size;
            for (size_t i = 0; i < num_frames; ++i) {
                output_data[i] = active_buffer[current_idx];
                current_idx++;
                if (current_idx >= buffer_size) [[unlikely]] current_idx = 0;
            }
            playback_read_idx_.store(current_idx, std::memory_order_relaxed);
        }
    }

    // ריברב על אות הלופ (רטוב מתווסף ליבש). מוחל לפני המטרונום כדי שהקליקים
    // יישארו יבשים וברורים. לא-הרסני: החוצץ המוקלט לא משתנה.
    if (playing && reverb_enabled_.load(std::memory_order_relaxed)) {
        for (size_t i = 0; i < num_frames; ++i) {
            float wet = reverb_.process(output_data[i]);
            output_data[i] = std::clamp(output_data[i] + wet * REVERB_WET, -1.0f, 1.0f);
        }
    }

    render_metronome(output_data, num_frames, state, playing, loop_pos, loop_size);
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
    const double TWO_PI = 6.283185307179586;
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
            float s = static_cast<float>(std::sin(click_phase_)) * click_env_;
            out[i] = std::clamp(out[i] + s, -1.0f, 1.0f);
            click_phase_ += TWO_PI * click_freq_ / config_.sample_rate;
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

// פונקציית ה-novelty החדשה, המבוססת על Frequency-Domain
std::vector<float> LooperEngine::extract_novelty_curve(const std::vector<float>& audio_data, int env_sr, int& out_chunk_size) {
    // גודל חלון ה-FFT. קובע את הרזולוציה בתדר מול הזמן.
    const int NFFT = 1024;
    const int HOP_SIZE = NFFT / 2; // חפיפה של 50%

    // קביעת קצב הדגימה הווירטואלי של העקומה
    out_chunk_size = HOP_SIZE;

    std::vector<float> novelty;
    if (audio_data.size() < NFFT) return novelty;

    novelty.reserve(audio_data.size() / HOP_SIZE);

    // אתחול KissFFT עבור ערכים ממשיים
    kiss_fftr_cfg fft_cfg = kiss_fftr_alloc(NFFT, 0, nullptr, nullptr);
    std::vector<kiss_fft_scalar> time_in(NFFT, 0.0f);
    std::vector<kiss_fft_cpx> freq_out(NFFT / 2 + 1);

    std::vector<float> prev_magnitudes(NFFT / 2 + 1, 0.0f);

    // חישוב חלון Hann פעם אחת מראש במקום cos() לכל דגימה בכל חלון
    const float PI_F = 3.14159265358979323846f;
    std::vector<float> hann_window(NFFT);
    for (int j = 0; j < NFFT; ++j) {
        hann_window[j] = 0.5f * (1.0f - std::cos(2.0f * PI_F * j / (NFFT - 1)));
    }

    // מעבר על האודיו בחלונות קופצים
    for (size_t i = 0; i + NFFT <= audio_data.size(); i += HOP_SIZE) {
        // 1. החלת חלון (Hann Window) למניעת זליגת תדרים בקצוות
        for (int j = 0; j < NFFT; ++j) {
            time_in[j] = audio_data[i + j] * hann_window[j];
        }

        // 2. ביצוע התמרת פורייה
        kiss_fftr(fft_cfg, time_in.data(), freq_out.data());

        float current_flux = 0.0f;

        // 3. חישוב ה-Spectral Flux (המשוואה המדוברת)
        for (int k = 0; k <= NFFT / 2; ++k) {
            // חישוב מגניטודה של המספר המרוכב
            float real = freq_out[k].r;
            float imag = freq_out[k].i;
            float magnitude = std::sqrt(real * real + imag * imag);

            // חישוב ההפרש בין החלון הנוכחי לקודם
            float diff = magnitude - prev_magnitudes[k];

            // Half-wave rectification (הוספת אנרגיה חדשה בלבד)
            if (diff > 0.0f) {
                current_flux += diff;
            }

            prev_magnitudes[k] = magnitude;
        }

        novelty.push_back(current_flux);
    }

    kiss_fftr_free(fft_cfg);

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
    std::vector<float> novelty = extract_novelty_curve(segment, 0, hop);
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

std::vector<float> LooperEngine::apply_zero_crossing_crossfade(std::vector<float>& audio, size_t crossfade_samples) {
    if (audio.size() <= crossfade_samples * 2) return audio;
    size_t len = audio.size();
    std::vector<float> result = audio;
    size_t best_end = len - 1;
    for (size_t i = len - 1; i > len - crossfade_samples; --i) {
        if (audio[i] * audio[i-1] <= 0.0f) { best_end = i; break; }
    }
    result.resize(best_end + 1);
    len = result.size();
    const float PI = 3.14159265358979323846f;
    for (size_t i = 0; i < crossfade_samples; ++i) {
        float progress = static_cast<float>(i) / static_cast<float>(crossfade_samples);
        float fade_out = 0.5f * (1.0f + std::cos(progress * PI));
        result[len - crossfade_samples + i] *= fade_out;
    }
    return result;
}

std::vector<float> LooperEngine::apply_loop_effect(const std::vector<float>& src, int fx) {
    std::vector<float> dst;
    if (src.empty()) return dst;
    auto lerp = [&](double pos) -> float {
        if (pos <= 0.0) return src.front();
        size_t i = static_cast<size_t>(pos);
        if (i + 1 >= src.size()) return src.back();
        float f = static_cast<float>(pos - i);
        return src[i] * (1.0f - f) + src[i + 1] * f;
    };
    switch (fx) {
        case 1: // reverse — same length, stays in sync
            dst.assign(src.rbegin(), src.rend());
            break;
        case 2: { // octave up — read at 2× (higher pitch, half length)
            dst.resize(src.size() / 2);
            for (size_t i = 0; i < dst.size(); ++i) dst[i] = lerp(i * 2.0);
            break;
        }
        case 3: { // octave down — read at 0.5× (lower pitch, double length)
            dst.resize(src.size() * 2);
            for (size_t i = 0; i < dst.size(); ++i) dst[i] = lerp(i * 0.5);
            break;
        }
        default: return dst;
    }
    dst = apply_zero_crossing_crossfade(dst, 256);   // clean seam after transform
    return dst;
}

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

    // סקראץ' אוברדאב פרטי ל-Worker: כל המיקסים קורים כאן,
    // והתוצאה מתפרסמת לחוצץ הכפול פעם בכל השלמת סיבוב לופ.
    std::vector<float> overdub_scratch;
    size_t overdub_idx = 0;
    bool overdub_publish_pending = false;

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

    // --- Spectral Flux זורם (novelty בזמן-אמת) ---
    // רץ על ה-Worker (לא על ה-Thread של Oboe), ולכן הקצאת KissFFT מותרת.
    // חלון 1024 עם hop של צ'אנק בודד; Flux = סכום ההפרשים החיוביים במגניטודה
    // מול המסגרת הקודמת (Half-wave rectified). מזהה תו חדש לפי *חידוש ספקטרלי*
    // ולא לפי אנרגיה גולמית — לכן רגיש לפריטת מיתר E דק גם ברמה נמוכה.
    const int FLUX_NFFT = 1024;
    const float PI_F = 3.14159265358979323846f;
    kiss_fftr_cfg flux_cfg = kiss_fftr_alloc(FLUX_NFFT, 0, nullptr, nullptr);
    std::vector<kiss_fft_scalar> flux_time_in(FLUX_NFFT, 0.0f);
    std::vector<kiss_fft_cpx> flux_freq_out(FLUX_NFFT / 2 + 1);
    std::vector<float> flux_prev_mag(FLUX_NFFT / 2 + 1, 0.0f);
    std::vector<float> flux_window(FLUX_NFFT);
    for (int j = 0; j < FLUX_NFFT; ++j)
        flux_window[j] = 0.5f * (1.0f - std::cos(2.0f * PI_F * j / (FLUX_NFFT - 1)));
    std::vector<float> flux_ring(FLUX_NFFT, 0.0f);   // חלון גולל של NFFT הדגימות האחרונות

    auto compute_flux = [&](const std::vector<float>& chunk) -> float {
        // דחיפת הצ'אנק החדש ושמירת NFFT הדגימות האחרונות
        flux_ring.insert(flux_ring.end(), chunk.begin(), chunk.end());
        if (flux_ring.size() > static_cast<size_t>(FLUX_NFFT))
            flux_ring.erase(flux_ring.begin(), flux_ring.end() - FLUX_NFFT);
        for (int j = 0; j < FLUX_NFFT; ++j) flux_time_in[j] = flux_ring[j] * flux_window[j];
        kiss_fftr(flux_cfg, flux_time_in.data(), flux_freq_out.data());
        float flux = 0.0f;
        for (int k = 0; k <= FLUX_NFFT / 2; ++k) {
            float mag = std::sqrt(flux_freq_out[k].r * flux_freq_out[k].r +
                                  flux_freq_out[k].i * flux_freq_out[k].i);
            float diff = mag - flux_prev_mag[k];
            if (diff > 0.0f) flux += diff;      // Half-wave rectification
            flux_prev_mag[k] = mag;
        }
        return flux;
    };

    // סף Flux אדפטיבי: חציון + k·MAD על היסטוריית ה-Flux האחרונה (חסין לספייקים).
    std::vector<float> flux_hist(64, 0.0f);
    size_t flux_hist_idx = 0;
    float current_flux = 0.0f;
    float current_flux_threshold = 0.0f;

    auto recompute_time_constants = [&]() {
        float chunk_seconds = static_cast<float>(config_.chunk_size) / static_cast<float>(config_.sample_rate);
        env_release_coef = std::exp(-chunk_seconds / ENV_RELEASE_SECONDS);
        max_record_samples = static_cast<size_t>(MAX_RECORD_SECONDS * config_.sample_rate);
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

        // --- מסירת לופ מיובא (פוענח על JNI, מתפרסם רק כאן) ---
        if (has_pending_import_.load(std::memory_order_acquire)) {
            int slot = acquire_writable_slot();
            if (slot >= 0) {
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    playback_buffers_[slot] = std::move(pending_import_);
                    pending_import_.clear();
                }
                has_pending_import_.store(false, std::memory_order_release);
                playback_read_idx_.store(0, std::memory_order_relaxed);
                active_playback_idx_.store(slot, std::memory_order_release);
                current_state_.store(LooperState::LOOPING, std::memory_order_release);
                std::cout << "[I/O] Imported loop injected to DSP." << std::endl;
            }
            // slot == -1: הקורא עוד לא נצפה — ננסה שוב באיטרציה הבאה
        }

        if (request_clear_.exchange(false, std::memory_order_relaxed)) {
            recorded_audio.clear();
            overdub_scratch.clear();
            overdub_publish_pending = false;
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
            local_chunk.push_back(sample);
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

        // Spectral Flux זורם — כלי מדידה אופליין בלבד. המדידה הוכיחה שה-Flux
        // *אינו* מבחין נקי (טרנזיינט AC/נקישה מייצר Flux בגובה פריטה רכה, וה-Flux
        // גדל עם עוצמה), ולכן איננו מזהה ההתקף הראשי. מחשבים אותו רק כשה-Sink פעיל
        // כדי לא לשלם FFT לכל צ'אנק במכשיר לחינם.
        bool telemetry_active = (telemetry_sink_.load(std::memory_order_relaxed) != nullptr);
        if (telemetry_active) {
            current_flux = compute_flux(local_chunk);
            std::vector<float> sorted(flux_hist);
            std::sort(sorted.begin(), sorted.end());
            float median = sorted[sorted.size() / 2];
            float mad_sum = 0.0f;
            for (float v : sorted) mad_sum += std::abs(v - median);
            float mad = mad_sum / sorted.size();
            current_flux_threshold = median + 4.0f * mad;
        }

        current_rms_.store(raw_volume, std::memory_order_relaxed);
        current_noise_std_dev_.store(noise_tracker_.get_std_dev(), std::memory_order_relaxed);

        LooperState state = current_state_.load(std::memory_order_relaxed);

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
                    // הרצפה המוחלטת מנותקת מ-min_onset: היא המבחין העיקרי בין פריטה
                    // (מסת מיתר ≥0.09) לטרנזיינט סביבתי (≤0.05), וכוילה אמפירית מול הקורפוס.
                    float absolute_sustain_floor = raw_sustain_floor_.load(std::memory_order_relaxed);
                    float sustain_threshold = std::max(raw_noise_tracker_.get_silence_threshold(), absolute_sustain_floor);

                    bool is_sustaining = (raw_volume > sustain_threshold);

                    if (is_sustaining) {
                        current_onset_streak_++; // האנרגיה ממשיכה להתקיים בעולם
                    } else {
                        current_onset_streak_ = 0; // האנרגיה מתה מהר מדי. זו הייתה נגיעה אקראית. מוחקים.
                    }
                }

                // 2. קבלת החלטה
                // המכונה תאשר הקלטה רק אם הרצף שרד 4 חלונות (התקף + 3 חלונות תהודה יציבים).
                bool auto_triggered = (mode != 1) && (current_onset_streak_ >= onset_persistence_target_.load(std::memory_order_relaxed));
                bool manual_triggered = (mode == 1) && request_record_start_.exchange(false, std::memory_order_relaxed);

                if (auto_triggered || manual_triggered) {
                    current_onset_streak_ = 0;
                    recorded_audio.clear();

                    // שאיבת הזמן האבוד: אנו מושכים את האודיו מה-Preroll, כך שכל 4 החלונות שעליהם התלבטנו
                    // (כולל הטרנזיינט הראשוני) מוכנסים במלואם להקלטה הסופית ללא קטיעות.
                    for (size_t i = 0; i < max_preroll_samples; ++i) {
                        size_t read_idx = (preroll_write_idx_ + i) % max_preroll_samples;
                        recorded_audio.push_back(preroll_buffer_[read_idx]);
                    }

                    peak_envelope = raw_volume;
                    session_peak_raw = raw_volume;    // רצפת הפעילות נבנית מהסשן הזה בלבד
                    silence_samples_count = 0;
                    inactivity_count = 0;
                    trailing_non_musical_samples = 0;
                    current_state_.store(LooperState::RECORDING, std::memory_order_release);
                } else {
                    // 3. עדכון סביבה דינמי — אך *רק* כשהצ'אנק באמת נראה כמו רקע.
                    // באג קריטי שתוקן: התקף אמיתי שלא הצית מיד את שלב א' (למשל פריטה
                    // רכה עם קליק חלש) נלמד כ"רעש", הסף ברח כלפי מעלה מהר מהאות עצמו,
                    // וההתקף הפך בלתי-ניתן-לזיהוי לצמיתות. הקפאת הלמידה בכל אנרגיה מעל
                    // חצי רצפת התהודה מונעת את הרעלת רצפת הרעש.
                    float activity_floor = raw_sustain_floor_.load(std::memory_order_relaxed) * 0.5f;
                    bool looks_like_background = (raw_volume < activity_floor);
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
                    float sil_floor = std::max(raw_noise_tracker_.get_silence_threshold(),
                                               silence_abs_floor_.load(std::memory_order_relaxed));
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

                    // סגירה על המוקדם מבין השניים. הזנב הלא-מוזיקלי לקוונטיזציה הוא
                    // הזמן מאז שהפעילות פסקה — תחילת הדעיכה ≈ הסיום המוזיקלי, ולכן
                    // אין צורך בתיקוני "+פעימה" הוריסטיים.
                    if (silence_samples_count >= silence_target ||
                        inactivity_count >= inactivity_target) {
                        trailing_non_musical_samples = std::max(silence_samples_count, inactivity_count);
                        current_state_.store(LooperState::PROCESSING, std::memory_order_release);
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

                // Crossfade קצה מופעל רק כשאין זנב מקופל: הוא משנה את אורך הלופ
                // (עד 256 דגימות — סחיפת טמפו מצטברת) ומאפס את הקצה, מה ששובר את
                // רציפות התפר שהקיפול כבר מבטיח. עם קיפול — התפר רציף מעצם הבנייה.
                if (!tail_folded) {
                    final_loop = apply_zero_crossing_crossfade(final_loop, 256);
                }

                if (!final_loop.empty()) {
                    last_published_loop_samples = final_loop.size();
                    // הקורא רדום במצב PROCESSING, ולכן החוצץ מוענק מיידית
                    int slot = acquire_writable_slot();
                    if (slot < 0) slot = 1 - active_playback_idx_.load(std::memory_order_relaxed);
                    {
                        std::lock_guard<std::mutex> lock(buffer_mutex_);
                        playback_buffers_[slot] = std::move(final_loop);
                    }
                    playback_read_idx_.store(0, std::memory_order_relaxed);
                    active_playback_idx_.store(slot, std::memory_order_release);
                    current_state_.store(LooperState::LOOPING, std::memory_order_release);
                } else {
                    current_state_.store(LooperState::IDLE, std::memory_order_release);
                }
                break;
            }

            case LooperState::LOOPING: {
                // --- אפקטים חיים על הלופ (Reverse / Octave) ---
                int fx = request_effect_.load(std::memory_order_relaxed);
                if (fx != 0) {
                    int slot = acquire_writable_slot();   // רק כשהקורא נצפה (בטיחות RCU)
                    if (slot >= 0) {
                        request_effect_.store(0, std::memory_order_relaxed);
                        std::vector<float> src;
                        {
                            std::lock_guard<std::mutex> lock(buffer_mutex_);
                            src = playback_buffers_[active_playback_idx_.load(std::memory_order_relaxed)];
                        }
                        std::vector<float> transformed = apply_loop_effect(src, fx);
                        if (!transformed.empty()) {
                            {
                                std::lock_guard<std::mutex> lock(buffer_mutex_);
                                playback_buffers_[slot] = std::move(transformed);
                            }
                            playback_read_idx_.store(0, std::memory_order_relaxed);
                            active_playback_idx_.store(slot, std::memory_order_release);
                            if (fx != 1) loop_beats_.store(0.0f, std::memory_order_relaxed); // אוקטבה משנה אורך → רשת לא תקפה
                        }
                    }
                }

                if (request_overdub_.exchange(false, std::memory_order_relaxed)) {
                    // כניסה לאוברדאב: מעתיקים את הלופ החי לסקראץ' פרטי של ה-Worker.
                    // כל הכתיבות יקרו בסקראץ' — לעולם לא בחוצץ שהרמקול קורא ממנו.
                    {
                        std::lock_guard<std::mutex> lock(buffer_mutex_);
                        overdub_scratch = playback_buffers_[active_playback_idx_.load(std::memory_order_relaxed)];
                    }
                    if (!overdub_scratch.empty()) {
                        overdub_idx = playback_read_idx_.load(std::memory_order_relaxed) % overdub_scratch.size();
                        overdub_publish_pending = false;
                        current_state_.store(LooperState::OVERDUBBING, std::memory_order_release);
                    }
                }
                break;
            }

            case LooperState::OVERDUBBING: {
                if (!overdub_scratch.empty()) {
                    for (float s : local_chunk) {
                        overdub_scratch[overdub_idx] = mix_and_soft_clip(overdub_scratch[overdub_idx], s);
                        overdub_idx++;
                        if (overdub_idx >= overdub_scratch.size()) {
                            overdub_idx = 0;
                            // פרסום בסוף כל סיבוב — השכבה החדשה נשמעת בסיבוב הבא
                            overdub_publish_pending = true;
                        }
                    }
                }

                bool exit_requested = request_looping_.load(std::memory_order_relaxed);

                if (overdub_publish_pending || exit_requested) {
                    int slot = acquire_writable_slot();
                    if (slot >= 0) {
                        {
                            std::lock_guard<std::mutex> lock(buffer_mutex_);
                            playback_buffers_[slot] = overdub_scratch;   // העתקה — הסקראץ' ממשיך לשמש אותנו
                        }
                        // אורך זהה בדיוק — הקורא ממשיך מאותו אינדקס בלי קפיצה
                        active_playback_idx_.store(slot, std::memory_order_release);
                        overdub_publish_pending = false;

                        if (exit_requested) {
                            request_looping_.store(false, std::memory_order_relaxed);
                            overdub_scratch.clear();
                            overdub_scratch.shrink_to_fit();
                            current_state_.store(LooperState::LOOPING, std::memory_order_release);
                        }
                    }
                    // slot == -1: נדחה לצ'אנק הבא; הדאבים ממשיכים להצטבר בסקראץ', דבר לא אובד
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
            t.sustain_threshold = std::max(raw_noise_tracker_.get_silence_threshold(),
                                           raw_sustain_floor_.load(std::memory_order_relaxed));
            t.silence_threshold = std::max(raw_noise_tracker_.get_silence_threshold(),
                                           silence_abs_floor_.load(std::memory_order_relaxed));
            t.peak_envelope = peak_envelope;
            t.onset_streak = current_onset_streak_;
            t.noise_mean_trigger = noise_tracker_.get_mean();
            t.noise_std_trigger = noise_tracker_.get_std_dev();
            t.noise_mean_raw = raw_noise_tracker_.get_mean();
            t.noise_std_raw = raw_noise_tracker_.get_std_dev();
            t.spectral_flux = current_flux;
            t.flux_threshold = current_flux_threshold;
            t.published_loop_samples = last_published_loop_samples;
            last_published_loop_samples = 0;   // מדווח פעם אחת, בצ'אנק הפרסום
            sink(t, telemetry_user_.load(std::memory_order_relaxed));
        }

        // עדכון היסטוריית ה-Flux לסף האדפטיבי (אופליין בלבד, כשה-Sink פעיל)
        if (telemetry_active &&
            (current_flux < current_flux_threshold || current_flux_threshold == 0.0f)) {
            flux_hist[flux_hist_idx] = current_flux;
            flux_hist_idx = (flux_hist_idx + 1) % flux_hist.size();
        }

        // נקודת הייחוס של ה-Flux לצ'אנק הבא
        prev_trigger_volume = trigger_volume;
        local_chunk.clear();
    }

    kiss_fftr_free(flux_cfg);
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

    // אפקטי Reverse/Octave כבר צרובים בחוצץ (הרסניים). את הריברב — שלב Output
    // חי ולא-הרסני — צורבים כאן אם פעיל, כדי שה-Export יישמע כמו ההשמעה. מחממים
    // את הריברב למצב יציב (מריצים את הלופ שוב ושוב) כדי שהזנב יעטוף את התפר
    // וייצא לופ רציף, ואז מקליטים סיבוב אחד של רטוב+יבש.
    if (reverb_enabled_.load(std::memory_order_relaxed)) {
        FreeverbMono rv;
        rv.init(config_.sample_rate);
        rv.set_params(REVERB_ROOM, REVERB_DAMP);
        size_t loop_n = snapshot.size();
        size_t warm_target = std::max(static_cast<size_t>(3.0f * config_.sample_rate), loop_n);
        for (size_t warmed = 0; warmed < warm_target; warmed += loop_n)
            for (size_t i = 0; i < loop_n; ++i) rv.process(snapshot[i]);   // חימום (תוצאה נזרקת)
        std::vector<float> wet_loop(loop_n);
        for (size_t i = 0; i < loop_n; ++i) {
            float wet = rv.process(snapshot[i]);
            wet_loop[i] = std::clamp(snapshot[i] + wet * REVERB_WET, -1.0f, 1.0f);
        }
        snapshot = std::move(wet_loop);
    }

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

    // החלת אלגוריתם ה-Crossfade על הקובץ המיובא כדי להבטיח לופ מושלם ללא קליקים דיגיטליים
    loaded_audio = apply_zero_crossing_crossfade(loaded_audio, 256);

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
