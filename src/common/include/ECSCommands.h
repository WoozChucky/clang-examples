#pragma once

#include <typeindex>
#include <memory>
#include <cstdint>

#include "ECS.h"
#include "SpscRing.h"

// #############################################################################
//                           ECS Command System (Render -> Game)
// #############################################################################

enum class ECSCommandType : uint8_t {
    CreateEntity = 0,
    DestroyEntity = 1,
    AddComponent = 2,
    RemoveComponent = 3,
    ModifyComponent = 4,
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
};

// #############################################################################
//                           ECS Command Processing (GameThread)
// #############################################################################

class ECSCommandProcessor {
public:
    // Process all pending ECS commands from the command ring
    static void ProcessCommands(ECS& world, SpscRing<ECSCommand, 128>& commandRing) {
        ECSCommand cmd; // Use default constructor explicitly (not aggregate init {})
        while (commandRing.Pop(cmd)) {
            switch (cmd.Type) {
                case ECSCommandType::CreateEntity: {
                    EntityId newEntity = world.CreateEntity();
                    // Could store newEntity somewhere if needed for response
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
            }
        }
    }

private:
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
        }
        // Add more component types as needed
    }
};
