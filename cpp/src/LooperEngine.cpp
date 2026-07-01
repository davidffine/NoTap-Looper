#include "../headers/LooperEngine.hpp"
#include "../includes/kissfft/kiss_fftr.h"
#include <iostream>
#include <cmath>
#include <numeric>
#include <algorithm>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

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
    history_capacity_ = static_cast<size_t>(sample_rate * tracking_window_seconds) / chunk_size;
    if (history_capacity_ < 10) history_capacity_ = 10;
    noise_history_.resize(history_capacity_, 0.0f);
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
float DynamicThresholdTracker::get_onset_threshold() const { return current_mean_ + (SIGMA_MULTIPLIER_ONSET * current_std_dev_); }
float DynamicThresholdTracker::get_silence_threshold() const { return current_mean_ + (SIGMA_MULTIPLIER_SILENCE * current_std_dev_); }
float DynamicThresholdTracker::get_mean() const { return current_mean_; }

LooperEngine::LooperEngine(EngineConfig config) :
        config_(config),
        input_queue_(config.sample_rate * 5),
        noise_tracker_(config.sample_rate, config.chunk_size, 1.0f)
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif

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

float LooperEngine::get_loop_position() const {
    int active_idx = active_playback_idx_.load(std::memory_order_acquire);
    const auto& active_buffer = playback_buffers_[active_idx];
    size_t buffer_size = active_buffer.size();

    if (buffer_size == 0) return 0.0f;

    size_t current_idx = playback_read_idx_.load(std::memory_order_relaxed);
    return static_cast<float>(current_idx) / static_cast<float>(buffer_size);
}

void LooperEngine::update_hardware_latency(float latency_seconds) {
    config_.preroll_seconds = latency_seconds + 0.02f;
    size_t new_max_samples = static_cast<size_t>(config_.preroll_seconds * config_.sample_rate);
    preroll_buffer_.resize(new_max_samples, 0.0f);
    std::cout << "[DSP] Dynamic Preroll calibrated to: " << config_.preroll_seconds << "s based on Hardware." << std::endl;
}

void LooperEngine::on_audio_callback(const float* input_data, size_t num_frames) {
    for (size_t i = 0; i < num_frames; ++i) input_queue_.push(input_data[i]);
}

