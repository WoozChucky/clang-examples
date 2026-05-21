// ECS code home. All template instantiations + dllexport definitions live here.
#include "ECS.h"
#include <mutex>
#include <atomic>

namespace {

// Aggregate counters across all per-type ComponentArray pools. Atomic so the panel
// (RenderThread) can read while GameThread/RenderThread Acquire/Recycle. Free stays
// equal to the summed free-list sizes (++ on Recycle, -- on reuse-Acquire).
struct ArrayPoolCountersT {
    std::atomic<size_t>   Free{0};
    std::atomic<size_t>   InUse{0};
    std::atomic<size_t>   Created{0};
    std::atomic<uint64_t> Reuses{0};
    void OnCreate()  noexcept { ++InUse; ++Created; }
    void OnReuse()   noexcept { --Free;  ++InUse; ++Reuses; }
    void OnRecycle() noexcept { ++Free;  --InUse; }
};
ArrayPoolCountersT& ArrayPoolCounters() { static ArrayPoolCountersT c; return c; }

// Per-type free-list of recycled ComponentArray<T>. Mutex-guarded: Recycle fires from a
// shared_ptr deleter on whatever thread drops the last ref (RenderThread when a snapshot
// recycles, GameThread otherwise). Non-leaked Meyers singleton per T; safe because the
// app joins both threads + resets LatestWorldSnapshot before static destruction, so no
// Recycle races the dtor. No InUse==0 assert (shutdown ordering, matching the staging pool).
template<typename T>
class ComponentArrayPool {
public:
    ComponentArray<T>* Acquire() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Free.empty()) {
            ComponentArray<T>* a = m_Free.back();
            m_Free.pop_back();
            ArrayPoolCounters().OnReuse();
            return a;
        }
        ArrayPoolCounters().OnCreate();
        return new ComponentArray<T>();
    }
    void Recycle(ComponentArray<T>* a) noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Free.push_back(a);
        ArrayPoolCounters().OnRecycle();
    }
    ~ComponentArrayPool() { for (ComponentArray<T>* a : m_Free) delete a; }
private:
    std::mutex m_Mutex;
    std::vector<ComponentArray<T>*> m_Free;
};

template<typename T>
ComponentArrayPool<T>& GetArrayPool() { static ComponentArrayPool<T> pool; return pool; }

// Acquire a recycled (or fresh) array, copy src into it reusing capacity, and wrap it in a
// shared_ptr whose deleter returns it to the pool instead of freeing.
template<typename T>
std::shared_ptr<ComponentArray<T>> MakePooledClone(const ComponentArray<T>& src) {
    ComponentArray<T>* arr = GetArrayPool<T>().Acquire();
    arr->CopyFrom(src);
    return std::shared_ptr<ComponentArray<T>>(arr, [](ComponentArray<T>* p) noexcept {
        GetArrayPool<T>().Recycle(p);
    });
}

} // namespace

// Out-of-line so it can reach the file-local array pool. Instantiated for each registered
// T by the explicit `template class ComponentArray<T>` block below.
template<typename T>
std::shared_ptr<IComponentArray> ComponentArray<T>::Clone() const {
    return MakePooledClone(*this);
}

// Explicit class template instantiations — emits one full copy of
// ComponentArray<T> (methods, vtable, RTTI) per registered T into ecs.dll.
#define ECS_INSTANTIATE_CLASS(T) template class ComponentArray<T>;
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_INSTANTIATE_CLASS)
#undef ECS_INSTANTIATE_CLASS

// ----- ComponentStore templated method definitions -----

template<typename T>
ComponentArray<T>& ComponentStore::MutateArray() {
    AssertOwnerThread();
    const auto typeIndex = std::type_index(typeid(T));
    auto& slot = m_ComponentArrays[typeIndex];
    if (!slot) {
        slot = std::make_shared<ComponentArray<T>>();
    }
    if (m_DirtyThisTick.insert(typeIndex).second) {
        // First mutation since last snapshot — clone.
        slot = MakePooledClone<T>(static_cast<const ComponentArray<T>&>(*slot));
    }
    return static_cast<ComponentArray<T>&>(*slot);
}

template<typename T>
const ComponentArray<T>* ComponentStore::GetArray() const {
    const auto typeIndex = std::type_index(typeid(T));
    const auto it = m_ComponentArrays.find(typeIndex);
    if (it == m_ComponentArrays.end()) {
        return nullptr;
    }
    return static_cast<const ComponentArray<T>*>(it->second.get());
}

template<typename T>
void ComponentStore::AddComponent(EntityId entity, T component) {
    MutateArray<T>().Add(entity, component);
}

template<typename T>
void ComponentStore::RemoveComponent(EntityId entity) {
    MutateArray<T>().Remove(entity);
}

template<typename T>
bool ComponentStore::HasComponent(EntityId entity) const {
    auto array = GetComponentArray<T>();
    return array && array->Has(entity);
}

template<typename T>
const T* ComponentStore::GetComponent(EntityId entity) const {
    const auto componentArray = GetComponentArray<T>();
    if (!componentArray) {
        return nullptr;
    }
    return componentArray->Get(entity);
}

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

// ----- ECS templated method definitions -----

template<typename T>
void ECS::AddComponent(EntityId entity, T component) {
    m_ComponentStore.AddComponent<T>(entity, component);
}

template<typename T>
void ECS::RemoveComponent(EntityId entity) {
    m_ComponentStore.RemoveComponent<T>(entity);
}

template<typename T>
bool ECS::HasComponent(EntityId entity) const {
    return m_ComponentStore.HasComponent<T>(entity);
}

template<typename T>
const T* ECS::GetComponent(EntityId entity) const {
    return m_ComponentStore.GetComponent<T>(entity);
}

template<typename T>
const ComponentArray<T>* ECS::GetArray() const {
    return m_ComponentStore.GetArray<T>();
}

template<typename T>
ComponentArray<T>& ECS::MutateArray() {
    return m_ComponentStore.MutateArray<T>();
}

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

ComponentArrayPoolStats GetComponentArrayPoolStats() {
    const auto& c = ArrayPoolCounters();
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
