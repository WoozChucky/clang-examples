#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <typeindex>
#include <string>
#include <algorithm>
#include <thread>
#include <type_traits>
#include <utility>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// Cross-DLL export annotation. Defined as dllexport in ecs.dll's TU,
// dllimport everywhere else. Allow override (defining ECS_API empty
// externally) for static testing scenarios.
#ifndef ECS_API
  #ifdef _WIN32
    #ifdef ECS_EXPORTS
      #define ECS_API __declspec(dllexport)
    #else
      #define ECS_API __declspec(dllimport)
    #endif
  #else
    #define ECS_API
  #endif
#endif

// #############################################################################
//                           Entity & Component IDs
// #############################################################################

using EntityId = uint64_t;
constexpr EntityId INVALID_ENTITY = 0;

// #############################################################################
//                           Component Types (Examples)
// #############################################################################

struct TransformComponent {
    glm::vec3 Position{0.0f, 0.0f, 0.0f};
    glm::vec3 Rotation{0.0f, 0.0f, 0.0f};
    glm::vec3 Scale{1.0f, 1.0f, 1.0f};
};

struct MeshComponent {
    uint32_t MeshId = 0;
    bool Visible = false;
};

struct SubMesh {
    uint32_t IndexStart = 0;
    uint32_t IndexCount = 0;
    uint32_t MaterialIndex = 0;
};

struct MaterialComponent {
    uint32_t MaterialId = 0;
    glm::vec4 BaseColor{1.0f};
    // Bit flags controlling material behavior
    // bit 0 (1): UseTexture — if set, renderer should sample a texture
    // Additional bits reserved for future use
    uint32_t Flags = 0;
};

struct TextComponent {
    std::string Text;
    glm::vec4 Color{1.0f};
    // Reserved for future use
    //std::string Font;
    size_t FontSize = 12;
};

enum class LightningType : uint8_t {
    Directional = 0,
    Point = 1,
    Spot = 2
};

struct LightningComponent {
    LightningType Type = LightningType::Directional;
    glm::vec4 Direction{0.0f, -1.0f, 0.0f, 0.0f}; // for directional lights
    glm::vec4 Color{1.0f};
    float Intensity = 1.0f;
    float Range = 1.0f;
};

struct ParentComponent {
    EntityId Parent = INVALID_ENTITY;
};

struct ChildComponent {
    std::unordered_set<EntityId> Children;
};

// Zero-size marker tagging the singleton directional light driven by the day/night cycle.
struct SunMarker {};

// X-macro: single source of truth for the set of component types that get
// explicit template instantiations in ecs.dll. Adding a new component type
// requires (1) declaring the struct above, (2) adding an X(NewType) line here,
// (3) registering in ECSCommandProcessor in ApplicationContext.h.
#define ECS_FOR_EACH_REGISTERED_COMPONENT(X) \
    X(TransformComponent) \
    X(MeshComponent) \
    X(MaterialComponent) \
    X(TextComponent) \
    X(LightningComponent) \
    X(ParentComponent) \
    X(ChildComponent) \
    X(SunMarker)

// #############################################################################
//                           Component Storage (Type-erased container)
// #############################################################################

class IComponentArray {
public:
    virtual ~IComponentArray() = default;
    virtual void Remove(EntityId entity) = 0;
    [[nodiscard]] virtual bool Has(EntityId entity) const = 0;
    [[nodiscard]] virtual size_t Size() const = 0;

    /**
     * @brief Deep-copies this array. Used by type-erased paths that cannot
     *        invoke MutateArray<T> (e.g. ComponentStore::RemoveAllComponents).
     * @return shared_ptr owning a fresh copy of the array contents.
     */
    [[nodiscard]] virtual std::shared_ptr<IComponentArray> Clone() const = 0;
};

template<typename T>
class ECS_API ComponentArray final : public IComponentArray {
public:
    void Add(const EntityId entity, T component) {
        if (m_EntityToIndex.contains(entity)) {
            // Update existing component
            size_t index = m_EntityToIndex[entity];
            m_Components[index] = component;
            return;
        }

        // Add new component
        const size_t newIndex = m_Components.size();
        m_EntityToIndex[entity] = newIndex;
        m_Components.push_back(component);
        m_IndexToEntity.push_back(entity);
    }

