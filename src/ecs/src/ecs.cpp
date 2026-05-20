// ECS code home. All template instantiations + dllexport definitions live here.
#include "ECS.h"

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
        slot = std::make_shared<ComponentArray<T>>(
                   static_cast<const ComponentArray<T>&>(*slot));
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

std::shared_ptr<const ECS> ECS::CreateSnapshot() {
    auto snap = std::make_shared<ECS>();
    snap->m_EntityStore = m_EntityStore;                     // value copy
    snap->m_SingletonEntity = m_SingletonEntity;             // preserve reserved id
    snap->m_ComponentStore.CopyArraysFrom(m_ComponentStore); // shallow shared_ptr copy
    m_ComponentStore.ClearDirty();
    return snap;
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
