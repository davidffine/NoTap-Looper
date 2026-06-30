#pragma once
#include <vector>
#include <atomic>
#include <cstddef>

template <typename T>
class LockFreeRingBuffer {
private:
    std::vector<T> buffer_;
    const size_t capacity_;
    alignas(64) std::atomic<size_t> write_index_{0};
    alignas(64) std::atomic<size_t> read_index_{0};

public:
    explicit LockFreeRingBuffer(size_t capacity) : capacity_(capacity + 1) {
        buffer_.resize(capacity_);
    }

    bool push(const T& item) {
        const size_t current_write = write_index_.load(std::memory_order_relaxed);
        const size_t next_write = (current_write + 1) % capacity_;
        if (next_write == read_index_.load(std::memory_order_acquire)) return false;
        buffer_[current_write] = item;
        write_index_.store(next_write, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t current_read = read_index_.load(std::memory_order_relaxed);
        if (current_read == write_index_.load(std::memory_order_acquire)) return false;
        item = buffer_[current_read];
        read_index_.store((current_read + 1) % capacity_, std::memory_order_release);
        return true;
    }
};