void LooperEngine::process_output_callback(float* output_data, size_t num_frames) {
    LooperState state = current_state_.load(std::memory_order_relaxed);
    if (state != LooperState::LOOPING && state != LooperState::OVERDUBBING) {
        for (size_t i = 0; i < num_frames; ++i) { output_data[i] = 0.0f; }
        return;
    }

    int active_idx = active_playback_idx_.load(std::memory_order_acquire);
    const auto& active_buffer = playback_buffers_[active_idx];

    if (active_buffer.empty()) {
        for (size_t i = 0; i < num_frames; ++i) { output_data[i] = 0.0f; }
        return;
    }

    size_t current_idx = playback_read_idx_.load(std::memory_order_relaxed);
    size_t buffer_size = active_buffer.size();

    for (size_t i = 0; i < num_frames; ++i) {
        output_data[i] = active_buffer[current_idx];
        current_idx++;
        if (current_idx >= buffer_size) [[unlikely]] current_idx = 0;
    }
    playback_read_idx_.store(current_idx, std::memory_order_relaxed);
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

    // מעבר על האודיו בחלונות קופצים
    for (size_t i = 0; i + NFFT <= audio_data.size(); i += HOP_SIZE) {
        // 1. החלת חלון (Hann Window) למניעת זליגת תדרים בקצוות
        for (int j = 0; j < NFFT; ++j) {
            float hann_multiplier = 0.5f * (1.0f - std::cos(2.0f * M_PI * j / (NFFT - 1)));
            time_in[j] = audio_data[i + j] * hann_multiplier;
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

float LooperEngine::extract_beat_length_from_onsets(const std::vector<float>& audio_data) {
    std::cout << "[DSP] Running Energy-Weighted Transient Histogram Analysis (KissFFT)..." << std::endl;

    if (audio_data.size() < static_cast<size_t>(config_.sample_rate)) {
        estimated_bpm_.store(120.0f, std::memory_order_relaxed);
        return (60.0f / 120.0f) * config_.sample_rate;
    }

    int chunk_size = 0;
    // הפקת עקומת הטרנזיינטים ממרחב התדר (Frequency-Domain)
    std::vector<float> novelty = extract_novelty_curve(audio_data, 0, chunk_size);

    if (novelty.empty() || chunk_size == 0) {
        estimated_bpm_.store(120.0f, std::memory_order_relaxed);
        return (60.0f / 120.0f) * config_.sample_rate;
    }

    float mean_nov = std::accumulate(novelty.begin(), novelty.end(), 0.0f) / novelty.size();
    float peak_threshold = mean_nov * 1.5f;

    struct TransientEvent { int index; float energy; };
    std::vector<TransientEvent> transients;

    // זיהוי חתימות תדר חזקות (Peaks) בעקומת הספקטרום
    for (size_t i = 1; i < novelty.size() - 1; ++i) {
        if (novelty[i] > peak_threshold && novelty[i] > novelty[i - 1] && novelty[i] > novelty[i + 1]) {
            // חסם מינימלי בין פעימות למניעת זיהוי כפול
            if (transients.empty() || (static_cast<int>(i) - transients.back().index) > 2) {
                transients.push_back({static_cast<int>(i), novelty[i]});
            }
        }
    }

    if (transients.size() < 2) {
        estimated_bpm_.store(120.0f, std::memory_order_relaxed);
        return (60.0f / 120.0f) * config_.sample_rate;
    }

    // קצב הדגימה החדש של עקומת ה-novelty
    int env_sr = config_.sample_rate / chunk_size;
    int min_lag_chunks = static_cast<int>((60.0f / 240.0f) * env_sr);
    int max_lag_chunks = static_cast<int>((60.0f / 45.0f) * env_sr);

    std::vector<float> energy_histogram(max_lag_chunks + 1, 0.0f);

    // מילוי ההיסטוגרמה
    for (size_t i = 0; i < transients.size(); ++i) {
        for (size_t j = i + 1; j < transients.size(); ++j) {
            int lag = transients[j].index - transients[i].index;
            if (lag >= min_lag_chunks && lag <= max_lag_chunks) {
                float weight = transients[i].energy * transients[j].energy;
                energy_histogram[lag] += weight;
                // פיזור קל לריכוך חוסר-דיוק אנושי בנגינה
                if (lag > min_lag_chunks) energy_histogram[lag - 1] += weight * 0.5f;
                if (lag < max_lag_chunks) energy_histogram[lag + 1] += weight * 0.5f;
            }
        }
    }

    int best_lag_chunks = min_lag_chunks;
    float max_energy = -1.0f;
    for (int lag = min_lag_chunks; lag <= max_lag_chunks; ++lag) {
        if (energy_histogram[lag] > max_energy) {
            max_energy = energy_histogram[lag];
            best_lag_chunks = lag;
        }
    }

    float refined_beat_samples = static_cast<float>(best_lag_chunks) * chunk_size;
    float calculated_bpm = 60.0f / (refined_beat_samples / config_.sample_rate);
    estimated_bpm_.store(calculated_bpm, std::memory_order_relaxed);

    return refined_beat_samples;
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

void LooperEngine::process_audio_asynchronously() {
    std::vector<float> local_chunk;
    local_chunk.reserve(config_.chunk_size);
    size_t max_preroll_samples = static_cast<size_t>(config_.preroll_seconds * config_.sample_rate);
    preroll_buffer_.resize(max_preroll_samples, 0.0f);
    preroll_write_idx_ = 0;
    std::vector<float> recorded_audio;
    recorded_audio.reserve(config_.sample_rate * 60);
    float peak_recording_rms = 0.0f;
    size_t silence_samples_count = 0;
    float sample;
    float prev_volume = 0.0f;

    while (is_running_.load(std::memory_order_relaxed)) {

        if (request_clear_.exchange(false, std::memory_order_relaxed)) {
            recorded_audio.clear();
            playback_buffers_[0].clear();
            playback_buffers_[1].clear();
            active_playback_idx_.store(0, std::memory_order_release);
            playback_read_idx_.store(0, std::memory_order_relaxed);
            silence_samples_count = 0;
            peak_recording_rms = 0.0f;
            current_state_.store(LooperState::IDLE, std::memory_order_release);
            estimated_bpm_.store(0.0f, std::memory_order_relaxed);
            continue;
        }

        local_chunk.clear();
        while (local_chunk.size() < static_cast<size_t>(config_.chunk_size) && input_queue_.pop(sample)) {
            local_chunk.push_back(sample);
        }

        if (local_chunk.empty()) { std::this_thread::yield(); continue; }

        float volume = calculate_rms(local_chunk);
        current_rms_.store(volume, std::memory_order_relaxed);
        current_noise_std_dev_.store(noise_tracker_.get_std_dev(), std::memory_order_relaxed);

        if (volume > noise_tracker_.get_onset_threshold() && volume > prev_volume * 1.5f) {
            transient_hit_flag_.store(true, std::memory_order_relaxed);
        }

        prev_volume = volume;

        LooperState state = current_state_.load(std::memory_order_relaxed);

        switch (state) {
            case LooperState::CALIBRATING: {
                noise_tracker_.observe_background_noise(volume);
                if (noise_tracker_.is_ready()) {
                    current_state_.store(LooperState::IDLE, std::memory_order_release);
                }
                break;
            }

            case LooperState::IDLE: {
                for (float s : local_chunk) {
                    preroll_buffer_[preroll_write_idx_] = s;
                    preroll_write_idx_ = (preroll_write_idx_ + 1) % max_preroll_samples;
                }

                int mode = detection_mode_.load(std::memory_order_relaxed);

                // טריגר אוטומטי (למצבים 0 ו-2)
                bool auto_triggered = (mode != 1) && (volume > noise_tracker_.get_onset_threshold() && volume > ABSOLUTE_MIN_ONSET_RMS);
                // טריגר ידני מהנגן (למצב 1: Tap & Trim)
                bool manual_triggered = (mode == 1) && request_record_start_.exchange(false, std::memory_order_relaxed);

                if (auto_triggered || manual_triggered) {
                    recorded_audio.clear();
                    for (size_t i = 0; i < max_preroll_samples; ++i) {
                        size_t read_idx = (preroll_write_idx_ + i) % max_preroll_samples;
                        recorded_audio.push_back(preroll_buffer_[read_idx]);
                    }
                    peak_recording_rms = volume;
                    silence_samples_count = 0;
                    current_state_.store(LooperState::RECORDING, std::memory_order_release);
                } else {
                    noise_tracker_.observe_background_noise(volume);
                }
                break;
            }

            case LooperState::RECORDING: {
                recorded_audio.insert(recorded_audio.end(), local_chunk.begin(), local_chunk.end());
                if (volume > peak_recording_rms) peak_recording_rms = volume;

                int mode = detection_mode_.load(std::memory_order_relaxed);

                if (mode == 1) {
                    // מצב Tap & Trim: המכונה ממתינה להחלטה אנושית מפורשת כדי לעצור
                    if (request_record_stop_.exchange(false, std::memory_order_relaxed)) {
                        current_state_.store(LooperState::PROCESSING, std::memory_order_release);
                    }
                } else {
                    // מצב Auto Silence: המכונה מחליטה מתי לעצור על בסיס דעיכת האנרגיה
                    float dynamic_silence_threshold = std::max(noise_tracker_.get_silence_threshold(), peak_recording_rms * 0.04f);
                    size_t dynamic_silence_target = static_cast<size_t>(3.5f * config_.sample_rate);

                    if (volume < dynamic_silence_threshold) {
                        silence_samples_count += local_chunk.size();
                        if (silence_samples_count >= dynamic_silence_target) {
                            current_state_.store(LooperState::PROCESSING, std::memory_order_release);
                        }
                    } else {
                        if (volume > peak_recording_rms * 0.15f) {
                            if (silence_samples_count > 0) silence_samples_count = 0;
                        } else {
                            size_t penalty = local_chunk.size() * 3;
                            if (silence_samples_count > penalty) silence_samples_count -= penalty;
                            else silence_samples_count = 0;
                        }
                    }
                }
                break;
            }

            case LooperState::PROCESSING: {
                size_t preroll_samples = static_cast<size_t>(config_.preroll_seconds * config_.sample_rate);
                size_t onset_index = find_true_onset(recorded_audio, preroll_samples, std::max(noise_tracker_.get_onset_threshold(), ABSOLUTE_MIN_ONSET_RMS));

                if (onset_index > 0 && onset_index < recorded_audio.size()) {
                    recorded_audio.erase(recorded_audio.begin(), recorded_audio.begin() + onset_index);
                }

                if (recorded_audio.size() <= silence_samples_count) {
                    current_state_.store(LooperState::IDLE, std::memory_order_release);
                    break;
                }

                float beat_length = extract_beat_length_from_onsets(recorded_audio);
                float actual_playing_samples = static_cast<float>(recorded_audio.size() - silence_samples_count);
                float exact_beats = actual_playing_samples / beat_length;
                float target_musical_beats = quantize_to_musical_phrase(exact_beats);
                size_t ideal_length = static_cast<size_t>(target_musical_beats * beat_length);

                std::vector<float> final_loop;
                final_loop.reserve(ideal_length);
                for (size_t i = 0; i < ideal_length && i < recorded_audio.size(); ++i) {
                    final_loop.push_back(recorded_audio[i]);
                }

                if (final_loop.size() < ideal_length) {
                    final_loop.resize(ideal_length, 0.0f);
                } else if (recorded_audio.size() > ideal_length) {
                    size_t tail_length = recorded_audio.size() - ideal_length;
                    tail_length = std::min(tail_length, ideal_length / 2);
                    for (size_t i = 0; i < tail_length; ++i) {
                        final_loop[i] += recorded_audio[ideal_length + i];
                        float tail_fade = 1.0f - (static_cast<float>(i) / static_cast<float>(tail_length));
                        final_loop[i] = std::clamp(final_loop[i] * tail_fade, -1.0f, 1.0f);
                    }
                }

                final_loop = apply_zero_crossing_crossfade(final_loop, 256);

                if (!final_loop.empty()) {
                    int inactive_idx = 1 - active_playback_idx_.load(std::memory_order_relaxed);
                    playback_buffers_[inactive_idx] = std::move(final_loop);
                    playback_read_idx_.store(0, std::memory_order_relaxed);
                    active_playback_idx_.store(inactive_idx, std::memory_order_release);
                    current_state_.store(LooperState::LOOPING, std::memory_order_release);
                } else {
                    current_state_.store(LooperState::IDLE, std::memory_order_release);
                }
                break;
            }

            case LooperState::LOOPING: {
                if (request_overdub_.load(std::memory_order_relaxed)) {
                    current_state_.store(LooperState::OVERDUBBING, std::memory_order_release);
                    request_overdub_.store(false, std::memory_order_relaxed);
                }
                break;
            }

            case LooperState::OVERDUBBING: {
                int active_idx = active_playback_idx_.load(std::memory_order_acquire);
                auto& active_buffer = playback_buffers_[active_idx];
                size_t buffer_size = active_buffer.size();

                if (buffer_size > 0) {
                    size_t overdub_idx = playback_read_idx_.load(std::memory_order_relaxed);
                    for (float sample : local_chunk) {
                        active_buffer[overdub_idx] = mix_and_soft_clip(active_buffer[overdub_idx], sample);
                        overdub_idx++;
                        if (overdub_idx >= buffer_size) overdub_idx = 0;
                    }
                }

                if (request_looping_.load(std::memory_order_relaxed)) {
                    current_state_.store(LooperState::LOOPING, std::memory_order_release);
                    request_looping_.store(false, std::memory_order_relaxed);
                }
                break;
            }
        }
    }
}
