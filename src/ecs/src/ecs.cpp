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
