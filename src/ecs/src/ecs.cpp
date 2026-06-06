// ECS code home. All template instantiations + dllexport definitions live here.
#include "ECS.h"
#include <mutex>
#include <atomic>

// Single exported definition of the shared array-pool counters. The struct,
// per-type pools, and MakePooledClone now live in ECS.h (ecs::detail) so
// consumer-defined component types instantiate their own pools locally.
namespace ecs::detail {
ArrayPoolCountersT& ArrayPoolCounters() { static ArrayPoolCountersT c; return c; }
} // namespace ecs::detail

// Explicit class template instantiations — emits one full copy of
// ComponentArray<T> (methods, vtable, RTTI) per registered T into ecs.dll.
#define ECS_INSTANTIATE_CLASS(T) template class ECS_API ComponentArray<T>;
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_INSTANTIATE_CLASS)
#undef ECS_INSTANTIATE_CLASS

// ----- Explicit instantiations of ComponentStore methods per registered T -----

#define ECS_INSTANTIATE_COMPONENT_STORE_METHODS(T) \
    template ECS_API ComponentArray<T>& ComponentStore::MutateArray<T>(); \
    template ECS_API const ComponentArray<T>* ComponentStore::GetArray<T>() const; \
    template ECS_API void ComponentStore::AddComponent<T>(EntityId, T); \
    template ECS_API void ComponentStore::RemoveComponent<T>(EntityId); \
    template ECS_API bool ComponentStore::HasComponent<T>(EntityId) const; \
    template ECS_API const T* ComponentStore::GetComponent<T>(EntityId) const;
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_INSTANTIATE_COMPONENT_STORE_METHODS)
#undef ECS_INSTANTIATE_COMPONENT_STORE_METHODS

// ----- ECS non-templated method definitions -----

void ECS::DestroyEntity(EntityId entity) {
    m_ComponentStore.RemoveAllComponents(entity);
    m_EntityStore.DestroyEntity(entity);
}

namespace {

// Recycles ECS snapshot objects so CreateSnapshot doesn't allocate the shell +
// EntityStore vectors + array map every tick. Mutex-guarded (Acquire once/tick on
// GameThread; Recycle from the deleter on whatever thread drops the last ref).
// LIFO so single-threaded reuse is deterministic.
class SnapshotPool {
public:
    ECS* Acquire() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Free.empty()) {
            ECS* e = m_Free.back();
            m_Free.pop_back();
            ++m_InUse;
            ++m_Reuses;
            return e;
        }
        ECS* e = new ECS();
        ++m_Created;
        ++m_InUse;
        return e;
    }
    void Recycle(ECS* e) noexcept {   // runs from a shared_ptr deleter (noexcept context)
        e->ResetForRecycle();
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Free.push_back(e);
        --m_InUse;
    }
    SnapshotPoolStats Stats() const {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return SnapshotPoolStats{ m_Free.size(), m_InUse, m_Created, m_Reuses };
    }
    ~SnapshotPool() {
        // m_InUse must be 0 at process exit: the app joins both threads and resets
        // LatestWorldSnapshot before static destruction, so every snapshot has been
        // recycled. A non-zero count here means a snapshot outlived teardown.
        assert(m_InUse == 0 && "SnapshotPool destroyed with live snapshots");
        for (ECS* e : m_Free) delete e;
    }
private:
    mutable std::mutex m_Mutex;
    std::vector<ECS*>  m_Free;
    size_t   m_InUse   = 0;
    size_t   m_Created = 0;
    uint64_t m_Reuses  = 0;
};

// Non-leaked Meyers singleton. Safe: the app joins both threads and resets
// LatestWorldSnapshot before teardown, so no recycling deleter fires during
// static destruction.
SnapshotPool& GetSnapshotPool() {
    static SnapshotPool pool;
    return pool;
}

} // namespace

std::shared_ptr<const ECS> ECS::CreateSnapshot() {
    ECS* snap = GetSnapshotPool().Acquire();
    snap->m_EntityStore = m_EntityStore;
    snap->m_SingletonEntity = m_SingletonEntity;
    snap->m_ComponentStore.CopyArraysFrom(m_ComponentStore);
    m_ComponentStore.ClearDirty();
    // The custom deleter recycles instead of freeing, so this can't use make_shared
    // (which would tie the object's lifetime to the control block + free it). The
    // per-tick control-block alloc here is intentional; do NOT "optimize" to make_shared.
    return std::shared_ptr<const ECS>(snap, [](const ECS* p) noexcept {
        GetSnapshotPool().Recycle(const_cast<ECS*>(p));
    });
}

SnapshotPoolStats GetSnapshotPoolStats() {
    return GetSnapshotPool().Stats();
}

const std::unordered_set<std::type_index>& BuiltinComponentTypes() {
    static const std::unordered_set<std::type_index> s = [] {
        std::unordered_set<std::type_index> set;
        #define ECS_ADD_BUILTIN(T) set.emplace(std::type_index(typeid(T)));
        ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_ADD_BUILTIN)
        #undef ECS_ADD_BUILTIN
        return set;
    }();
    return s;
}

void ECS::RemoveNonBuiltinComponentArrays() {
    m_ComponentStore.RemoveArraysNotIn(BuiltinComponentTypes());
}

ComponentArrayPoolStats GetComponentArrayPoolStats() {
    const auto& c = ecs::detail::ArrayPoolCounters();
    return ComponentArrayPoolStats{
        c.Free.load(std::memory_order_relaxed),
        c.InUse.load(std::memory_order_relaxed),
        c.Created.load(std::memory_order_relaxed),
        c.Reuses.load(std::memory_order_relaxed)
    };
}

void ECS::Clear() {
    // Destroy only gameplay (active) entities + their components. The reserved
    // singleton entity is not in the active list, so its singleton components
    // (input, cameras, app-control, config) survive world loads + reloads.
    const std::vector<EntityId> active = m_EntityStore.GetActiveEntities(); // copy; mutated below
    for (const EntityId e : active) {
        m_ComponentStore.RemoveAllComponents(e);
    }
    m_EntityStore.ClearActive();
}

// ----- ECS templated method explicit instantiations per registered T -----

#define ECS_INSTANTIATE_ECS_METHODS(T) \
    template ECS_API void ECS::AddComponent<T>(EntityId, T); \
    template ECS_API void ECS::RemoveComponent<T>(EntityId); \
    template ECS_API bool ECS::HasComponent<T>(EntityId) const; \
    template ECS_API const T* ECS::GetComponent<T>(EntityId) const; \
    template ECS_API const ComponentArray<T>* ECS::GetArray<T>() const; \
    template ECS_API ComponentArray<T>& ECS::MutateArray<T>();
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_INSTANTIATE_ECS_METHODS)
#undef ECS_INSTANTIATE_ECS_METHODS

// ----- ComponentStore non-templated method definitions -----

void ComponentStore::RemoveAllComponents(EntityId entity) {
    AssertOwnerThread();
    for (auto& [type, slot] : m_ComponentArrays) {
        if (!slot->Has(entity)) continue;  // skip arrays that don't hold the entity
        if (m_DirtyThisTick.insert(type).second) {
            slot = slot->Clone();          // first write this tick → clone (virtual)
        }
        slot->Remove(entity);
    }
}

void ComponentStore::CopyArraysFrom(const ComponentStore& other) {
    m_ComponentArrays = other.m_ComponentArrays;
}

void ComponentStore::ClearDirty() {
    AssertOwnerThread();
    m_DirtyThisTick.clear();
}
