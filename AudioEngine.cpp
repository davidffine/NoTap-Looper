#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <chrono>

// הגדרות חומרה למניעת עיכובים חישוביים מתת-נורמלים (Denormals)
#include <xmmintrin.h>
#include <pmmintrin.h>

#include "miniaudio.h"

// עטיפת ה-Include כך שלא יכשיל קומפילציה על מכונות Windows/Linux סטנדרטיות
#ifdef __ANDROID__
#include "oboe/include/oboe/Oboe.h"
#endif

// ==========================================
// 1. תשתית זיכרון נטולת-נעילות לקלט
// ==========================================
template <typename T>
class LockFreeRingBuffer {
private:
    std::vector<T> buffer_;
    const size_t capacity_;
    alignas(64) std::atomic<size_t> write_index_{0};
    alignas(64) std::atomic<size_t> read_index_{0};

public:
    explicit LockFreeRingBuffer(size_t capacity) : capacity_(capacity + 1) {
        buffer_.resize(capacity_);
    }

    bool push(const T& item) {
        const size_t current_write = write_index_.load(std::memory_order_relaxed);
        const size_t next_write = (current_write + 1) % capacity_;
        if (next_write == read_index_.load(std::memory_order_acquire)) {
            return false;
        }
        buffer_[current_write] = item;
        write_index_.store(next_write, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t current_read = read_index_.load(std::memory_order_relaxed);
        if (current_read == write_index_.load(std::memory_order_acquire)) {
            return false;
        }
        item = buffer_[current_read];
        read_index_.store((current_read + 1) % capacity_, std::memory_order_release);
        return true;
    }
};

// ==========================================
// 2. הגדרות מערכת (State Machine & Config)
// ==========================================
enum class LooperState {
    CALIBRATING,
    IDLE,
    RECORDING,
    PROCESSING,
    LOOPING
};

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

// ==========================================
// מנוע הניתוח הסטטיסטי - האוזן האובייקטיבית
// ==========================================
class DynamicThresholdTracker {
private:
    std::vector<float> noise_history_;
    size_t history_capacity_;
    size_t write_idx_ = 0;
    bool is_buffer_full_ = false;

    float current_mean_ = 0.0f;
    float current_std_dev_ = 0.0f;

    const float SIGMA_MULTIPLIER_ONSET = 6.0f;  
    const float SIGMA_MULTIPLIER_SILENCE = 2.0f; 

public:
    DynamicThresholdTracker(int sample_rate, int chunk_size, float tracking_window_seconds = 1.0f) {
        history_capacity_ = static_cast<size_t>(sample_rate * tracking_window_seconds) / chunk_size; 
        if (history_capacity_ < 10) history_capacity_ = 10;
        noise_history_.resize(history_capacity_, 0.0f);
    }

    void observe_background_noise(float chunk_rms) {
        noise_history_[write_idx_] = chunk_rms;
        write_idx_++;

        if (write_idx_ >= history_capacity_) {
            write_idx_ = 0;
            is_buffer_full_ = true;
        }
        recalculate_statistics();
    }

    void recalculate_statistics() {
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

    bool is_ready() const {
        return is_buffer_full_ || write_idx_ > (history_capacity_ / 2);
    }

    float get_onset_threshold() const {
        return current_mean_ + (SIGMA_MULTIPLIER_ONSET * current_std_dev_);
    }

    float get_silence_threshold() const {
        return current_mean_ + (SIGMA_MULTIPLIER_SILENCE * current_std_dev_);
    }
    
    float get_mean() const { return current_mean_; }
};

// ==========================================
// 3. הליבה החישובית האבסולוטית (The Refined DSP Engine)
// ==========================================
class LooperEngine {
private:
    EngineConfig config_;
    LockFreeRingBuffer<float> input_queue_;
    
    std::atomic<LooperState> current_state_{LooperState::CALIBRATING};
    std::atomic<bool> is_running_{true};
    std::thread worker_thread_;

    // החלפנו את משתני הסף המוחלטים במעקב דינמי
    DynamicThresholdTracker noise_tracker_;

    std::vector<float> playback_buffers_[2];
    alignas(64) std::atomic<int> active_playback_idx_{0};
    alignas(64) std::atomic<size_t> playback_read_idx_{0};

    std::vector<float> preroll_buffer_;
    size_t preroll_write_idx_{0};

