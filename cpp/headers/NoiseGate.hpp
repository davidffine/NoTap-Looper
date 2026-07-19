#pragma once

// ============================================================================
// שער ספקטרלי מעגלי לניקוי-רעש של שכבת לופ ("CLEAN", פייז ניקוי — Pro).
//
// הבעיה: כל אוברדאב מקליט את רעש-החדר מחדש — הספק הרעש נערם עם כל שכבה
// (5 שכבות ≈ ‎+7dB היס). מודלים נוירוניים (RNNoise וכו') מאומנים על *דיבור*
// ואוכלים אקורד מוחזק/זנב-ריברב כ"רעש" — הכלי הלא-נכון למוזיקה.
//
// השיטה (Spectral Gating, נוסח Audacity/noisereduce, בטוח-למוזיקה):
//   Pass 1: STFT מעגלי; לכל bin תדר — היסטוגרמת log-הספק על פני כל הפריימים.
//           רצפת-הרעש של ה-bin = אחוזון 20 (רעש-רקע נוכח בכל פריים; מוזיקה
//           דלילה במישור זמן-תדר). ללא הקלטת-ייחוס וללא אחסון O(F·NB).
//   הגנת-מוזיקה: דרון/בס שמחזיק bin לכל אורך הלופ היה מרמה את האחוזון —
//           רצפת ה-bin נחסמת ב-×CAP מהחציון המקומי בתדר (רעש-חדר רחב-סרט ≈
//           חלק ספקטרלית; פסגה בודדת = מוזיקה, לא רעש).
//   Pass 2: הנחתת-הספק g=√max(1-β·n/p, g_floor²) עם רצפה (-14dB — לא ∞;
//           מחיקה מוחלטת נשמעת "מתחת למים"), החלקת-תדר (3 bins, נגד רעש
//           מוזיקלי) והחלקת-זמן א-סימטרית (התקפה מיידית — טרנזיינט עובר נקי;
//           שחרור ~100ms — בלי שאיבה). פאזה מקורית (סטנדרט חיסור ספקטרלי).
//   ‎OLA מעגלי (הלופ הוא מעגל) ⇒ רציפות-תפר מתמטית, אורך פלט == קלט בדיוק.
//
// נמדד אופליין ב-tools/noisecheck (עומק ניקוי, שימור טון, תפר) — לא מנחשים.
// עלות: ‎~2×FFT על הלופ (שני מעברים) — עשרות ms לשכבה, על ה-Worker בלבד.
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../includes/kissfft/kiss_fftr.h"

