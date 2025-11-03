#pragma once

#include <vector>

#include "lib.h"

// Simple SPSC ring buffer (fixed capacity), lock-free producer/consumer.
// Single producer, single consumer.
template<typename T, size_t N>
class SpscRing {
public:
    SpscRing() {
        SM_ASSERT((N & (N - 1)) == 0, "N must be power of two for mask");
        head.store(0);
        tail.store(0);
        data.resize(N);
    }

    // Return true if pushed, false if full
    bool Push(const T& v) {
        uint64_t h = head.load(std::memory_order_relaxed);
        uint64_t t = tail.load(std::memory_order_acquire); // consumer progress
        if ((h - t) >= N) return false; // full
        data[h & mask] = v;
        head.store(h + 1, std::memory_order_release);
        return true;
    }

    // Return true if popped into out, false if empty
    bool Pop(T& out) {
        uint64_t t = tail.load(std::memory_order_relaxed);
        uint64_t h = head.load(std::memory_order_acquire); // producer progress
        if (t == h) return false; // empty
        out = data[t & mask];
        tail.store(t + 1, std::memory_order_release);
        return true;
    }

    // Check emptiness without popping (not essential)
    bool Empty() const {
        return tail.load(std::memory_order_acquire) == head.load(std::memory_order_acquire);
    }

private:
    static constexpr uint64_t mask = N - 1;
    std::vector<T> data;
    std::atomic<uint64_t> head{}; // producer index
    std::atomic<uint64_t> tail{}; // consumer index
};