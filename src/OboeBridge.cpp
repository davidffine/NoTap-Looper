#include "OboeBridge.hpp"
#include <iostream>

#ifdef __ANDROID__

OboeLooperEngine::OboeLooperEngine(LooperEngine& dsp_engine) : dsp_engine_(dsp_engine) {}

OboeLooperEngine::~OboeLooperEngine() {
    stop();
}

bool OboeLooperEngine::start() {
    // ----------------------------------------------------
    // 1. פתיחת זרם המיקרופון (INPUT) - אקוסטיקה טהורה
    // ----------------------------------------------------
    oboe::AudioStreamBuilder in_builder;
    in_builder.setDirection(oboe::Direction::Input);
    in_builder.setFormat(oboe::AudioFormat::Float);
    in_builder.setChannelCount(oboe::ChannelCount::Mono);
    // חשוב: Unprocessed מבטל את מסנני הרעש של אנדרואיד שלא יהרסו את צליל הגיטרה
    in_builder.setInputPreset(oboe::InputPreset::Unprocessed);
    in_builder.setSharingMode(oboe::SharingMode::Exclusive);
    in_builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    in_builder.setCallback(this);

    oboe::Result in_result = in_builder.openStream(recording_stream_);
    if (in_result != oboe::Result::OK) {
        std::cerr << "[Hardware Error] Failed to open INPUT stream." << std::endl;
        return false;
    }

    // ----------------------------------------------------
    // 2. פתיחת זרם הרמקול (OUTPUT)
    // ----------------------------------------------------
    oboe::AudioStreamBuilder out_builder;
    out_builder.setDirection(oboe::Direction::Output);
    out_builder.setFormat(oboe::AudioFormat::Float);
    out_builder.setChannelCount(oboe::ChannelCount::Mono);
    out_builder.setSharingMode(oboe::SharingMode::Exclusive);
    out_builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    out_builder.setCallback(this);
    // קריטי: הרמקול חייב לעבוד בדיוק באותו קצב דגימה כמו המיקרופון
    out_builder.setSampleRate(recording_stream_->getSampleRate());

    oboe::Result out_result = out_builder.openStream(playback_stream_);
    if (out_result != oboe::Result::OK) {
        std::cerr << "[Hardware Error] Failed to open OUTPUT stream." << std::endl;
        return false;
    }

    // ----------------------------------------------------
    // 3. כיול הזיכרון והתנעת החומרה
    // ----------------------------------------------------
    int optimal_buffer_size = recording_stream_->getFramesPerBurst() * 2;
    recording_stream_->setBufferSizeInFrames(optimal_buffer_size);
    playback_stream_->setBufferSizeInFrames(playback_stream_->getFramesPerBurst() * 2);

    // עדכון ה-DSP על הלייטנסי האמיתי של המערכת
    dsp_engine_.update_hardware_latency(static_cast<float>(optimal_buffer_size) / static_cast<float>(recording_stream_->getSampleRate()));

    // פתיחת הסכרים
    recording_stream_->requestStart();
    playback_stream_->requestStart();

    return true;
}

void OboeLooperEngine::stop() {
    if (recording_stream_) {
        recording_stream_->requestStop();
        recording_stream_->close();
        recording_stream_.reset();
    }
    if (playback_stream_) {
        playback_stream_->requestStop();
        playback_stream_->close();
        playback_stream_.reset();
    }
}

// Oboe קורא לפונקציה הזו פעמיים: פעם כשיש סאונד מהמיקרופון, ופעם כשהרמקול רעב לסאונד
oboe::DataCallbackResult OboeLooperEngine::onAudioReady(oboe::AudioStream *audioStream,
                                                        void *audioData,
                                                        int32_t numFrames) {
    float* floatData = static_cast<float*>(audioData);

    // ניתוב דטרמיניסטי לפי כיוון הזרם
    if (audioStream->getDirection() == oboe::Direction::Input) {
        // דחיפת המידע מהמיקרופון אל תור הזיכרון של המנוע
        dsp_engine_.on_audio_callback(floatData, numFrames);
    } else {
        // משיכת הסאונד המחושב מהמנוע אל הרמקול
        dsp_engine_.process_output_callback(floatData, numFrames);
    }

    return oboe::DataCallbackResult::Continue;
}

#endif