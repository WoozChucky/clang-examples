#pragma once

#include <typeindex>
#include <memory>
#include <cstdint>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "ECS.h"
#include "SpscRing.h"
#include "ComponentSerializerRegistry.h"
#include "lib.h"  // SM_WARN

// #############################################################################
//                           ECS Command System (Render -> Game)
// #############################################################################

enum class ECSCommandType : uint8_t {
    CreateEntity = 0,
    DestroyEntity = 1,
    AddComponent = 2,
    RemoveComponent = 3,
    ModifyComponent = 4,
    DuplicateEntity = 5,
    RebuildNavMesh = 6,  // engine hook — dispatched via ECSCommandHooks::OnRebuildNavMesh
    BakeNavMesh = 7,     // engine hook — dispatched via ECSCommandHooks::OnBakeNavMesh
    ModifyComponentJson = 8,
    AddComponentByName  = 9,
    RemoveComponentByName = 10,
};

// Type-erased component storage for commands
struct ComponentData {
    std::type_index Type;
    std::shared_ptr<void> Data; // Type-erased component data
    size_t Size;

    // Default constructor - initializes Type with typeid(void)
    ComponentData()
        : Type(typeid(void))
        , Data(nullptr)
        , Size(0)
    {}

    // Full constructor
    ComponentData(std::type_index type, std::shared_ptr<void> data, size_t size)
        : Type(type)
        , Data(std::move(data))
        , Size(size)
    {}

    template<typename T>
    static ComponentData Create(const T& component) {
        auto data = std::make_shared<T>(component);
        return ComponentData(
            std::type_index(typeid(T)),
            std::static_pointer_cast<void>(data),
            sizeof(T)
        );
    }

    template<typename T>
    T* Get() {
        if (Type != std::type_index(typeid(T))) {
            return nullptr;
        }
        return static_cast<T*>(Data.get());
    }

    template<typename T>
    const T* Get() const {
        if (Type != std::type_index(typeid(T))) {
            return nullptr;
        }
        return static_cast<const T*>(Data.get());
    }
};

struct ECSCommand {
    ECSCommandType Type;
    EntityId TargetEntity;
    ComponentData Component; // For Add/Modify operations
    std::type_index ComponentType; // For Remove operations
    std::string ComponentName;  // for *ByName / ModifyComponentJson (registry key)
    std::string ComponentJson;  // for ModifyComponentJson (serialized component)

    // Default constructor
    ECSCommand()
        : Type(ECSCommandType::CreateEntity)
        , TargetEntity(INVALID_ENTITY)
        , Component()
        , ComponentType(typeid(void))
    {}

    // Constructor with type only
    explicit ECSCommand(ECSCommandType type)
        : Type(type)
        , TargetEntity(INVALID_ENTITY)
        , Component()
        , ComponentType(typeid(void))
    {}

    // Constructor with type and entity
    ECSCommand(ECSCommandType type, EntityId entity)
        : Type(type)
        , TargetEntity(entity)
        , Component()
        , ComponentType(typeid(void))
    {}

    // Constructor with type, entity, and component data
    ECSCommand(ECSCommandType type, EntityId entity, ComponentData component)
        : Type(type)
        , TargetEntity(entity)
        , Component(std::move(component))
        , ComponentType(typeid(void))
    {}

    // Constructor with type, entity, empty component, and component type (for RemoveComponent)
    ECSCommand(ECSCommandType type, EntityId entity, ComponentData component, std::type_index componentType)
        : Type(type)
        , TargetEntity(entity)
        , Component(std::move(component))
        , ComponentType(componentType)
    {}

    // Factory methods for type-safe command creation
    static ECSCommand CreateEntity() {
        return ECSCommand(ECSCommandType::CreateEntity);
    }

    static ECSCommand DestroyEntity(EntityId entity) {
        return ECSCommand(ECSCommandType::DestroyEntity, entity);
    }

    static ECSCommand DuplicateEntity(EntityId entity) {
        return ECSCommand(ECSCommandType::DuplicateEntity, entity);
    }