    void Remove(const EntityId entity) override {
        if (!m_EntityToIndex.contains(entity)) {
            return; // Entity doesn't have this component
        }

        // Swap-and-pop for efficient removal
        size_t indexOfRemoved = m_EntityToIndex[entity];
        size_t indexOfLast = m_Components.size() - 1;

        // Swap with last element
        m_Components[indexOfRemoved] = m_Components[indexOfLast];

        // Update mappings for the swapped entity
        EntityId entityOfLast = m_IndexToEntity[indexOfLast];
        m_EntityToIndex[entityOfLast] = indexOfRemoved;
        m_IndexToEntity[indexOfRemoved] = entityOfLast;

        // Remove old mappings
        m_EntityToIndex.erase(entity);
        m_IndexToEntity.pop_back();

        m_Components.pop_back();
    }

    T* Get(const EntityId entity) {
        if (!m_EntityToIndex.contains(entity)) {
            return nullptr;
        }
        return &m_Components[m_EntityToIndex[entity]];
    }

    const T* Get(const EntityId entity) const {
        if (!m_EntityToIndex.contains(entity)) {
            return nullptr;
        }
        return &m_Components[m_EntityToIndex.at(entity)];
    }

    [[nodiscard]] bool Has(const EntityId entity) const override {
        return m_EntityToIndex.contains(entity);
    }

    [[nodiscard]] size_t Size() const override {
        return m_Components.size();
    }

    [[nodiscard]] std::shared_ptr<IComponentArray> Clone() const override {
        return std::make_shared<ComponentArray<T>>(*this);
    }

    // Iterator access for systems
    std::vector<T>& GetComponents() { return m_Components; }
    const std::vector<T>& GetComponents() const { return m_Components; }

    // Get entity for a component index (useful for systems iterating components)
    [[nodiscard]] EntityId GetEntity(const size_t index) const {
        return index < m_IndexToEntity.size() ? m_IndexToEntity[index] : INVALID_ENTITY;
    }

private:
    std::vector<T> m_Components;
    std::unordered_map<EntityId, size_t> m_EntityToIndex;
    std::vector<EntityId> m_IndexToEntity;
};

// Declare that ComponentArray<T> is instantiated elsewhere (in ecs.dll's TU).
// Prevents per-TU local instantiation in editor.exe, game.dll, test_ecs.exe;
// they link against ecs.dll's exported copy.
// Suppressed in ecs.dll's own TU (ECS_EXPORTS defined) because the explicit
// instantiation definitions below take precedence and MSVC warns on the
// combination of dllexport + extern on an instantiation (C4910).
#ifndef ECS_EXPORTS
#define ECS_EXTERN_TEMPLATE_DECL(T) extern template class ECS_API ComponentArray<T>;
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_EXTERN_TEMPLATE_DECL)
#undef ECS_EXTERN_TEMPLATE_DECL
#endif

// #############################################################################
//                           Component Store (Type registry)
// #############################################################################

class ECS_API ComponentStore {
public:
    template<typename T>
    void RegisterComponent() {
        const auto typeIndex = std::type_index(typeid(T));

        if (m_ComponentArrays.contains(typeIndex)) {
            return; // Already registered
        }

        m_ComponentArrays[typeIndex] = std::make_shared<ComponentArray<T>>();
    }

    template<typename T>
    void AddComponent(EntityId entity, T component);

    template<typename T>
    void RemoveComponent(EntityId entity);

    template<typename T>
    const T* GetComponent(EntityId entity) const;

    template<typename T>
    [[nodiscard]] bool HasComponent(EntityId entity) const;

    template<typename T>
    const ComponentArray<T>* GetComponentArray() const {
        const auto typeIndex = std::type_index(typeid(T));

        const auto it = m_ComponentArrays.find(typeIndex);
        if (it == m_ComponentArrays.end()) {
            return nullptr;
        }

        return static_cast<const ComponentArray<T>*>(it->second.get());
    }

