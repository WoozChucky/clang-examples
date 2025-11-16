#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <typeindex>
#include <string>
#include <algorithm>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

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

struct MaterialComponent {
    uint32_t MaterialId = 0;
    uint32_t TextureId = 0;
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
};

// #############################################################################
//                           Entity & Component IDs
// #############################################################################

using EntityId = uint64_t;
constexpr EntityId INVALID_ENTITY = 0;

// #############################################################################
//                           Component Storage (Type-erased container)
// #############################################################################

class IComponentArray {
public:
    virtual ~IComponentArray() = default;
    virtual void Remove(EntityId entity) = 0;
    [[nodiscard]] virtual bool Has(EntityId entity) const = 0;
    [[nodiscard]] virtual size_t Size() const = 0;
};

template<typename T>
class ComponentArray final : public IComponentArray {
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
        m_IndexToEntity[newIndex] = entity;
        m_Components.push_back(component);
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
        m_IndexToEntity.erase(indexOfLast);

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

    // Iterator access for systems
    std::vector<T>& GetComponents() { return m_Components; }
    const std::vector<T>& GetComponents() const { return m_Components; }

    // Get entity for a component index (useful for systems iterating components)
    [[nodiscard]] EntityId GetEntity(const size_t index) const {
        auto it = m_IndexToEntity.find(index);
        return (it != m_IndexToEntity.end()) ? it->second : INVALID_ENTITY;
    }

private:
    std::vector<T> m_Components;
    std::unordered_map<EntityId, size_t> m_EntityToIndex;
    std::unordered_map<size_t, EntityId> m_IndexToEntity;
};

// #############################################################################
//                           Component Store (Type registry)
// #############################################################################

class ComponentStore {
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
    void AddComponent(EntityId entity, T component) {
        GetComponentArray<T>()->Add(entity, component);
    }

    template<typename T>
    void RemoveComponent(EntityId entity) {
        GetComponentArray<T>()->Remove(entity);
    }

    template<typename T>
    T* GetComponent(EntityId entity) {
        return GetComponentArray<T>()->Get(entity);
    }

    template<typename T>
    const T* GetComponent(EntityId entity) const {
        return GetComponentArray<T>()->Get(entity);
    }

    template<typename T>
    [[nodiscard]] bool HasComponent(EntityId entity) const {
        auto array = GetComponentArray<T>();
        return array && array->Has(entity);
    }

    template<typename T>
    ComponentArray<T>* GetComponentArray() {
        const auto typeIndex = std::type_index(typeid(T));

        if (!m_ComponentArrays.contains(typeIndex)) {
            RegisterComponent<T>(); // Auto-register on first use
        }

        return static_cast<ComponentArray<T>*>(m_ComponentArrays[typeIndex].get());
    }

    template<typename T>
    const ComponentArray<T>* GetComponentArray() const {
        const auto typeIndex = std::type_index(typeid(T));

        const auto it = m_ComponentArrays.find(typeIndex);
        if (it == m_ComponentArrays.end()) {
            return nullptr;
        }

        return static_cast<const ComponentArray<T>*>(it->second.get());
    }

    void RemoveAllComponents(EntityId entity) {
        for (auto& [type, array] : m_ComponentArrays) {
            array->Remove(entity);
        }
    }

    void Cleanup() {
        m_ComponentArrays.clear();
    }

private:
    std::unordered_map<std::type_index, std::shared_ptr<IComponentArray>> m_ComponentArrays;
};

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

class ECS {
public:
    // Entity management
    EntityId CreateEntity() {
        return m_EntityStore.CreateEntity();
    }

    void DestroyEntity(const EntityId entity) {
        m_ComponentStore.RemoveAllComponents(entity);
        m_EntityStore.DestroyEntity(entity);
    }

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
    void AddComponent(const EntityId entity, T component) {
        m_ComponentStore.AddComponent<T>(entity, component);
    }

