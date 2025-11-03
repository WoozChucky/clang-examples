#pragma once
#include <atomic>
#include <cstdint>

template <typename T>
class Seqlock {
public:
    void store(const T& v) {
        // single writer
        uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_release);   // mark write begin (odd)
        data_ = v;                                      // publish payload
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);   // mark write end (even)
    }

    T load() const {
        for (;;) {
            uint64_t s0 = seq_.load(std::memory_order_acquire);
            if (s0 & 1) continue; // writer in progress
            T v = data_;          // optimistic read
            std::atomic_thread_fence(std::memory_order_acquire);
            uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s0 == s1) return v; // consistent
        }
    }

private:
    mutable std::atomic<uint64_t> seq_{0};
    T data_{};
};