    static ECSCommand RebuildNavMesh() {
        return ECSCommand(ECSCommandType::RebuildNavMesh);
    }

    static ECSCommand BakeNavMesh() {
        return ECSCommand(ECSCommandType::BakeNavMesh);
    }

    template<typename T>
    static ECSCommand AddComponent(EntityId entity, const T& component) {
        return ECSCommand(
            ECSCommandType::AddComponent,
            entity,
            ComponentData::Create(component)
        );
    }

    template<typename T>
    static ECSCommand RemoveComponent(EntityId entity) {
        return ECSCommand(
            ECSCommandType::RemoveComponent,
            entity,
            ComponentData{},
            std::type_index(typeid(T))
        );
    }

    template<typename T>
    static ECSCommand ModifyComponent(EntityId entity, const T& component) {
        return ECSCommand(
            ECSCommandType::ModifyComponent,
            entity,
            ComponentData::Create(component)
        );
    }

    static ECSCommand ModifyComponentJson(EntityId entity, std::string name, std::string json) {
        ECSCommand c(ECSCommandType::ModifyComponentJson, entity);
        c.ComponentName = std::move(name);
        c.ComponentJson = std::move(json);
        return c;
    }
    static ECSCommand AddComponentByName(EntityId entity, std::string name) {
        ECSCommand c(ECSCommandType::AddComponentByName, entity);
        c.ComponentName = std::move(name);
        return c;
    }
    static ECSCommand RemoveComponentByName(EntityId entity, std::string name) {
        ECSCommand c(ECSCommandType::RemoveComponentByName, entity);
        c.ComponentName = std::move(name);
        return c;
    }
};

// #############################################################################
//                           ECS Command Processing (GameThread)
// #############################################################################

// Engine-side handlers for commands that can't be processed in pure-ECS-land
// (e.g., RebuildNavMesh needs to call NavMeshSystem::Rebuild, which is engine-private).
// Default-constructed hooks struct means "no engine-side handlers" — commands are silently
// dropped. GameThread populates this with NavMeshSystem::Rebuild for the rebuild case.
struct ECSCommandHooks {
    std::function<void(ECS&)> OnRebuildNavMesh; // optional
    std::function<void(ECS&)> OnBakeNavMesh;    // optional — Spec 4 disk bake trigger
};

