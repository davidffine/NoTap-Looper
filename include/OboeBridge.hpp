#pragma once

#include "LooperEngine.hpp"
#include <memory>

#ifdef __ANDROID__
#include <oboe/Oboe.h>

class OboeLooperEngine : public oboe::AudioStreamCallback {
private:
    // עכשיו יש לנו שני זרמים נפרדים: אוזניים ופה
    std::shared_ptr<oboe::AudioStream> recording_stream_;
    std::shared_ptr<oboe::AudioStream> playback_stream_;
    LooperEngine& dsp_engine_;

public:
    explicit OboeLooperEngine(LooperEngine& dsp_engine);
    ~OboeLooperEngine();

    bool start();
    void stop();

    // הפונקציה הזו תשרת את שני הזרמים בו-זמנית ותנתב את המידע
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audioStream,
                                          void *audioData,
                                          int32_t numFrames) override;
};
#endif