    void RemoveAllComponents(EntityId entity);

    void Cleanup() {
        m_ComponentArrays.clear();
    }

    /**
     * @brief Returns a mutable reference to the component array for type T,
     *        cloning the array on first write per tick to preserve any in-flight
     *        snapshot.
     * @tparam T Component type. Must be CopyConstructible.
     * @return Reference to the master's array for T. Valid until the next
     *         CreateSnapshot or RemoveAllComponents call.
     * @threading GameThread only.
     * @cow First call per tick clones the array; subsequent calls return the
     *      same (already-cloned) array.
     */
    template<typename T>
    ComponentArray<T>& MutateArray();

    /**
     * @brief Returns a const pointer to the array for T, or nullptr if no
     *        component of T has been registered yet.
     * @snapshot Safe to call through a snapshot reference; the returned array
     *           is immutable for the snapshot's lifetime.
     */
    template<typename T>
    const ComponentArray<T>* GetArray() const;

    /**
     * @brief Copies the array map (shared_ptr refcount bumps) from `other`.
     *        Used by CreateSnapshot. The destination's dirty set is left empty.
     */
    void CopyArraysFrom(const ComponentStore& other);

    /**
     * @brief Resets the master's per-tick dirty set after a snapshot is published.
     */
    void ClearDirty();

private:
#ifndef NDEBUG
    std::thread::id m_OwnerThread = std::this_thread::get_id();
    void AssertOwnerThread() const {
        assert(std::this_thread::get_id() == m_OwnerThread &&
               "ECS mutated from non-owner thread");
    }
#else
    void AssertOwnerThread() const {}
#endif

    std::unordered_map<std::type_index, std::shared_ptr<IComponentArray>> m_ComponentArrays;
    std::unordered_set<std::type_index> m_DirtyThisTick;
};

// Per-T extern template declarations for ComponentStore methods. Guarded
// with ECS_EXPORTS so ecs.dll's own TU (which provides the definitions)
// doesn't see the extern declarations.
#ifndef ECS_EXPORTS
#define ECS_EXTERN_COMPONENT_STORE_METHODS(T) \
    extern template ECS_API ComponentArray<T>& ComponentStore::MutateArray<T>(); \
    extern template ECS_API const ComponentArray<T>* ComponentStore::GetArray<T>() const; \
    extern template ECS_API void ComponentStore::AddComponent<T>(EntityId, T); \
    extern template ECS_API void ComponentStore::RemoveComponent<T>(EntityId); \
    extern template ECS_API bool ComponentStore::HasComponent<T>(EntityId) const; \
    extern template ECS_API const T* ComponentStore::GetComponent<T>(EntityId) const;
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_EXTERN_COMPONENT_STORE_METHODS)
#undef ECS_EXTERN_COMPONENT_STORE_METHODS
#endif

// #############################################################################
//                           Entity Store (Entity lifecycle management)
// #############################################################################

class EntityStore {
public:
    EntityId CreateEntity() {
        const EntityId id = m_FreeEntities.empty() ? m_NextEntityId++ : m_FreeEntities.back();
        if (!m_FreeEntities.empty()) m_FreeEntities.pop_back();

        m_ActiveEntities.push_back(id);
        return id;
    }

    void DestroyEntity(const EntityId entity) {
        const auto it = std::ranges::find(m_ActiveEntities, entity);
        if (it != m_ActiveEntities.end()) {
            m_ActiveEntities.erase(it);
            m_FreeEntities.push_back(entity); // Recycle ID
        }
    }

    [[nodiscard]] bool IsValid(const EntityId entity) const {
        return std::ranges::find(m_ActiveEntities, entity) != m_ActiveEntities.end();
    }

    [[nodiscard]] size_t GetEntityCount() const {
        return m_ActiveEntities.size();
    }

    [[nodiscard]] const std::vector<EntityId>& GetActiveEntities() const {
        return m_ActiveEntities;
    }

