#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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
        outIndex = idx;
        return Block(idx);
    }

    void Release(uint32_t index) { m_Free->Enqueue(index); }

    std::byte* Block(uint32_t index) { return m_Storage.data() + static_cast<size_t>(index) * m_BlockSize; }

    size_t BlockSize() const { return m_BlockSize; }
    size_t BlockCount() const { return m_Count; }

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
};