class ECSCommandProcessor {
public:
    // Process all pending ECS commands from the command ring
    static void ProcessCommands(ECS& world, SpscRing<ECSCommand, 128>& commandRing,
                                const ECSCommandHooks& hooks = {}) {
        ECSCommand cmd; // Use default constructor explicitly (not aggregate init {})
        while (commandRing.Pop(cmd)) {
            switch (cmd.Type) {
                case ECSCommandType::CreateEntity: {
                    world.CreateEntity();
                    break;
                }

                case ECSCommandType::DestroyEntity: {
                    if (cmd.TargetEntity != INVALID_ENTITY) {
                        world.DestroyEntity(cmd.TargetEntity);
                    }
                    break;
                }

                case ECSCommandType::AddComponent: {
                    if (cmd.TargetEntity != INVALID_ENTITY) {
                        ApplyComponentCommand(world, cmd.TargetEntity, cmd.Component, true);
                    }
                    break;
                }

                case ECSCommandType::RemoveComponent: {
                    if (cmd.TargetEntity != INVALID_ENTITY) {
                        RemoveComponentByType(world, cmd.TargetEntity, cmd.ComponentType);
                    }
                    break;
                }

                case ECSCommandType::ModifyComponent: {
                    if (cmd.TargetEntity != INVALID_ENTITY) {
                        ApplyComponentCommand(world, cmd.TargetEntity, cmd.Component, false);
                    }
                    break;
                }

                case ECSCommandType::AddComponentByName: {
                    if (cmd.TargetEntity != INVALID_ENTITY) {
                        if (const auto* en = SerializerRegistry().Find(cmd.ComponentName)) en->addDefault(world, cmd.TargetEntity);
                        else SM_WARN("AddComponentByName: no serializer for '%s'", cmd.ComponentName.c_str());
                    }
                    break;
                }
                case ECSCommandType::RemoveComponentByName: {
                    if (cmd.TargetEntity != INVALID_ENTITY) {
                        if (const auto* en = SerializerRegistry().Find(cmd.ComponentName)) en->remove(world, cmd.TargetEntity);
                        else SM_WARN("RemoveComponentByName: no serializer for '%s'", cmd.ComponentName.c_str());
                    }
                    break;
                }
                case ECSCommandType::ModifyComponentJson: {
                    if (cmd.TargetEntity != INVALID_ENTITY) {
                        const auto* en = SerializerRegistry().Find(cmd.ComponentName);
                        if (!en) { SM_WARN("ModifyComponentJson: no serializer for '%s'", cmd.ComponentName.c_str()); break; }
                        try { en->load(world, cmd.TargetEntity, nlohmann::json::parse(cmd.ComponentJson)); }
                        catch (const std::exception& ex) { SM_WARN("ModifyComponentJson('%s') parse/apply failed: %s", cmd.ComponentName.c_str(), ex.what()); }
                    }
                    break;
                }

                case ECSCommandType::DuplicateEntity: {
                    if (cmd.TargetEntity != INVALID_ENTITY && world.IsValidEntity(cmd.TargetEntity)) {
                        const EntityId dst = world.CreateEntity();
                        DuplicateEntityComponents(world, cmd.TargetEntity, dst);
                    }
                    break;
                }

                case ECSCommandType::RebuildNavMesh: {
                    if (hooks.OnRebuildNavMesh) hooks.OnRebuildNavMesh(world);
                    break;
                }

                case ECSCommandType::BakeNavMesh: {
                    if (hooks.OnBakeNavMesh) hooks.OnBakeNavMesh(world);
                    break;
                }
            }
        }
    }

private:
    // Copy the editor-facing per-entity components from src to dst via the serializer registry.
    // The registry holds exactly the persisted per-entity types — singletons (cameras/viewport/
    // input/atmosphere/game-state/action-queue/navmesh-config) and hierarchy (Parent/Child) are
    // not registered, so they are naturally excluded. Game-defined components are copied too.
    static void DuplicateEntityComponents(ECS& world, EntityId src, EntityId dst) {
        for (const auto& entry : SerializerRegistry().Entries())
            if (entry.copyTo) entry.copyTo(world, src, dst);
    }