namespace notap_dsp {

inline std::vector<float> denoise_loop(const std::vector<float>& x) {
    const int W = 2048, HOP = 512;          // חלון 43ms@48k, חפיפה 75%
    const int NB = W / 2 + 1;
    const size_t N = x.size();
    if (N < static_cast<size_t>(8 * HOP)) return x;   // קצר מכדי לפרופיל רעש

    // --- פרמטרים (כוילו מול tools/noisecheck) ---
    // כוילו אמפירית מול tools/noisecheck: P20 של רעש אקספוננציאלי ≈ 0.22×הממוצע,
    // ולכן BETA חייב להיות גבוה — סף הפתיחה הוא BETA×0.22×mean, וכל פריים-רעש
    // שחוצה אותו מרצד את השער (ב-BETA=8 ⇒ ‎~17% מהפריימים; ב-12 ⇒ ‎~7%).
    const float BETA      = 12.0f;  // עוצמת-חיסור על רצפת P20
    const float G_FLOOR   = 0.15f;  // הנחתה מרבית ‎-16.5dB — שער, לא מחיקה
    const float RELEASE   = 0.55f;  // דעיכת gain פר-פריים (~30ms שחרור; איטי יותר
                                    // החזיק את השער פתוח על ריצודי-רעש — נמדד)
    const float CAP_MULT  = 6.0f;   // תקרת רצפת-bin מול החציון המקומי (הגנת דרון)
    const int   CAP_HALF  = 12;     // חצי-חלון החציון המקומי בתדר
    const float P_TILE    = 0.20f;  // האחוזון של רצפת הרעש

    const float TWO_PI = 6.283185307179586f;
    std::vector<float> win(W);
    for (int i = 0; i < W; ++i) win[i] = 0.5f * (1.0f - std::cos(TWO_PI * i / (W - 1)));

    kiss_fftr_cfg fwd = kiss_fftr_alloc(W, 0, nullptr, nullptr);
    kiss_fftr_cfg inv = kiss_fftr_alloc(W, 1, nullptr, nullptr);
    std::vector<kiss_fft_scalar> tin(W), tout(W);
    std::vector<kiss_fft_cpx>    spec(NB);

    const size_t F = (N + HOP - 1) / HOP;   // כיסוי מחזורי מלא של הלופ

    // --- Pass 1: היסטוגרמת log-הספק פר-bin (64KB במקום O(F·NB) מגה-בייטים) ---
    constexpr int   HB      = 34;
    constexpr float DB_LO   = -80.0f;
    constexpr float DB_STEP = 4.0f;
    std::vector<uint16_t> hist(static_cast<size_t>(NB) * HB, 0);
    for (size_t f = 0; f < F; ++f) {
        const size_t pos = (f * HOP) % N;
        for (int i = 0; i < W; ++i) tin[i] = x[(pos + i) % N] * win[i];
        kiss_fftr(fwd, tin.data(), spec.data());
        for (int k = 0; k < NB; ++k) {
            float p = spec[k].r * spec[k].r + spec[k].i * spec[k].i;
            float dbp = 10.0f * std::log10(p + 1e-20f);
            int b = static_cast<int>((dbp - DB_LO) / DB_STEP);
            b = std::clamp(b, 0, HB - 1);
            hist[static_cast<size_t>(k) * HB + b]++;
        }
    }

    // רצפת-רעש פר-bin = הספק אחוזון P_TILE מתוך ההיסטוגרמה
    std::vector<float> noise(NB);
    const uint32_t target = static_cast<uint32_t>(P_TILE * F);
    for (int k = 0; k < NB; ++k) {
        uint32_t cum = 0; int b = 0;
        for (; b < HB; ++b) {
            cum += hist[static_cast<size_t>(k) * HB + b];
            if (cum > target) break;
        }
        float db_center = DB_LO + (std::min(b, HB - 1) + 0.5f) * DB_STEP;
        noise[k] = std::pow(10.0f, db_center / 10.0f);
    }

    // הגנת-מוזיקה: bin שרצפתו חורגת בהרבה מהחציון המקומי מוחזק ע"י מוזיקה
    // מתמשכת (דרון/בס) — חוסמים את הרצפה כדי לא לחסר את התוכן עצמו.
    std::vector<float> capped(NB), med_scratch;
    for (int k = 0; k < NB; ++k) {
        int lo = std::max(0, k - CAP_HALF), hi = std::min(NB - 1, k + CAP_HALF);
        med_scratch.assign(noise.begin() + lo, noise.begin() + hi + 1);
        auto mid = med_scratch.begin() + med_scratch.size() / 2;
        std::nth_element(med_scratch.begin(), mid, med_scratch.end());
        capped[k] = std::min(noise[k], CAP_MULT * (*mid));
    }

    // --- Pass 2: הנחתה + החלקות + OLA מעגלי ---
    std::vector<float> out(N, 0.0f), norm(N, 0.0f);
    std::vector<float> g_raw(NB), g_smooth(NB), g_prev(NB, 1.0f);
    const float inv_w = 1.0f / static_cast<float>(W);
    for (size_t f = 0; f < F; ++f) {
        const size_t pos = (f * HOP) % N;
        for (int i = 0; i < W; ++i) tin[i] = x[(pos + i) % N] * win[i];
        kiss_fftr(fwd, tin.data(), spec.data());

        for (int k = 0; k < NB; ++k) {
            float p = spec[k].r * spec[k].r + spec[k].i * spec[k].i;
            float g2 = (p > 1e-20f) ? (p - BETA * capped[k]) / p : 0.0f;
            g_raw[k] = std::sqrt(std::max(g2, G_FLOOR * G_FLOOR));
        }
        // החלקת-תדר: ממוצע-נע 3 bins — מוחק ריצודי-bin בודדים (רעש מוזיקלי)
        g_smooth[0] = g_raw[0];
        g_smooth[NB - 1] = g_raw[NB - 1];
        for (int k = 1; k < NB - 1; ++k)
            g_smooth[k] = (g_raw[k - 1] + g_raw[k] + g_raw[k + 1]) * (1.0f / 3.0f);
        // החלקת-זמן א-סימטרית: עלייה מיידית (התקפה), ירידה בדעיכה (שחרור)
        for (int k = 0; k < NB; ++k) {
            float g = std::max(g_smooth[k], g_prev[k] * RELEASE);
            g_prev[k] = g;
            spec[k].r *= g;
            spec[k].i *= g;
        }

        kiss_fftri(inv, spec.data(), tout.data());
        for (int i = 0; i < W; ++i) {
            size_t j = (pos + i) % N;
            out[j]  += tout[i] * inv_w * win[i];
            norm[j] += win[i] * win[i];
        }
    }

    kiss_fftr_free(fwd);
    kiss_fftr_free(inv);
    for (size_t i = 0; i < N; ++i)
        if (norm[i] > 1e-8f) out[i] /= norm[i];
    return out;
}

} // namespace notap_dsp
