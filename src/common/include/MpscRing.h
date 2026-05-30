#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// Bounded lock-free queue (Dmitry Vyukov's MPMC algorithm). Safe for multiple
// producers AND multiple consumers; used here MPSC (many adapter threads enqueue,
// GameThread dequeues) and as a free-list inside NetBufferPool. Capacity N must be
// a power of two; the ring holds up to N elements. No allocation per op. T must be
// trivially copyable (POD).
template <typename T, size_t N>
class MpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "MpscRing<T>: T must be trivially copyable (it is copied by plain assignment under concurrency)");
    static constexpr size_t kLine = 64;   // cache-line (avoid false sharing)

    struct alignas(kLine) Cell {
        std::atomic<size_t> seq;
        T                   data;
    };

public:
    MpscRing() {
        for (size_t i = 0; i < N; ++i)
            m_Cells[i].seq.store(i, std::memory_order_relaxed);
        m_Enq.store(0, std::memory_order_relaxed);
        m_Deq.store(0, std::memory_order_relaxed);
    }
    MpscRing(const MpscRing&) = delete;
    MpscRing& operator=(const MpscRing&) = delete;

    bool Enqueue(const T& v) {
        Cell* cell;
        size_t pos = m_Enq.load(std::memory_order_relaxed);
        for (;;) {
            cell = &m_Cells[pos & (N - 1)];
            const size_t seq = cell->seq.load(std::memory_order_acquire);
            const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (dif == 0) {
                if (m_Enq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (dif < 0) {
                return false;   // full
            } else {
                pos = m_Enq.load(std::memory_order_relaxed);
            }
        }
        cell->data = v;
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool Dequeue(T& out) {
        Cell* cell;
        size_t pos = m_Deq.load(std::memory_order_relaxed);
        for (;;) {
            cell = &m_Cells[pos & (N - 1)];
            const size_t seq = cell->seq.load(std::memory_order_acquire);
            const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (dif == 0) {
                if (m_Deq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (dif < 0) {
                return false;   // empty
            } else {
                pos = m_Deq.load(std::memory_order_relaxed);
            }
        }
        out = cell->data;
        cell->seq.store(pos + N, std::memory_order_release);
        return true;
    }

private:
    alignas(kLine) Cell m_Cells[N];
    alignas(kLine) std::atomic<size_t> m_Enq;
    alignas(kLine) std::atomic<size_t> m_Deq;
};
