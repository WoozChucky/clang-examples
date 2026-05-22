#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "Engine.h"

struct StagingPoolStats {
    size_t   Free;            // blocks currently on the free-list
    size_t   InUse;           // blocks handed out and not yet returned
    size_t   Created;         // blocks ever malloc'd
    uint64_t Reuses;          // Acquire calls satisfied from the free-list
    size_t   ReservedBytes;   // total capacity of all blocks the pool owns (free + in use)
    size_t   FreeBytes;       // total capacity sitting on the free-list
};

// Best-fit, grow-to-fit free-list of variable-size staging buffers. Acquire on the
// GameThread / ModelWorker, Return on the RenderThread / GameThread retry — so every
// method is mutex-guarded. The workload is bursty (model load time), so a mutex is the
// right tool (mirrors the ECS SnapshotPool decision).
class StagingBufferPool {
public:
    static constexpr size_t kHeaderSize = 16; // >= sizeof(size_t); keeps user ptr 16-aligned

    void* Acquire(size_t size) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        // Best-fit: the smallest free block that fits, so a giant block is not consumed
        // by a tiny request.
        size_t best = SIZE_MAX;
        size_t bestIdx = SIZE_MAX;
        for (size_t i = 0; i < m_Free.size(); ++i) {
            if (m_Free[i].Capacity >= size && m_Free[i].Capacity < best) {
                best = m_Free[i].Capacity;
                bestIdx = i;
            }
        }
        if (bestIdx != SIZE_MAX) {
            FreeBlock b = m_Free[bestIdx];
            m_Free[bestIdx] = m_Free.back();
            m_Free.pop_back();
            m_FreeBytes -= b.Capacity;
            ++m_InUse;
            ++m_Reuses;
            return b.UserPtr;
        }
        auto* block = static_cast<char*>(std::malloc(kHeaderSize + size));
        *reinterpret_cast<size_t*>(block) = size;        // stamp capacity into the header
        void* userPtr = block + kHeaderSize;
        m_ReservedBytes += size;
        ++m_Created;
        ++m_InUse;
        return userPtr;
    }

    void Return(void* userPtr) {
        if (!userPtr) return;
        auto* block = static_cast<char*>(userPtr) - kHeaderSize;
        const size_t capacity = *reinterpret_cast<size_t*>(block);
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Free.push_back(FreeBlock{ userPtr, capacity });
        m_FreeBytes += capacity;
        --m_InUse;
    }

    StagingPoolStats Stats() const {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return StagingPoolStats{ m_Free.size(), m_InUse, m_Created,
                                 m_Reuses, m_ReservedBytes, m_FreeBytes };
    }

    ~StagingBufferPool() {
        // Free only the free-list. Unlike the ECS SnapshotPool we deliberately do NOT
        // assert InUse==0: at teardown the GameThread's pending m_CompletedJobs may still
        // hold acquired buffers. Those leak at process exit — benign, identical to the
        // prior std::malloc behaviour; the pool does not own them and cannot free them.
        for (const FreeBlock& b : m_Free)
            std::free(static_cast<char*>(b.UserPtr) - kHeaderSize);
    }

private:
    struct FreeBlock { void* UserPtr; size_t Capacity; };
    mutable std::mutex     m_Mutex;
    std::vector<FreeBlock> m_Free;
    size_t   m_InUse         = 0;
    size_t   m_Created       = 0;
    uint64_t m_Reuses        = 0;
    size_t   m_ReservedBytes = 0;
    size_t   m_FreeBytes     = 0;
};

// Process-wide staging pool. DEFINED in StagingBufferPool.cpp and exported from Engine.dll
// so there is exactly ONE instance across the Engine.dll / editor.exe boundary. An inline
// header definition gives each module its OWN function-local static, which left the editor's
// Memory panel (editor.exe) reading an always-empty copy while the upload code (Engine.dll)
// filled a different one. Tests instantiate their own local StagingBufferPool instead.
ENGINE_API StagingBufferPool& GetStagingPool();
ENGINE_API StagingPoolStats GetStagingPoolStats();
