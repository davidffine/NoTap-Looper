#include "OboeBridge.hpp"
#include <iostream>

#ifdef __ANDROID__

OboeLooperEngine::OboeLooperEngine(LooperEngine& dsp_engine) : dsp_engine_(dsp_engine) {}

OboeLooperEngine::~OboeLooperEngine() {
    stop();
}

bool OboeLooperEngine::start() {
    // Idempotent: a second start() (e.g. a redundant onStart) must not open a
    // duplicate pair of streams. Already running → nothing to do.
    if (recording_stream_ || playback_stream_) return true;

    // ----------------------------------------------------
    // 1. פתיחת זרם המיקרופון (INPUT) - אקוסטיקה טהורה
    // ----------------------------------------------------
    oboe::AudioStreamBuilder in_builder;
    in_builder.setDirection(oboe::Direction::Input);
    in_builder.setFormat(oboe::AudioFormat::Float);
    in_builder.setChannelCount(oboe::ChannelCount::Mono);
    // חשוב: Unprocessed מבטל את מסנני הרעש של אנדרואיד שלא יהרסו את צליל הגיטרה
    in_builder.setInputPreset(oboe::InputPreset::Unprocessed);
    // Shared (לא Exclusive): זרם MMAP בלעדי תופס פורט חומרה ייעודי; סגירתו —
    // בעיקר במוות אלים (Swipe-kill) — מאלצת את AudioFlinger לפרק את המסלול ולנתב
    // מחדש, מה שגורם ל-A2DP של המוזיקה לקפוץ לרמקול הפנימי ובחזרה. Shared עובר
    // דרך המיקסר; אין פורט בלעדי לתפוס/לשחרר, ולכן אין "פאניקת ניתוב".
    in_builder.setSharingMode(oboe::SharingMode::Shared);
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
    out_builder.setSharingMode(oboe::SharingMode::Shared);   // ראה הערת ה-INPUT — מונע תפיסת פורט בלעדי
    out_builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    // כוונת הניתוב מפורשת: זהו אודיו מדיה/מוזיקה — משתלב נקי עם נגני מדיה אחרים
    // (Spotify) במקום להיחשב מסלול תקשורת/התראה שמאלץ ניתוב מיוחד.
    out_builder.setUsage(oboe::Usage::Media);
    out_builder.setContentType(oboe::ContentType::Music);
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

    // עדכון ה-DSP על קצב הדגימה האמיתי של החומרה ועל הלייטנסי שלה.
    // קריטי: Oboe פותח את המיקרופון בקצב הנייטיב של המכשיר (בדרך כלל 48000),
    // בעוד המנוע נבנה עם ברירת מחדל של 44100 — בלי העדכון הזה כל קבועי הזמן
    // (Preroll, סף השקט, חישוב ה-BPM) סוטים בכ-9% בכל מכשיר מודרני.
    int hardware_sample_rate = recording_stream_->getSampleRate();
    dsp_engine_.update_hardware_config(hardware_sample_rate,
        static_cast<float>(optimal_buffer_size) / static_cast<float>(hardware_sample_rate));

    // פתיחת הסכרים
    recording_stream_->requestStart();
    playback_stream_->requestStart();

    // --- אומדן לייטנסי הלוך-ושוב לפיצוי אוברדאב ---
    // הדגימה שנקלטת "עכשיו" נוגנה כנגד מה שהמשתמש שמע לפני (פלט+קלט); המנוע
    // מעגן את כתיבת האוברדאב אחורה בהתאם. מקור מועדף: timestamps של המערכת
    // (calculateLatencyMillis) — מיד אחרי start הם לרוב עוד לא זורמים, ואז נסוגים
    // לאומדן דטרמיניסטי: סכום חוצצי-התוכנה של שני הזרמים + מרווח HAL/המרה שמרני
    // (10ms — תת-אומדן מכוון: פיצוי-חסר משאיר איחור קטן; פיצוי-יתר היה מקדים את
    // השכבה, שגיאה בולטת יותר פסיכו-אקוסטית).
    {
        double total_ms;
        auto in_lat  = recording_stream_->calculateLatencyMillis();
        auto out_lat = playback_stream_->calculateLatencyMillis();
        if (in_lat && out_lat && in_lat.value() > 0.0 && out_lat.value() > 0.0) {
            total_ms = in_lat.value() + out_lat.value();
        } else {
            int frames = recording_stream_->getBufferSizeInFrames() +
                         playback_stream_->getBufferSizeInFrames();
            total_ms = 1000.0 * frames / hardware_sample_rate + 10.0;
        }
        int comp_samples = static_cast<int>(total_ms * hardware_sample_rate / 1000.0);
        dsp_engine_.set_overdub_latency_samples(comp_samples);
        std::cout << "[Hardware] Overdub latency compensation: " << total_ms
                  << "ms (" << comp_samples << " samples)" << std::endl;
    }

    return true;
}

// פירוק חינני *סינכרוני*: requestStop() ו-close() של Oboe חוסמים עד להשלמה,
// ולכן כשהפונקציה חוזרת ה-HAL כבר שוחרר נקי. נקרא מ-onStop() — "חלון הזהב"
// שמובטח לפני Swipe-kill. סדר: עוצרים את *שני* הזרמים (שום פורט לא פעיל), ורק
// אז סוגרים — הפלט קודם (מפסיק לרנדר), ואז הקלט (משחרר את מסלול הלכידה אחרון).
void OboeLooperEngine::stop() {
    if (playback_stream_)  playback_stream_->requestStop();
    if (recording_stream_) recording_stream_->requestStop();

    if (playback_stream_) {
        playback_stream_->close();
        playback_stream_.reset();
    }
    if (recording_stream_) {
        recording_stream_->close();
        recording_stream_.reset();
    }
}

// Oboe קורא לפונקציה הזו פעמיים: פעם כשיש סאונד מהמיקרופון, ופעם כשהרמקול רעב לסאונד
oboe::DataCallbackResult OboeLooperEngine::onAudioReady(oboe::AudioStream *audioStream,
                                                        void *audioData,
                                                        int32_t numFrames) {
    // FTZ הוא מצב רגיסטר פר-Thread; Threads של Oboe נוצרים על-ידי המערכת ואין
    // Hook לתחילתם — מציתים פעם אחת פר-Thread מכאן. בלעדיו זנב הריברב הדועך
    // (רשת משוב ×0.84) מייצר דנורמלים על Thread הפלט — סכנת נחירת CPU בזמן-אמת.
    static thread_local bool ftz_armed = false;
    if (!ftz_armed) { enable_denormal_flush_to_zero(); ftz_armed = true; }

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