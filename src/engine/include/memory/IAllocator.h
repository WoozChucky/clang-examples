#pragma once
#include <cstddef>
#include <memory/MemoryCategory.h>
#include <memory/AllocatorStats.h>

namespace Engine {

// Minimal allocator interface. Hot paths use concrete allocator types directly
// (devirtualized). This interface exists for polymorphic observation (the
// registry holds IAllocator*). Header-only, not exported: vtables live in each
// deriving module and are consistent across modules.
class IAllocator {
public:
    virtual ~IAllocator() = default;
    // Convention: a size==0 request returns nullptr (no allocation).
    virtual void* Allocate(size_t size, size_t align = alignof(std::max_align_t)) = 0;
    virtual void  Deallocate(void* ptr, size_t size = 0) = 0;
    [[nodiscard]] virtual const AllocatorStats& Stats() const = 0;
    [[nodiscard]] virtual MemCategory Category() const = 0;
    [[nodiscard]] virtual const char* Name() const = 0;
};

} // namespace Engine
