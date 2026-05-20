#pragma once
#include <cstddef>
#include <cstdint>

namespace Engine {

struct AllocatorStats {
    size_t   Used     = 0; // bytes currently handed out
    size_t   Peak     = 0; // high-water mark of Used
    size_t   Capacity = 0; // total backing bytes
    uint64_t AllocCount = 0;
    uint64_t FreeCount  = 0;
};

} // namespace Engine