    void export_to_wav(const std::vector<float>& audio_buffer, const std::string& filename) {
        if (audio_buffer.empty()) return;
        ma_encoder_config encoderConfig = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 1, config_.sample_rate);
        ma_encoder encoder;
        if (ma_encoder_init_file(filename.c_str(), &encoderConfig, &encoder) != MA_SUCCESS) return;
        ma_uint64 framesWritten = 0;
        ma_encoder_write_pcm_frames(&encoder, audio_buffer.data(), audio_buffer.size(), &framesWritten);
        ma_encoder_uninit(&encoder);
    }

    float calculate_rms(const std::vector<float>& chunk) {
        if (chunk.empty()) return 0.0f;
        float sum_squares = std::inner_product(chunk.begin(), chunk.end(), chunk.begin(), 0.0f);
        return std::sqrt(sum_squares / chunk.size());
    }

    size_t find_true_onset(const std::vector<float>& audio_data, size_t max_search_samples, float threshold) {
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

    std::vector<float> extract_novelty_curve(const std::vector<float>& audio_data, int env_sr, int& out_chunk_size) {
        out_chunk_size = config_.sample_rate / env_sr;
        std::vector<float> env;
        env.reserve(audio_data.size() / out_chunk_size + 1);
        
        for (size_t i = 0; i < audio_data.size(); i += out_chunk_size) {
            float sum = 0.0f;
            size_t count = 0;
            for (size_t j = i; j < i + out_chunk_size && j < audio_data.size(); ++j) {
                sum += audio_data[j] * audio_data[j]; 
                count++;
            }
            env.push_back(std::sqrt(sum / count)); 
        }

        std::vector<float> novelty(env.size(), 0.0f);
        for (size_t i = 1; i < env.size(); ++i) {
            float diff = env[i] - env[i - 1];
            novelty[i] = (diff > 0.0f) ? diff : 0.0f; 
        }
        return novelty;
    }

    float extract_beat_length_from_onsets(const std::vector<float>& audio_data) {
        std::cout << "[DSP] Running Advanced Energy-Weighted Transient Histogram Analysis..." << std::endl;
        
        if (audio_data.size() < static_cast<size_t>(config_.sample_rate)) {
            return (60.0f / 120.0f) * config_.sample_rate; 
        }

        int chunk_size = 0;
        const int env_sr = 400; 
        std::vector<float> novelty = extract_novelty_curve(audio_data, env_sr, chunk_size);

        float mean_nov = std::accumulate(novelty.begin(), novelty.end(), 0.0f) / novelty.size();
        float peak_threshold = mean_nov * 2.0f; // הורדנו מעט את הסף כדי לתפוס גם פריטות חלשות, שכן האנרגיה תסנן אותן בהמשך

        struct TransientEvent {
            int index;
            float energy;
        };
        std::vector<TransientEvent> transients;
        
        // 1. איתור פיקים וצימוד האנרגיה הדינמית שלהם
        for (size_t i = 1; i < novelty.size() - 1; ++i) {
            if (novelty[i] > peak_threshold && novelty[i] > novelty[i - 1] && novelty[i] > novelty[i + 1]) {
                // Debouncing של 40ms כדי לאפשר חלוקות מהירות אך למנוע כפילויות פיזיקליות
                if (transients.empty() || (static_cast<int>(i) - transients.back().index) > (env_sr * 0.04f)) {
                    transients.push_back({static_cast<int>(i), novelty[i]});
                }
            }
        }

        if (transients.size() < 2) {
            std::cout << "[WARN] Insufficient transients for matrix analysis. Defaulting to 120 BPM." << std::endl;
            return (60.0f / 120.0f) * config_.sample_rate;
        }

        // הגדרת גבולות פיזיקליים אנושיים אבסולוטיים (במונחי דגימות מעטפת)
        int min_lag_chunks = static_cast<int>((60.0f / 240.0f) * env_sr); // 240 BPM
        int max_lag_chunks = static_cast<int>((60.0f / 45.0f) * env_sr);  // 45 BPM

        // 2. בניית היסטוגרמה משוקללת (Autocorrelation-like Histogram matrix)
        // המטרה: למצוא איזה מרווח זמן (Lag) חוזר על עצמו עם האנרגיה הגבוהה ביותר
        std::vector<float> energy_histogram(max_lag_chunks + 1, 0.0f);

        for (size_t i = 0; i < transients.size(); ++i) {
            for (size_t j = i + 1; j < transients.size(); ++j) {
                int lag = transients[j].index - transients[i].index;
                
                if (lag >= min_lag_chunks && lag <= max_lag_chunks) {
                    // המשקל הגיאומטרי של המרווח נקבע לפי מכפלת האנרגיות של שני הפיקים
                    float weight = transients[i].energy * transients[j].energy;
                    
                    // הזרקת האנרגיה לתוך ה-Lag הנוכחי (עם חלון ריכוך קל מסביבו למניעת סטיות קטנות בזמן)
                    energy_histogram[lag] += weight;
                    if (lag > min_lag_chunks) energy_histogram[lag - 1] += weight * 0.5f;
                    if (lag < max_lag_chunks) energy_histogram[lag + 1] += weight * 0.5f;
                }
            }
        }

        // 3. מציאת ה-Lag הדומיננטי ביותר (ההר הגבוה ביותר בהיסטוגרמה)
        int best_lag_chunks = min_lag_chunks;
        float max_energy = -1.0f;
        
        for (int lag = min_lag_chunks; lag <= max_lag_chunks; ++lag) {
            if (energy_histogram[lag] > max_energy) {
                max_energy = energy_histogram[lag];
                best_lag_chunks = lag;
            }
        }

        // 4. המרה חזרה לרזולוציית האודיו המקורית (Audio Samples)
        float refined_beat_samples = static_cast<float>(best_lag_chunks) * chunk_size;

        std::cout << "[MATH] Energy Grid Lock complete. Dominant Interval: " 
                  << (refined_beat_samples / config_.sample_rate) * 1000.0f 
                  << " ms (Estimated Tempo: " << 60.0f / (refined_beat_samples / config_.sample_rate) << " BPM)" << std::endl;

        return refined_beat_samples;
    }

    float quantize_to_musical_phrase(float raw_beats) {
        float nearest_int = std::round(raw_beats);
        if (nearest_int < 2.0f) return 2.0f; 
        
        float candidates[] = {
            std::round(raw_beats / 4.0f) * 4.0f,
            std::round(raw_beats / 2.0f) * 2.0f,
            nearest_int                          
        };
        
        float best_match = nearest_int;
        float min_penalty = 1e9f;
        
        for (float candidate : candidates) {
            if (candidate < 2.0f) continue;
            
            float deviation = std::abs(raw_beats - candidate);
            float penalty = deviation;
            
            if (std::fmod(candidate, 4.0f) == 0.0f) penalty *= 0.5f; 
            else if (std::fmod(candidate, 2.0f) == 0.0f) penalty *= 0.8f; 
            else penalty *= 1.5f; 
            
            if (penalty < min_penalty) {
                min_penalty = penalty;
                best_match = candidate;
            }
        }
        return best_match;
    }

    std::vector<float> apply_zero_crossing_crossfade(std::vector<float>& audio, size_t crossfade_samples = 256) {
        if (audio.size() <= crossfade_samples * 2) return audio;
        
        size_t len = audio.size();
        std::vector<float> result = audio;
        
        size_t best_end = len - 1;
        for (size_t i = len - 1; i > len - crossfade_samples; --i) {
            if (audio[i] * audio[i-1] <= 0.0f) {
                best_end = i;
                break;
            }
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

    void process_audio_asynchronously() {
        std::vector<float> local_chunk;
        local_chunk.reserve(config_.chunk_size);
        
        size_t max_preroll_samples = static_cast<size_t>(config_.preroll_seconds * config_.sample_rate);
        preroll_buffer_.resize(max_preroll_samples, 0.0f);
        preroll_write_idx_ = 0;
        
        std::vector<float> recorded_audio;
        recorded_audio.reserve(config_.sample_rate * 60); 
        
        float peak_recording_rms = 0.0f;
        size_t silence_samples_count = 0;
        size_t silence_target_samples = static_cast<size_t>(config_.silence_duration * config_.sample_rate);
        float sample;

        std::cout << "[STATE] Engine Boot: Entering CALIBRATING phase." << std::endl;

        while (is_running_.load(std::memory_order_relaxed)) {
            local_chunk.clear();
            while (local_chunk.size() < static_cast<size_t>(config_.chunk_size) && input_queue_.pop(sample)) {
                local_chunk.push_back(sample);
            }

            if (local_chunk.empty()) { std::this_thread::yield(); continue; }

            float volume = calculate_rms(local_chunk);
            LooperState state = current_state_.load(std::memory_order_relaxed);

            switch (state) {
                case LooperState::CALIBRATING: {
                    // בניית תשתית הידע (A priori) של החדר
                    noise_tracker_.observe_background_noise(volume);
                    
                    if (noise_tracker_.is_ready()) {
                        std::cout << "[CALIB] Statistical baseline acquired. Base Noise (Mean): " 
                                  << noise_tracker_.get_mean() << std::endl;
                        current_state_.store(LooperState::IDLE, std::memory_order_release);
                    }
                    break;
                }

                case LooperState::IDLE: {
                    for (float s : local_chunk) {
                        preroll_buffer_[preroll_write_idx_] = s;
                        preroll_write_idx_ = (preroll_write_idx_ + 1) % max_preroll_samples;
                    }

                    // סף הפריטה מעודכן דינמית מתוך המודל הסטטיסטי
                    if (volume > noise_tracker_.get_onset_threshold()) {
                        recorded_audio.clear();
                        for (size_t i = 0; i < max_preroll_samples; ++i) {
                            size_t read_idx = (preroll_write_idx_ + i) % max_preroll_samples;
                            recorded_audio.push_back(preroll_buffer_[read_idx]);
                        }
                        peak_recording_rms = volume;
                        silence_samples_count = 0;
                        current_state_.store(LooperState::RECORDING, std::memory_order_release);
                    } else {
                        // כל עוד אין אירוע אקוסטי, המערכת ממשיכה ללמוד ולעדכן את ההבנה שלה על הסביבה
                        noise_tracker_.observe_background_noise(volume);
                    }
                    break;
                }

                case LooperState::RECORDING: {
                    recorded_audio.insert(recorded_audio.end(), local_chunk.begin(), local_chunk.end());
                    
                    if (volume > peak_recording_rms) peak_recording_rms = volume;

                    float dynamic_silence_threshold = std::max(noise_tracker_.get_silence_threshold(), peak_recording_rms * 0.04f);

                    // סבלנות דינמית: המתן 2.5 שניות של שקט מוחלט כדי לוודא סיום, ללא קשר ל-BPM
                    size_t dynamic_silence_target = static_cast<size_t>(2.5f * config_.sample_rate);

                    if (volume < dynamic_silence_threshold) {
                        silence_samples_count += local_chunk.size();
                        if (silence_samples_count >= dynamic_silence_target) {
                            // שים לב: אנחנו חותכים מההקלטה את זמן ההמתנה כדי שהמנוע הקוונטי לא ינתח אותו
                            recorded_audio.erase(recorded_audio.end() - silence_samples_count, recorded_audio.end());
                            current_state_.store(LooperState::PROCESSING, std::memory_order_release);
                        }
                    } else {
                        if (volume > peak_recording_rms * 0.15f) { // הורדנו את רף "שבירת השתיקה" ל-15% כדי לתפוס תווים חלשים בסוף ביט
                            if (silence_samples_count > 0) silence_samples_count = 0; 
                        } else {
                            size_t penalty = local_chunk.size() * 3;
                            if (silence_samples_count > penalty) {
                                silence_samples_count -= penalty;
                            } else {
                                silence_samples_count = 0;
                            }
                        }
                    }
                    break;
                }

                case LooperState::PROCESSING: {
                    std::cout << "\n--- [MATH] INITIATING DISCRETE EVENT QUANTIZATION ---" << std::endl;
                    
                    size_t preroll_samples = static_cast<size_t>(config_.preroll_seconds * config_.sample_rate);
                    // שימוש בסף הדינמי שהוגדר לחיתוך רעשים עודפים
                    size_t onset_index = find_true_onset(recorded_audio, preroll_samples, noise_tracker_.get_onset_threshold());
                    
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
                        export_to_wav(final_loop, "final_loop_diagnostic.wav");
                        
                        int inactive_idx = 1 - active_playback_idx_.load(std::memory_order_relaxed);
                        playback_buffers_[inactive_idx] = std::move(final_loop);
                        playback_read_idx_.store(0, std::memory_order_relaxed);
                        active_playback_idx_.store(inactive_idx, std::memory_order_release);
                        
                        std::cout << "[STATE] Architecture Complete. Transitioning to LOOPING.\n" << std::endl;
                        current_state_.store(LooperState::LOOPING, std::memory_order_release);
                    } else {
                        current_state_.store(LooperState::IDLE, std::memory_order_release);
                    }
                    break;
                }

                case LooperState::LOOPING: { break; }
            }
        }
    }

public:
    LooperEngine(EngineConfig config = EngineConfig{}) : 
        config_(config), 
        input_queue_(config.sample_rate * 5),
        noise_tracker_(config.sample_rate, config.chunk_size, 1.0f) // בניית האוזן עם יכולת שמיעה לשנייה אחורה
    {
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
        worker_thread_ = std::thread(&LooperEngine::process_audio_asynchronously, this);
    }

    ~LooperEngine() {
        is_running_.store(false, std::memory_order_relaxed);
        if (worker_thread_.joinable()) worker_thread_.join();
    }

    void update_hardware_latency(float latency_seconds) {
        // נוסיף מרווח ביטחון קל של 20 מילישניות לחישוב של מערכת ההפעלה
        config_.preroll_seconds = latency_seconds + 0.02f;
        size_t new_max_samples = static_cast<size_t>(config_.preroll_seconds * config_.sample_rate);
        preroll_buffer_.resize(new_max_samples, 0.0f);
        std::cout << "[DSP] Dynamic Preroll calibrated to: " << config_.preroll_seconds << "s based on Hardware." << std::endl;
    }

    void on_audio_callback(const float* input_data, size_t num_frames) {
        for (size_t i = 0; i < num_frames; ++i) input_queue_.push(input_data[i]);
    }

    void process_output_callback(float* output_data, size_t num_frames) {
        if (current_state_.load(std::memory_order_relaxed) != LooperState::LOOPING) {
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
    
    LooperState get_current_state() const { return current_state_.load(std::memory_order_relaxed); }
};

// ==========================================
// 4. תשתית הקרנל לאנדרואיד (מבודדת לחלוטין)
// ==========================================
#ifdef __ANDROID__
class OboeLooperEngine : public oboe::AudioStreamCallback {
private:
    std::shared_ptr<oboe::AudioStream> audio_stream_;
    LooperEngine& dsp_engine_; 

public:
    OboeLooperEngine(LooperEngine& dsp_engine) : dsp_engine_(dsp_engine) {}

    bool start() {
        oboe::AudioStreamBuilder builder;

        builder.setDirection(oboe::Direction::Input); 
        builder.setFormat(oboe::AudioFormat::Float);
        builder.setChannelCount(oboe::ChannelCount::Mono);

        builder.setInputPreset(oboe::InputPreset::VoiceCommunication);

        builder.setSharingMode(oboe::SharingMode::Exclusive);
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
        builder.setAudioApi(oboe::AudioApi::AAudio);
        builder.setCallback(this);

        oboe::Result result = builder.openStream(audio_stream_);
        if (result != oboe::Result::OK) {
            std::cerr << "[Hardware Error] Failed to open stream: " << oboe::convertToText(result) << std::endl;
            return false;
        }

        // עכשיו, כשהזרם פתוח, אפשר לשאול את החומרה למאפייניה האמיתיים
        int optimal_buffer_size = audio_stream_->getFramesPerBurst() * 2;
        audio_stream_->setBufferSizeInFrames(optimal_buffer_size);

        // עדכון אובייקטיבי של ה-Latency למנוע ה-DSP הראשי
        dsp_engine_.update_hardware_latency(static_cast<float>(optimal_buffer_size) / 44100.0f);

        result = audio_stream_->requestStart();
        return result == oboe::Result::OK;
    }

    void stop() {
        if (audio_stream_) {
            audio_stream_->requestStop();
            audio_stream_->close();
        }
    }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audioStream, 
                                          void *audioData, 
                                          int32_t numFrames) override {
        float* floatData = static_cast<float*>(audioData);
        dsp_engine_.on_audio_callback(floatData, numFrames);
        return oboe::DataCallbackResult::Continue;
    }
};
#endif

// ==========================================
// 5. גשר הבדיקות (Main Engine)
// ==========================================
void audio_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    LooperEngine* engine = static_cast<LooperEngine*>(pDevice->pUserData);
    if (engine == nullptr) return;
    
    if (pInput != nullptr && engine->get_current_state() != LooperState::LOOPING) {
        engine->on_audio_callback(static_cast<const float*>(pInput), frameCount);
    }
    
    if (pOutput != nullptr) {
        engine->process_output_callback(static_cast<float*>(pOutput), frameCount);
    }
}

int main() {
    std::cout << "[System] Initializing Raw Acoustic DSP Architecture..." << std::endl;
    LooperEngine engine;
    
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_duplex);
    deviceConfig.performanceProfile = ma_performance_profile_low_latency;
    deviceConfig.periodSizeInFrames = 256;
    deviceConfig.capture.format    = ma_format_f32;
    deviceConfig.capture.channels  = 1;
    deviceConfig.playback.format   = ma_format_f32;
    deviceConfig.playback.channels = 1; 
    deviceConfig.sampleRate        = 44100;
    deviceConfig.dataCallback      = audio_data_callback;
    deviceConfig.pUserData         = &engine;
    
    ma_device device;
    if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS) {
        std::cerr << "[Error] Failed to initialize physical duplex device." << std::endl;
        return -1;
    }
    
    ma_device_start(&device);
    std::cout << "[System] Hardware linked. Microphones & Speakers are HOT." << std::endl;
    std::cout << "[System] Awaiting input. Press Enter to terminate process." << std::endl;
    
    std::cin.get();
    ma_device_uninit(&device);
    
    return 0;
}