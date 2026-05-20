#pragma once
#include <Engine.h>
#include <memory/MemoryCategory.h>
#include <memory/AllocatorStats.h>
#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

namespace Engine {

class IAllocator;

// Non-owning observer registry. Single instance via Registry(), which lives in
// Engine.dll's TU so every module sees the same one. All methods are out-of-line
// (defined in AllocatorRegistry.cpp) so callers never touch the std members
// across the DLL boundary. Locked, but only on register/unregister/enumerate —
// never on the allocation hot path.
// Caller must Unregister(a) before *a is destroyed; the registry never owns or outlives its entries.
class ENGINE_API AllocatorRegistry {
public:
    void Register(IAllocator* a);
    void Unregister(IAllocator* a);
    // The callback runs under the registry lock; it must NOT call back into the registry (non-recursive mutex -> deadlock).
    void ForEach(const std::function<void(IAllocator*)>& fn) const;
    AllocatorStats SumByCategory(MemCategory cat) const;
    [[nodiscard]] size_t Count() const;

private:
    mutable std::mutex       m_Mutex;
    std::vector<IAllocator*> m_Allocators;
};

ENGINE_API AllocatorRegistry& Registry();

} // namespace Engine
