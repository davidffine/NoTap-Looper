// שמירת מצב האזהרות הנוכחי של המהדר
#pragma GCC diagnostic push

// כיבוי האזהרה הספציפית שיוצרת את אזעקת השווא מול פעולות אטומיות
#pragma GCC diagnostic ignored "-Wstringop-overflow"

// יישום הספרייה
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

// שחזור מצב האזהרות הרגיל כדי להגן על שאר המערכת
#pragma GCC diagnostic pop