    void Clear() {
        m_ActiveEntities.clear();
        m_FreeEntities.clear();
        m_NextEntityId = 1; // Start from 1 (0 is INVALID_ENTITY)
    }

private:
    EntityId                        m_NextEntityId = 1; // 0 reserved for INVALID_ENTITY
    std::vector<EntityId>           m_ActiveEntities;
    std::vector<EntityId>           m_FreeEntities; // Recycled entity IDs
};

// #############################################################################
//                           ECS World (Main API)
// #############################################################################

class ECS_API ECS {
public:
    // Entity management
    EntityId CreateEntity() {
        return m_EntityStore.CreateEntity();
    }

    void DestroyEntity(EntityId entity);

    [[nodiscard]] bool IsValidEntity(const EntityId entity) const {
        return m_EntityStore.IsValid(entity);
    }

    [[nodiscard]] size_t GetEntityCount() const {
        return m_EntityStore.GetEntityCount();
    }

    [[nodiscard]] const std::vector<EntityId>& GetActiveEntities() const {
        return m_EntityStore.GetActiveEntities();
    }

    // Component management
    template<typename T>
    void AddComponent(EntityId entity, T component);

    template<typename T>
    void RemoveComponent(EntityId entity);

    template<typename T>
    const T* GetComponent(EntityId entity) const;

    template<typename T>
    [[nodiscard]] bool HasComponent(EntityId entity) const;

    // Multi-component queries
    template<typename... Components>
    [[nodiscard]] bool HasComponents(const EntityId entity) const {
        return (HasComponent<Components>(entity) && ...);
    }

    // Get multiple components at once (returns tuple of const pointers)
    // Usage: if (auto [transform, mesh] = world.GetComponents<TransformComponent, MeshComponent>(entity); transform && mesh) { ... }
    template<typename... Components>
    std::tuple<const Components*...> GetComponents(EntityId entity) const {
        return std::make_tuple(GetComponent<Components>(entity)...);
    }

    // Get component array for system iteration (read-only)
    template<typename T>
    const ComponentArray<T>* GetComponentArray() const {
        return m_ComponentStore.GetComponentArray<T>();
    }

    // Iterate entities with specific components (simple view)
    template<typename... Components>
    [[nodiscard]] std::vector<EntityId> View() const {
        std::vector<EntityId> result;
        for (EntityId entity : m_EntityStore.GetActiveEntities()) {
            if (HasComponents<Components...>(entity)) {
                result.push_back(entity);
            }
        }
        return result;
    }

    /**
     * @brief Single-entity in-place edit. Lambda receives a `T&` referring
     *        to the cloned-for-this-tick array slot.
     * @tparam T Component type. Must be CopyConstructible.
     * @tparam F Callable taking `T&`. Return value discarded.
     * @param e Entity that should hold T. No-op if invalid or lacking T.
     * @threading GameThread only.
     * @cow Probes Has(e) first; clones only if the entity holds the component.
     */
    template<typename T, typename F>
    void Modify(EntityId e, F&& fn) {
        const auto* readArr = m_ComponentStore.GetArray<T>();
        if (!readArr || !readArr->Has(e)) return;
        auto& writeArr = m_ComponentStore.MutateArray<T>();
        if (T* c = writeArr.Get(e)) {
            std::forward<F>(fn)(*c);
        }
    }

    /**
     * @brief Bulk-write access to the array for T. Clones once per tick.
     *        Prefer this over Modify when iterating many entities of one type.
     * @threading GameThread only.
     * @cow Triggers a clone on first call per tick.
     */
    template<typename T>
    ComponentArray<T>& MutateArray();

    /**
     * @brief Bulk-read access. Use for systems iterating one component type densely.
     * @return Pointer to const array, or nullptr if type unregistered.
     * @snapshot Safe through a snapshot reference; immutable for snapshot lifetime.
     */
    template<typename T>
    const ComponentArray<T>* GetArray() const;

    void Clear();