    // Apply a component command (add or modify)
    static void ApplyComponentCommand(ECS& world, EntityId entity, const ComponentData& componentData, bool isAdd) {
        // Handle known component types
        if (componentData.Type == std::type_index(typeid(TransformComponent))) {
            if (auto* transform = componentData.Get<TransformComponent>()) {
                world.AddComponent(entity, *transform); // AddComponent handles both add and update
            }
        } else if (componentData.Type == std::type_index(typeid(LightningComponent))) {
            if (auto* lightning = componentData.Get<LightningComponent>()) {
                world.AddComponent(entity, *lightning);
            }
        } else if (componentData.Type == std::type_index(typeid(MeshComponent))) {
            if (auto* mesh = componentData.Get<MeshComponent>()) {
                world.AddComponent(entity, *mesh);
            }
        } else if (componentData.Type == std::type_index(typeid(MaterialComponent))) {
            if (auto* material = componentData.Get<MaterialComponent>()) {
                world.AddComponent(entity, *material);
            }
        } else if (componentData.Type == std::type_index(typeid(TextComponent))) {
            if (auto* text = componentData.Get<TextComponent>()) {
                world.AddComponent(entity, *text);
            }
        } else if (componentData.Type == std::type_index(typeid(SunMarker))) {
            world.AddComponent(entity, SunMarker{});
        } else if (componentData.Type == std::type_index(typeid(DayNightConfigComponent))) {
            if (auto* cfg = componentData.Get<DayNightConfigComponent>()) {
                world.AddComponent(entity, *cfg); // AddComponent updates if present
            }
        } else if (componentData.Type == std::type_index(typeid(FogComponent))) {
            if (auto* fog = componentData.Get<FogComponent>()) {
                world.AddComponent(entity, *fog);
            }
        } else if (componentData.Type == std::type_index(typeid(SkyComponent))) {
            if (auto* sky = componentData.Get<SkyComponent>()) {
                world.AddComponent(entity, *sky);
            }
        } else if (componentData.Type == std::type_index(typeid(UIRectComponent))) {
            if (auto* rect = componentData.Get<UIRectComponent>()) {
                world.AddComponent(entity, *rect);
            }
        } else if (componentData.Type == std::type_index(typeid(StateScopeComponent))) {
            if (auto* scope = componentData.Get<StateScopeComponent>()) {
                world.AddComponent(entity, *scope);
            }
        } else if (componentData.Type == std::type_index(typeid(ColliderComponent))) {
            if (auto* btn = componentData.Get<ColliderComponent>()) {
                world.AddComponent(entity, *btn);
            }
        } else if (componentData.Type == std::type_index(typeid(NavMeshSourceComponent))) {
            if (auto* src = componentData.Get<NavMeshSourceComponent>()) {
                world.AddComponent(entity, *src);
            }
        } else if (componentData.Type == std::type_index(typeid(NavMeshConfigComponent))) {
            if (auto* cfg = componentData.Get<NavMeshConfigComponent>()) {
                world.AddComponent(entity, *cfg); // AddComponent updates if present (singleton edit)
            }
        } else if (componentData.Type == std::type_index(typeid(NavObstacleComponent))) {
            if (auto* obs = componentData.Get<NavObstacleComponent>()) {
                world.AddComponent(entity, *obs);
            }
        } else if (componentData.Type == std::type_index(typeid(NavAgentComponent))) {
            if (auto* a = componentData.Get<NavAgentComponent>()) {
                world.AddComponent(entity, *a);
            }
        } else if (componentData.Type == std::type_index(typeid(NavTargetComponent))) {
            if (auto* t = componentData.Get<NavTargetComponent>()) {
                world.AddComponent(entity, *t);
            }
        } else if (componentData.Type == std::type_index(typeid(NavConstrainedComponent))) {
            world.AddComponent(entity, NavConstrainedComponent{});
        } else if (componentData.Type == std::type_index(typeid(NavClassComponent))) {
            if (auto* nc = componentData.Get<NavClassComponent>()) {
                world.AddComponent(entity, *nc);
            }
        }
        // Add more component types as needed
    }

    // Remove a component by type index
    static void RemoveComponentByType(ECS& world, EntityId entity, std::type_index typeIndex) {
        if (typeIndex == std::type_index(typeid(TransformComponent))) {
            world.RemoveComponent<TransformComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(LightningComponent))) {
            world.RemoveComponent<LightningComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(MeshComponent))) {
            world.RemoveComponent<MeshComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(MaterialComponent))) {
            world.RemoveComponent<MaterialComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(TextComponent))) {
            world.RemoveComponent<TextComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(SunMarker))) {
            world.RemoveComponent<SunMarker>(entity);
        } else if (typeIndex == std::type_index(typeid(DayNightConfigComponent))) {
            world.RemoveComponent<DayNightConfigComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(FogComponent))) {
            world.RemoveComponent<FogComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(SkyComponent))) {
            world.RemoveComponent<SkyComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(UIRectComponent))) {
            world.RemoveComponent<UIRectComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(StateScopeComponent))) {
            world.RemoveComponent<StateScopeComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(ColliderComponent))) {
            world.RemoveComponent<ColliderComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(NavMeshSourceComponent))) {
            world.RemoveComponent<NavMeshSourceComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(NavMeshConfigComponent))) {
            world.RemoveComponent<NavMeshConfigComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(NavObstacleComponent))) {
            world.RemoveComponent<NavObstacleComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(NavAgentComponent))) {
            world.RemoveComponent<NavAgentComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(NavTargetComponent))) {
            world.RemoveComponent<NavTargetComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(NavConstrainedComponent))) {
            world.RemoveComponent<NavConstrainedComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(NavClassComponent))) {
            world.RemoveComponent<NavClassComponent>(entity);
        }
        // Add more component types as needed
    }
};
