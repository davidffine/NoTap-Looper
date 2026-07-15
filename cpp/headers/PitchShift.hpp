#pragma once

// ============================================================================
// אוקטבה *שומרת-אורך* לשכבת לופ (Phase-3 רב-מסלול).
//
// אפקטי האוקטבה הקיימים (OctaveResample.hpp) הם ריסמפול טהור: הם משנים גם
// גובה וגם *אורך* (up→N/2, down→2N). בלופ רב-מסלול כל השכבות חייבות אותו אורך
// (loop_length_), ולכן צריך העתקת-גובה שאינה נוגעת באורך.
//
// השיטה = מתיחת-זמן (Phase Vocoder) ואז ריסמפול, כך שהאורך חוזר למקור:
//   up  (גובה ×2):  מתיחת-זמן ×2 (→2N, אותו גובה) ואז octave_up (דצימציה ×2 →N, גובה ×2)
//   down(גובה ×0.5): מתיחת-זמן ×0.5 (→N/2, אותו גובה) ואז octave_down (אינטרפולציה ×2 →N)
// שלב הריסמפול ×2 מסופק ע"י המסננים חצי-הסרט המעגליים הקיימים (איכות גבוהה,
// Anti-Alias, שומרי-תפר). ה-Phase Vocoder כאן מבצע *רק* את מתיחת-הזמן ×2/×0.5.
//
// ה-STFT מעגלי (הלופ הוא מעגל) — פריימי הניתוח נקראים במחזוריות, וה-OLA
// כותב במחזוריות לחוצץ באורך היעד, כך שהתפר של המקור נשמר.
//
// אזהרה: Phase Vocoder מרחיב טרנזיינטים ועלול להישמע "פאזי" בתוכן קשה. נמדד
// אופליין (tools/pitchcheck) על-גובה/אורך/תפר; לאיכות ‎audible חובה בדיקת מכשיר.
// ============================================================================

#include <cmath>
#include <cstddef>
#include <vector>

#include "OctaveResample.hpp"
#include "../includes/kissfft/kiss_fftr.h"

namespace notap_dsp {

namespace detail {

inline float princ_arg(float phase) {
    // עיטוף ל-(-π, π]
    const float TWO_PI = 6.283185307179586f;
    return phase - TWO_PI * std::round(phase / TWO_PI);
}

// מתיחת-זמן Phase-Vocoder של לופ מעגלי לאורך יעד out_len (שומר גובה).
// W = גודל FFT, Ha = צעד ניתוח. Hs = out_len/in_len · Ha (צעד סינתזה).
// out_len חופשי (מעוגל לדגימה); כאן נשתמש ב-2N ו-N/2 בלבד.
inline std::vector<float> pv_time_stretch(const std::vector<float>& x, size_t out_len) {
    const size_t N = x.size();
    if (N < 64 || out_len < 64) return {};

    const int W = 1024;
    const int Ha = W / 4;                       // 75% חפיפה
    const float ratio = static_cast<float>(out_len) / static_cast<float>(N);
    const float Hs_f = ratio * static_cast<float>(Ha);
    const float TWO_PI = 6.283185307179586f;

    // חלון Hann מנורמל ל-OLA (מחולק אחר-כך בסכום-החלונות המצטבר).
    std::vector<float> win(W);
    for (int i = 0; i < W; ++i) win[i] = 0.5f * (1.0f - std::cos(TWO_PI * i / (W - 1)));

    const int NB = W / 2 + 1;
    kiss_fftr_cfg fwd = kiss_fftr_alloc(W, 0, nullptr, nullptr);
    kiss_fftr_cfg inv = kiss_fftr_alloc(W, 1, nullptr, nullptr);
    std::vector<kiss_fft_scalar> tin(W), tout(W);
    std::vector<kiss_fft_cpx>    spec(NB);

    std::vector<float> prev_phase(NB, 0.0f), sum_phase(NB, 0.0f);
    std::vector<float> out(out_len, 0.0f), norm(out_len, 0.0f);

    // מס' פריימים לכיסוי מחזורי מלא של הלופ בצעד הניתוח.
    const size_t F = (N + Ha - 1) / Ha;
    for (size_t f = 0; f < F; ++f) {
        const size_t a_pos = (f * Ha) % N;
        for (int i = 0; i < W; ++i) tin[i] = x[(a_pos + i) % N] * win[i];
        kiss_fftr(fwd, tin.data(), spec.data());

        const long s_pos = static_cast<long>(std::llround(f * Hs_f)) % static_cast<long>(out_len);
        for (int k = 0; k < NB; ++k) {
            float re = spec[k].r, im = spec[k].i;
            float mag = std::sqrt(re * re + im * im);
            float phase = std::atan2(im, re);
            float omega = TWO_PI * k / W;                      // תדר הבין (rad/דגימה)
            if (f == 0) {
                sum_phase[k] = phase;
            } else {
                float dphi = princ_arg(phase - prev_phase[k] - omega * Ha);
                float true_freq = omega + dphi / Ha;           // תדר אמיתי (rad/דגימה)
                sum_phase[k] = princ_arg(sum_phase[k] + Hs_f * true_freq);
            }
            prev_phase[k] = phase;
            spec[k].r = mag * std::cos(sum_phase[k]);
            spec[k].i = mag * std::sin(sum_phase[k]);
        }

        kiss_fftri(inv, spec.data(), tout.data());
        // OLA מעגלי + צבירת ריבוע-החלון לנרמול (kissfft inverse אינו מנורמל → /W)
        const float inv_w = 1.0f / static_cast<float>(W);
        for (int i = 0; i < W; ++i) {
            size_t idx = (static_cast<size_t>(s_pos) + i) % out_len;
            out[idx]  += tout[i] * inv_w * win[i];
            norm[idx] += win[i] * win[i];
        }
    }

    kiss_fftr_free(fwd);
    kiss_fftr_free(inv);

    for (size_t i = 0; i < out_len; ++i)
        if (norm[i] > 1e-8f) out[i] /= norm[i];
    return out;
}

} // namespace detail

// אוקטבה שומרת-אורך על לופ מעגלי. up=true → +אוקטבה, up=false → -אוקטבה.
// אורך הפלט == אורך הקלט (האינוריאנטה של המנוע הרב-מסלולי).
inline std::vector<float> octave_pitch(const std::vector<float>& src, bool up) {
    const size_t N = src.size();
    if (N < 512) return {};   // מתחת לזה גם הריסמפול חצי-הסרט לא אמין
    if (up) {
        // מתיחת-זמן ×2 (→2N, אותו גובה) ואז דצימציה ×2 (→N, גובה ×2)
        std::vector<float> stretched = detail::pv_time_stretch(src, 2 * N);
        if (stretched.empty()) return {};
        return octave_up(stretched);          // 2N → N
    } else {
        // מתיחת-זמן ×0.5 (→N/2, אותו גובה) ואז אינטרפולציה ×2 (→N, גובה ×0.5)
        std::vector<float> stretched = detail::pv_time_stretch(src, N / 2);
        if (stretched.empty()) return {};
        return octave_down(stretched);        // N/2 → N
    }
}

} // namespace notap_dsp
