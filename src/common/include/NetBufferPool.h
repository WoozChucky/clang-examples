#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "MpscRing.h"

// Fixed-block payload pool with lock-free acquire/release. The free list is an
// MPMC ring of block indices. Adapter threads Acquire (copying received bytes in),
// GameThread Releases after PollEvent consumes the block. Acquire returns nullptr
// when momentarily exhausted (caller logs + drops/backpressures — never silently).
//
// blockCount is rounded UP to a power of two (ring requirement) and clamped to
// kMaxBlocks; the extra blocks are simply available capacity.
class NetBufferPool {
public:
    NetBufferPool(size_t blockSize, size_t blockCount)
        : m_BlockSize(blockSize), m_Count(RoundUpPow2(blockCount)) {
        if (m_Count > kMaxBlocks) m_Count = kMaxBlocks;
        m_Storage.resize(m_BlockSize * m_Count);
        m_Free = std::make_unique<FreeRing>();
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_Count); ++i)
            m_Free->Enqueue(i);   // all blocks initially free
    }

    std::byte* Acquire(uint32_t& outIndex) {
        uint32_t idx;
        if (!m_Free->Dequeue(idx)) return nullptr;
        // observability only; off the free-list path. Record a TRUE high-water at
        // acquire time so the peak survives the (often near-instant) Release — a
        // read-time sample of InUse misses short-lived blocks (e.g. send buffers
        // freed on the IOCP worker microseconds later).
        const size_t now = m_InUse.fetch_add(1, std::memory_order_relaxed) + 1;
        size_t peak = m_PeakInUse.load(std::memory_order_relaxed);
        while (now > peak && !m_PeakInUse.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {}
        outIndex = idx;
        return Block(idx);
    }

    // A free-list must NEVER lose a block. The underlying ring (MPMC Vyukov) can
    // transiently report "full" even though it isn't: when a concurrent Acquire
    // (Dequeue) is preempted between reserving its slot and publishing it free, an
    // Enqueue that wraps to that cell sees it unfreed and returns false. The ring's
    // capacity (kMaxBlocks) far exceeds the live block count, so it is never
    // *permanently* full — spin until the Enqueue takes (the preempted consumer
    // resumes and frees the cell). Without this retry, Release would drop the index
    // and slowly leak blocks under contention (observed ~5-8% in stress tests).
    void Release(uint32_t index) {
        while (!m_Free->Enqueue(index)) { std::this_thread::yield(); }
        m_InUse.fetch_sub(1, std::memory_order_relaxed);   // observability only; index is already back on the free-list
    }

    std::byte* Block(uint32_t index) { return m_Storage.data() + static_cast<size_t>(index) * m_BlockSize; }

    // Index of a block pointer previously returned by Acquire/Block. UB if `block`
    // is not a block start from this pool.
    uint32_t IndexOf(const std::byte* block) const {
        return static_cast<uint32_t>(
            (block - m_Storage.data()) / static_cast<std::ptrdiff_t>(m_BlockSize));
    }

    size_t BlockSize() const { return m_BlockSize; }
    size_t BlockCount() const { return m_Count; }
    // Approximate live-block count (relaxed; for stats/observability, not synchronization).
    size_t InUse() const { return m_InUse.load(std::memory_order_relaxed); }
    // True high-water mark of InUse since construction (recorded at Acquire, so it
    // survives Release). Relaxed; observability only.
    size_t PeakInUse() const { return m_PeakInUse.load(std::memory_order_relaxed); }

private:
    static size_t RoundUpPow2(size_t n) {
        if (n < 1) return 1;
        size_t p = 1; while (p < n) p <<= 1; return p;
    }
    static constexpr size_t kMaxBlocks = 4096;
    using FreeRing = MpscRing<uint32_t, kMaxBlocks>;

    size_t                  m_BlockSize;
    size_t                  m_Count;
    std::vector<std::byte>  m_Storage;
    std::unique_ptr<FreeRing> m_Free;   // heap (large; avoid bloating the pool object)
    std::atomic<size_t>     m_InUse{0};
    std::atomic<size_t>     m_PeakInUse{0};
};
