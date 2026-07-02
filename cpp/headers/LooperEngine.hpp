#pragma once

#include "LockFreeRingBuffer.hpp"
#include <vector>
#include <string>
#include <atomic>
#include <thread>

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

inline float mix_and_soft_clip(float existing_sample, float new_sample) {
    float sum = existing_sample + new_sample;
    return sum / (1.0f + std::abs(sum));
}

class DynamicThresholdTracker {
private:
    std::vector<float> noise_history_;
    size_t history_capacity_;
    size_t write_idx_ = 0;
    bool is_buffer_full_ = false;
    float current_mean_ = 0.0f;
    float current_std_dev_ = 0.0f;

    const float SIGMA_MULTIPLIER_ONSET = 8.5f;
    const float SIGMA_MULTIPLIER_SILENCE = 2.0f;

    std::atomic<float> target_bpm_{120.0f};

public:
    DynamicThresholdTracker(int sample_rate, int chunk_size, float tracking_window_seconds = 1.0f);
    void observe_background_noise(float chunk_rms);
    void recalculate_statistics();
    bool is_ready() const;
    float get_onset_threshold() const;
    float get_silence_threshold() const;
    float get_mean() const;
    float get_std_dev() const { return current_std_dev_; }
    void set_target_bpm(float bpm) { target_bpm_.store(bpm, std::memory_order_relaxed); }
};

class LooperEngine {
private:
    EngineConfig config_;
    LockFreeRingBuffer<float> input_queue_;

    std::atomic<LooperState> current_state_{LooperState::CALIBRATING};
    std::atomic<bool> is_running_{true};
    std::thread worker_thread_;
    std::atomic<float> target_bpm_{120.0f};

    DynamicThresholdTracker noise_tracker_;

    std::vector<float> playback_buffers_[2];
    alignas(64) std::atomic<int> active_playback_idx_{0};
    alignas(64) std::atomic<size_t> playback_read_idx_{0};

    std::vector<float> preroll_buffer_;
    size_t preroll_write_idx_{0};

    std::atomic<bool> request_record_start_{false};
    std::atomic<bool> request_record_stop_{false};
    std::atomic<bool> request_overdub_{false};
    std::atomic<bool> request_looping_{false};
    std::atomic<bool> request_clear_{false};

    std::atomic<float> estimated_bpm_{0.0f};

    // [חדש] שמירת אונטולוגיית הזיהוי (0=Auto, 1=Tap, 2=BPM)
    std::atomic<int> detection_mode_{0};

    const float ABSOLUTE_MIN_ONSET_RMS = 0.015f;

    float calculate_rms(const std::vector<float>& chunk);
    size_t find_true_onset(const std::vector<float>& audio_data, size_t max_search_samples, float threshold);
    std::vector<float> extract_novelty_curve(const std::vector<float>& audio_data, int env_sr, int& out_chunk_size);
    float extract_beat_length_from_onsets(const std::vector<float>& audio_data);
    float quantize_to_musical_phrase(float raw_beats);
    std::vector<float> apply_zero_crossing_crossfade(std::vector<float>& audio, size_t crossfade_samples = 256);
    void process_audio_asynchronously();

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

    float get_estimated_bpm() const;
    float get_loop_position() const;

    bool export_to_wav(const char* filepath);
    bool import_from_wav(const char* filepath);

    void update_hardware_latency(float latency_seconds);

    void on_audio_callback(const float* input_data, size_t num_frames);
    void process_output_callback(float* output_data, size_t num_frames);
    LooperState get_current_state() const;

    void set_target_bpm(float bpm) {
        target_bpm_.store(bpm, std::memory_order_relaxed);
    }
};