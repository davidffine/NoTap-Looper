#pragma once
#include <atomic>
#include <vector>
#include <cstddef>

template <typename T>
class LockFreeRingBuffer {
private:
    std::vector<T> buffer_;
    const size_t capacity_;

    // alignas(64) כופה על המשתנים לשבת ב-Cache Lines נפרדים.
    // זה מונע מצב שבו פתיל הקריאה ופתיל הכתיבה מרוקנים אחד לשני 
    // את זיכרון המטמון (Cache Invalidation) כשהם מעדכנים אינדקסים.
    alignas(64) std::atomic<size_t> write_index_{0};
    alignas(64) std::atomic<size_t> read_index_{0};

public:
    // האובייקט תמיד מקצה מקום אחד יותר מהנדרש כדי להבדיל בין מצב "מלא" למצב "ריק"
    explicit LockFreeRingBuffer(size_t capacity) : capacity_(capacity + 1) {
        buffer_.resize(capacity_);
    }

    // מיועד לשימוש *אך ורק* על ידי פתיל האודיו (Producer)
    bool push(const T& item) {
        const size_t current_write = write_index_.load(std::memory_order_relaxed);
        const size_t next_write = (current_write + 1) % capacity_;

        // memory_order_acquire מבטיח שכל קריאה מהזיכרון תתבצע רק אחרי
        // שווידאנו את הסטטוס העדכני ביותר של פתיל הקריאה.
        if (next_write == read_index_.load(std::memory_order_acquire)) {
            return false; // החוצץ מלא (Buffer Overrun)
        }

        buffer_[current_write] = item;
        
        // memory_order_release מבטיח שהמידע נכתב פיזית לזיכרון 
        // לפני שאנחנו מעדכנים את האינדקס כדי שהצרכן יראה אותו.
        write_index_.store(next_write, std::memory_order_release);
        return true;
    }

    // מיועד לשימוש *אך ורק* על ידי פתיל העיבוד/WSOLA (Consumer)
    bool pop(T& item) {
        const size_t current_read = read_index_.load(std::memory_order_relaxed);

        if (current_read == write_index_.load(std::memory_order_acquire)) {
            return false; // החוצץ ריק (Buffer Underrun)
        }

        item = buffer_[current_read];
        read_index_.store((current_read + 1) % capacity_, std::memory_order_release);
        return true;
    }

    // פונקציית עזר למשיכת בלוקים שלמים (Chunks) כדי לייעל את עיבוד ה-DSP
    size_t pop_block(T* dest, size_t count) {
        size_t items_read = 0;
        for (size_t i = 0; i < count; ++i) {
            if (!pop(dest[i])) {
                break;
            }
            items_read++;
        }
        return items_read;
    }
    
    size_t get_capacity() const {
        return capacity_ - 1;
    }
};