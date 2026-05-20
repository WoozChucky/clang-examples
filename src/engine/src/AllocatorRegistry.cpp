#include <memory/AllocatorRegistry.h>
#include <memory/IAllocator.h>
#include <algorithm>

namespace Engine {

void AllocatorRegistry::Register(IAllocator* a) {
    if (!a) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Allocators.push_back(a);
}

void AllocatorRegistry::Unregister(IAllocator* a) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Allocators.erase(
        std::remove(m_Allocators.begin(), m_Allocators.end(), a),
        m_Allocators.end());
}

void AllocatorRegistry::ForEach(const std::function<void(IAllocator*)>& fn) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (IAllocator* a : m_Allocators) fn(a);
}

AllocatorStats AllocatorRegistry::SumByCategory(MemCategory cat) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    AllocatorStats sum;
    for (IAllocator* a : m_Allocators) {
        if (a->Category() != cat) continue;
        const AllocatorStats& s = a->Stats();
        sum.Used       += s.Used;
        sum.Peak       += s.Peak;
        sum.Capacity   += s.Capacity;
        sum.AllocCount += s.AllocCount;
        sum.FreeCount  += s.FreeCount;
    }
    return sum;
}

size_t AllocatorRegistry::Count() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Allocators.size();
}

AllocatorRegistry& Registry() {
    static AllocatorRegistry instance; // single instance in Engine.dll
    return instance;
}

} // namespace Engine
