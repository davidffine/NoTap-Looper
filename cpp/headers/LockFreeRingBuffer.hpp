#pragma once
#include <vector>
#include <atomic>
#include <cstddef>

template <typename T>
class LockFreeRingBuffer {
private:
    // עיגול הקיבולת לחזקה-של-2 מאפשר עטיפת אינדקס ב-AND בודד (mask) במקום
    // חלוקת-שארית (modulo) יקרה. capacity_ הוא ערך-ריצה (5s·sr) שאינו חזקה-של-2,
    // ולכן הקומפיילר אינו יכול לצמצם את ה-% ל-shift — נפלט UDIV לכל push/pop על
    // נתיב ה-RT. AND בודד שקול-מתמטית ((x+1)&(2ⁿ-1) == (x+1) mod 2ⁿ) ומבטל אותו.
    static size_t round_up_pow2(size_t n) {
        size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    std::vector<T> buffer_;
    const size_t capacity_;   // חזקה-של-2
    const size_t mask_;       // capacity_ - 1
    alignas(64) std::atomic<size_t> write_index_{0};
    alignas(64) std::atomic<size_t> read_index_{0};

public:
    // עדיין "מבזבזים" סלוט אחד להבחנת ריק/מלא (next==read ⇒ מלא), ולכן מעגלים את
    // capacity+1. usable = capacity_-1 ≥ capacity המבוקש (העיגול רק מגדיל).
    explicit LockFreeRingBuffer(size_t capacity)
        : capacity_(round_up_pow2(capacity + 1)), mask_(capacity_ - 1) {
        buffer_.resize(capacity_);
    }

    bool push(const T& item) {
        const size_t current_write = write_index_.load(std::memory_order_relaxed);
        const size_t next_write = (current_write + 1) & mask_;
        if (next_write == read_index_.load(std::memory_order_acquire)) return false;
        buffer_[current_write] = item;
        write_index_.store(next_write, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t current_read = read_index_.load(std::memory_order_relaxed);
        if (current_read == write_index_.load(std::memory_order_acquire)) return false;
        item = buffer_[current_read];
        read_index_.store((current_read + 1) & mask_, std::memory_order_release);
        return true;
    }
};
