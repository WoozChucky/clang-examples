#pragma once
#include <cstdint>
#include <cstdlib>
#include <lib.h>                 // SM_ERROR
#include <memory/IAllocator.h>
#include <memory/MemUtil.h>      // AlignUp

namespace Engine {

// Fixed-capacity linear (bump) allocator. Allocate is O(1); individual frees are
// no-ops. Reset() reclaims everything (Peak retained). On overflow returns
// nullptr and logs — a deliberate guardrail, not silent growth. Not thread-safe.
class ArenaAllocator : public IAllocator {
public:
    using Marker = size_t;

    // Owns a freshly malloc'd buffer.
    ArenaAllocator(size_t capacity, MemCategory cat, const char* name)
        : m_Category(cat), m_Name(name), m_OwnsMemory(true)
    {
        m_Buffer = static_cast<uint8_t*>(std::malloc(capacity ? capacity : 1));
        if (!m_Buffer) {
            SM_ERROR("ArenaAllocator '%s': failed to allocate %zu bytes", name, capacity);
            m_Stats.Capacity = 0;
            m_OwnsMemory = false;
        } else {
            m_Stats.Capacity = capacity;
        }
    }

    // Views an externally-owned buffer (does not free it).
    ArenaAllocator(void* buffer, size_t capacity, MemCategory cat, const char* name)
        : m_Buffer(static_cast<uint8_t*>(buffer)), m_Category(cat),
          m_Name(name), m_OwnsMemory(false)
    {
        m_Stats.Capacity = m_Buffer ? capacity : 0;
    }

    ~ArenaAllocator() override {
        if (m_OwnsMemory && m_Buffer) std::free(m_Buffer);
        m_Buffer = nullptr;
    }

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    void* Allocate(size_t size, size_t align) override {
        if (!m_Buffer || size == 0) return nullptr;
        const size_t alignedOffset = AlignUp(m_Offset, align);
        const size_t newOffset     = alignedOffset + size;
        if (newOffset > m_Stats.Capacity) {
            SM_ERROR("ArenaAllocator '%s' exhausted: req %zu, used %zu, cap %zu",
                     m_Name, size, m_Offset, m_Stats.Capacity);
            return nullptr;
        }
        m_Offset = newOffset;
        m_Stats.Used = m_Offset;
        if (m_Offset > m_Stats.Peak) m_Stats.Peak = m_Offset;
        ++m_Stats.AllocCount;
        return m_Buffer + alignedOffset;
    }

    void Deallocate(void*, size_t = 0) override {} // bulk free via Reset

    void Reset() {
        m_Offset = 0;
        m_Stats.Used = 0; // Peak retained
    }

    Marker GetMarker() const { return m_Offset; }
    void   RewindTo(Marker m) { m_Offset = m; m_Stats.Used = m; }

    // FrameAllocator-compatible API (so the subclass needs no render-code changes).
    void* AllocateBytes(size_t size, size_t alignment = 16) { return Allocate(size, alignment); }
    template<class T> T* Allocate() { return static_cast<T*>(Allocate(sizeof(T), alignof(T))); }
    template<class T> T* AllocateArray(size_t count) {
        return count ? static_cast<T*>(Allocate(sizeof(T) * count, alignof(T))) : nullptr;
    }
    [[nodiscard]] size_t GetUsedBytes()   const { return m_Stats.Used; }
    [[nodiscard]] size_t GetCapacity()    const { return m_Stats.Capacity; }
    [[nodiscard]] size_t GetPeakUsage()   const { return m_Stats.Peak; }
    [[nodiscard]] float  GetUsagePercent() const {
        return m_Stats.Capacity ? (float)m_Stats.Used / (float)m_Stats.Capacity * 100.0f : 0.0f;
    }
    void ResetPeakUsage() { m_Stats.Peak = m_Stats.Used; }

    const AllocatorStats& Stats() const override { return m_Stats; }
    MemCategory Category() const override { return m_Category; }
    const char* Name() const override { return m_Name; }

private:
    uint8_t*       m_Buffer = nullptr;
    size_t         m_Offset = 0;
    MemCategory    m_Category;
    const char*    m_Name;
    bool           m_OwnsMemory = true;
    AllocatorStats m_Stats;
};

} // namespace Engine
