#pragma once
#include <cstdint>
#include <malloc.h>              // _aligned_malloc / _aligned_free (Windows)
#include <vector>
#include <lib.h>                 // SM_ERROR, SM_ASSERT
#include <memory/IAllocator.h>
#include <memory/MemUtil.h>      // AlignUp

namespace Engine {

// Fixed-block free-list allocator. All blocks are the same size, so there is no
// fragmentation. Grows by adding a new slab when full; slabs are never moved or
// freed until destruction, so every outstanding pointer stays valid. O(1)
// alloc/free. Not thread-safe.
// Deallocate is unchecked: double-free, freeing a foreign pointer, or freeing a
// pointer obtained before a Reset() is undefined behavior and will corrupt the
// stats and the free-list. Caller owns correctness.
class PoolAllocator : public IAllocator {
public:
    PoolAllocator(size_t blockSize, size_t blockCount, size_t blockAlign,
                  MemCategory cat, const char* name)
        : m_BlockCount(blockCount), m_Category(cat), m_Name(name)
    {
        SM_ASSERT(blockSize >= sizeof(void*),
                  "PoolAllocator '%s': blockSize must be >= sizeof(void*)", name);
        SM_ASSERT(blockCount > 0, "PoolAllocator '%s': blockCount must be > 0", name);
        m_BlockAlign = blockAlign < alignof(void*) ? alignof(void*) : blockAlign;
        const size_t minBlock = blockSize > sizeof(void*) ? blockSize : sizeof(void*);
        m_Stride = AlignUp(minBlock, m_BlockAlign);
        AddSlab(); // initial slab
    }

    ~PoolAllocator() override {
        for (void* slab : m_Slabs) _aligned_free(slab);
    }

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    void* Allocate(size_t size, size_t align) override {
        if (size == 0) return nullptr;
        SM_ASSERT(size <= m_Stride, "PoolAllocator '%s': request %zu exceeds block %zu",
                  m_Name, size, m_Stride);
        SM_ASSERT(align <= m_BlockAlign, "PoolAllocator '%s': align %zu exceeds %zu",
                  m_Name, align, m_BlockAlign);
        if (!m_FreeList) AddSlab();        // grow
        if (!m_FreeList) return nullptr;   // growth failed
        void* block = m_FreeList;
        m_FreeList = *reinterpret_cast<void**>(block);
        m_Stats.Used += m_Stride;
        ++m_Stats.AllocCount;
        if (m_Stats.Used > m_Stats.Peak) m_Stats.Peak = m_Stats.Used;
        return block;
    }

    void Deallocate(void* ptr, size_t = 0) override {
        if (!ptr) return;
        SM_ASSERT(m_Stats.Used >= m_Stride,
                  "PoolAllocator '%s': Deallocate underflow (double-free or foreign ptr?)", m_Name);
        *reinterpret_cast<void**>(ptr) = m_FreeList;
        m_FreeList = ptr;
        m_Stats.Used -= m_Stride;
        ++m_Stats.FreeCount;
    }

    void Reset() {
        m_FreeList = nullptr;
        for (void* slab : m_Slabs) ThreadSlab(slab);
        m_Stats.Used = 0; // Peak retained
    }

    const AllocatorStats& Stats() const override { return m_Stats; }
    MemCategory Category() const override { return m_Category; }
    const char* Name() const override { return m_Name; }

private:
    void ThreadSlab(void* slab) {
        uint8_t* base = static_cast<uint8_t*>(slab);
        for (size_t i = 0; i < m_BlockCount; ++i) {
            void* block = base + i * m_Stride;
            *reinterpret_cast<void**>(block) = m_FreeList;
            m_FreeList = block;
        }
    }

    void AddSlab() {
        void* slab = _aligned_malloc(m_Stride * m_BlockCount, m_BlockAlign);
        if (!slab) {
            SM_ERROR("PoolAllocator '%s': slab allocation failed (%zu bytes)",
                     m_Name, m_Stride * m_BlockCount);
            return;
        }
        m_Slabs.push_back(slab);
        ThreadSlab(slab);
        m_Stats.Capacity += m_Stride * m_BlockCount;
    }

    size_t              m_Stride = 0;
    size_t              m_BlockCount = 0;
    size_t              m_BlockAlign = 0;
    void*               m_FreeList = nullptr;
    std::vector<void*>  m_Slabs;
    MemCategory         m_Category;
    const char*         m_Name;
    AllocatorStats      m_Stats;
};

} // namespace Engine