    /**
     * @brief Publishes an immutable snapshot of the world for cross-thread reads.
     *
     * Performs a shallow copy of the component-array map (refcount bumps), a deep
     * copy of EntityStore, and resets the master's per-tick dirty set. After
     * return, the next mutation on any component type clones-on-write to keep
     * the returned snapshot isolated.
     *
     * @threading GameThread only (single-writer rule).
     * @snapshot The returned ECS exposes only const accessors.
     */
    [[nodiscard]] std::shared_ptr<const ECS> CreateSnapshot();

private:
    EntityStore m_EntityStore;
    ComponentStore m_ComponentStore;
};

// Per-T extern template declarations for ECS methods. Guarded with ECS_EXPORTS
// so ecs.dll's own TU doesn't see them (provides definitions instead).
#ifndef ECS_EXPORTS
#define ECS_EXTERN_ECS_METHODS(T) \
    extern template ECS_API void ECS::AddComponent<T>(EntityId, T); \
    extern template ECS_API void ECS::RemoveComponent<T>(EntityId); \
    extern template ECS_API bool ECS::HasComponent<T>(EntityId) const; \
    extern template ECS_API const T* ECS::GetComponent<T>(EntityId) const; \
    extern template ECS_API const ComponentArray<T>* ECS::GetArray<T>() const; \
    extern template ECS_API ComponentArray<T>& ECS::MutateArray<T>();
ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_EXTERN_ECS_METHODS)
#undef ECS_EXTERN_ECS_METHODS
#endif

// #############################################################################
//                           Example Usage (in comments)
// #############################################################################

/*
// Create ECS world
ECS world;

// Create entities
EntityId player = world.CreateEntity();
EntityId enemy = world.CreateEntity();

// Add components
world.AddComponent(player, TransformComponent{{0, 0, 0}, {0, 0, 0}, {1, 1, 1}});
world.AddComponent(player, MeshComponent{1, 0, true});

world.AddComponent(enemy, TransformComponent{{10, 0, 0}, {0, 0, 0}, {1, 1, 1}});
world.AddComponent(enemy, MeshComponent{2, 1, true});

// Read a single component (const — snapshot-safe)
if (const auto* transform = world.GetComponent<TransformComponent>(player)) {
    float x = transform->Position.x; // read only
}

// Mutate a single component via Modify (COW-safe, GameThread only)
world.Modify<TransformComponent>(player, [](TransformComponent& t) {
    t.Position.x += 1.0f; // Move player
});

world.Modify<MeshComponent>(player, [](MeshComponent& m) {
    m.Visible = false; // Hide mesh
});

// Read multiple components at once (C++17 structured bindings)
if (auto [transform, mesh] = world.GetComponents<TransformComponent, MeshComponent>(player); transform && mesh) {
    float x = transform->Position.x; // read only
    bool vis = mesh->Visible;        // read only
}

// Check if entity has component
if (world.HasComponent<MeshComponent>(player)) {
    // Render player
}

// Multi-component query (existence check)
if (world.HasComponents<TransformComponent, MeshComponent>(player)) {
    // Entity has both components
}

// Iterate all entities with specific components (simple system, read-only)
for (EntityId entity : world.View<TransformComponent, MeshComponent>()) {
    const auto* transform = world.GetComponent<TransformComponent>(entity);
    const auto* mesh = world.GetComponent<MeshComponent>(entity);
    // Render mesh at transform position (read only)
}

// Bulk-mutate all transforms in a system (COW-safe, one clone per tick)
{
    ComponentArray<TransformComponent>& transforms = world.MutateArray<TransformComponent>();
    for (size_t i = 0; i < transforms.Size(); ++i) {
        TransformComponent& transform = transforms.GetComponents()[i];
        EntityId entity = transforms.GetEntity(i);
        transform.Position.y += 0.1f; // write
    }
}

// Read-only dense iteration via GetArray (snapshot-safe)
if (const auto* transforms = world.GetArray<TransformComponent>()) {
    for (size_t i = 0; i < transforms->Size(); ++i) {
        const TransformComponent& transform = transforms->GetComponents()[i];
        EntityId entity = transforms->GetEntity(i);
        // read transform
    }
}

// Destroy entity (removes all components automatically)
world.DestroyEntity(enemy);
*/
