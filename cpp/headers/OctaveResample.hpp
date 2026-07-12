#pragma once

// ============================================================================
// ריסמפול אוקטבה מדויק (יחס 2 בדיוק) עם סינון חצי-סרט מעגלי.
//
// הבעיה בגרסה הקודמת (אינטרפולציה לינארית, בלי סינון):
//   ‎oct-up: דצימציה ×2 ללא Anti-Alias — כל התוכן מעל Fs/4 מתקפל חזרה
//   כרעש לא-הרמוני. ‎oct-down: אינטרפולציה לינארית = גרעין משולש — דימויים
//   (images) סביב Fs/2 מוחלשים ב-~12dB בלבד בתדרים גבוהים.
//
// הפתרון: ביחס 2 בדיוק אין צורך באינטרפולציה כלל —
//   ‎oct-up  = סינון LP(Fs/4) ואז לקיחת כל דגימה שנייה (פוליפאזה מנוונת).
//   ‎oct-down = שתילת אפסים + LP(Fs/4)×2 — האינטרפולטור חצי-הסרט הקלאסי:
//              הדגימות הזוגיות עוברות כמות-שהן, האי-זוגיות מסונתזות מהטאפים
//              האי-זוגיים בלבד.
//
// הקונבולוציה *מעגלית*: הלופ הוא מעגל, וסינון לינארי היה יוצר קצוות
// (Warm-up של המסנן) — כלומר פגם תפר חדש. סינון מעגלי משמר את רציפות
// התפר של המקור בדיוק, ולכן הטרנספורמציה אינה זקוקה לשום טיפול-תפר נוסף.
//
// גרעין: חצי-סרט 63 טאפים בחלון Blackman — מקדמים זוגיים אפס מלבד המרכז
// (0.5), ולכן העלות בפועל ~16 MAC לדגימת פלט. Stopband ≥ ~70dB.
// ============================================================================

#include <cmath>
#include <cstddef>
#include <vector>

namespace notap_dsp {

inline const std::vector<float>& halfband_odd_taps() {
    // מקדמי הטאפים האי-זוגיים h[1], h[3], ... h[31] של חצי-סרט 63 טאפים.
    // h[n] = sinc(n/2)/2 · Blackman(n, 63); הזוגיים = 0, המרכז = 0.5.
    static const std::vector<float> taps = [] {
        constexpr int HALF = 31;                 // n = -31..31
        constexpr double PI = 3.14159265358979323846;
        std::vector<float> t;
        for (int n = 1; n <= HALF; n += 2) {
            double sinc = std::sin(PI * n / 2.0) / (PI * n);   // sinc(n/2)/2 בפועל
            double x = (n + HALF) / (2.0 * HALF);              // 0..1 על פני החלון
            double w = 0.42 - 0.5 * std::cos(2.0 * PI * x) + 0.08 * std::cos(4.0 * PI * x);
            t.push_back(static_cast<float>(sinc * w));
        }
        return t;
    }();
    return taps;
}

// אוקטבה למעלה: LP חצי-סרט מעגלי + דצימציה ×2. אורך הפלט = N/2 (רצפת חלוקה).
inline std::vector<float> octave_up(const std::vector<float>& src) {
    const size_t N = src.size();
    if (N < 256) return {};
    const auto& h = halfband_odd_taps();
    std::vector<float> dst(N / 2);
    for (size_t i = 0; i < dst.size(); ++i) {
        const size_t c = 2 * i;
        double acc = 0.5 * src[c];               // הטאפ המרכזי
        for (size_t k = 0; k < h.size(); ++k) {
            const size_t n = 2 * k + 1;          // אינדקסי הטאפים האי-זוגיים
            acc += h[k] * (static_cast<double>(src[(c + n) % N]) + src[(c + N - n) % N]);
        }
        dst[i] = static_cast<float>(acc);
    }
    return dst;
}

// אוקטבה למטה: אינטרפולטור חצי-סרט מעגלי (שתילת אפסים + LP×2 בפוליפאזה).
// אורך הפלט = 2N בדיוק; הדגימות הזוגיות = המקור ללא שינוי.
inline std::vector<float> octave_down(const std::vector<float>& src) {
    const size_t N = src.size();
    if (N < 128) return {};
    const auto& h = halfband_odd_taps();
    std::vector<float> dst(2 * N);
    for (size_t i = 0; i < N; ++i) {
        dst[2 * i] = src[i];                     // מסלול המרכז: 2·h[0] = 1
        double acc = 0.0;
        for (size_t k = 0; k < h.size(); ++k) {
            // y[2i+1] = 2·Σ h[2k+1]·(x[i-k] + x[i+1+k]) — סימטרי סביב המרכז
            acc += h[k] * (static_cast<double>(src[(i + N - k) % N]) + src[(i + 1 + k) % N]);
        }
        dst[2 * i + 1] = static_cast<float>(2.0 * acc);
    }
    return dst;
}

} // namespace notap_dsp