    template<typename T>
    void RemoveComponent(const EntityId entity) {
        m_ComponentStore.RemoveComponent<T>(entity);
    }

    template<typename T>
    T* GetComponent(const EntityId entity) {
        return m_ComponentStore.GetComponent<T>(entity);
    }

    template<typename T>
    const T* GetComponent(const EntityId entity) const {
        return m_ComponentStore.GetComponent<T>(entity);
    }

    template<typename T>
    [[nodiscard]] bool HasComponent(const EntityId entity) const {
        return m_ComponentStore.HasComponent<T>(entity);
    }

    // Multi-component queries
    template<typename... Components>
    [[nodiscard]] bool HasComponents(const EntityId entity) const {
        return (HasComponent<Components>(entity) && ...);
    }

    // Get multiple components at once (returns tuple of pointers)
    // Usage: if (auto [transform, mesh] = world.GetComponents<TransformComponent, MeshComponent>(entity); transform && mesh) { ... }
    template<typename... Components>
    std::tuple<Components*...> GetComponents(EntityId entity) {
        return std::make_tuple(GetComponent<Components>(entity)...);
    }

    // Const version
    template<typename... Components>
    std::tuple<const Components*...> GetComponents(EntityId entity) const {
        return std::make_tuple(GetComponent<Components>(entity)...);
    }

    // Get component array for system iteration
    template<typename T>
    ComponentArray<T>* GetComponentArray() {
        return m_ComponentStore.GetComponentArray<T>();
    }

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

    void Clear() {
        m_EntityStore.Clear();
        m_ComponentStore.Cleanup();
    }

    [[nodiscard]] std::shared_ptr<const ECS> CreateSnapshot() const {
        auto snapshot = std::make_shared<ECS>();
        snapshot->m_EntityStore = m_EntityStore;     // Copy entity IDs
        snapshot->m_ComponentStore = m_ComponentStore; // Copy component arrays
        return snapshot;
	}

private:
    EntityStore m_EntityStore;
    ComponentStore m_ComponentStore;
};

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

// Query single component
if (auto* transform = world.GetComponent<TransformComponent>(player)) {
    transform->Position.x += 1.0f; // Move player
}

// Query multiple components at once (C++17 structured bindings)
if (auto [transform, mesh] = world.GetComponents<TransformComponent, MeshComponent>(player); transform && mesh) {
    transform->Position.x += 1.0f; // Move player
    mesh->Visible = false; // Hide mesh
}

// Alternative: unpack to named variables
auto [playerTransform, playerMesh] = world.GetComponents<TransformComponent, MeshComponent>(player);
if (playerTransform && playerMesh) {
    // Both components exist
    playerTransform->Position.y += 5.0f;
    playerMesh->MaterialId = 2;
}

// Check if entity has component
if (world.HasComponent<MeshComponent>(player)) {
    // Render player
}

// Multi-component query (existence check)
if (world.HasComponents<TransformComponent, MeshComponent>(player)) {
    // Entity has both components
}

// Iterate all entities with specific components (simple system)
for (EntityId entity : world.View<TransformComponent, MeshComponent>()) {
    auto* transform = world.GetComponent<TransformComponent>(entity);
    auto* mesh = world.GetComponent<MeshComponent>(entity);
    // Render mesh at transform position
}

// Or use structured bindings in the loop
for (EntityId entity : world.View<TransformComponent, MeshComponent>()) {
    auto [transform, mesh] = world.GetComponents<TransformComponent, MeshComponent>(entity);
    if (transform && mesh) {
        // Render mesh at transform position
    }
}

// Advanced: Direct component array iteration (cache-friendly for systems)
auto* transforms = world.GetComponentArray<TransformComponent>();
for (size_t i = 0; i < transforms->Size(); ++i) {
    TransformComponent& transform = transforms->GetComponents()[i];
    EntityId entity = transforms->GetEntity(i);
    // Process transform
}

// Destroy entity (removes all components automatically)
world.DestroyEntity(enemy);
*/
