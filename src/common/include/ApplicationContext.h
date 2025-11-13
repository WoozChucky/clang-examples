#pragma once

#include <typeindex>
#include <memory>
#include <mutex>

#include "Camera.h"
#include "Input.h"
#include "SpscRing.h"
#include "Seqlock.h"

#include "ECS.h"

struct ApplicationSettings {
    uint32_t windowWidth = 1920;
    uint32_t windowHeight = 1080;
    bool vsyncEnabled = true;
};

// Runtime-configurable game thread settings (Render -> Game)
struct GameThreadSettings {
    double TargetTPS = 60.0;           // Target ticks per second
    uint32_t SpinThresholdMicros = 500; // Microseconds to start spinning before target
    bool EnableFrameTimeTracking = true; // Track min/max/avg frame times
};

// Frame timing statistics (Game -> Render)
struct FrameTimeStats {
    double MinFrameTimeMs = 0.0;
    double MaxFrameTimeMs = 0.0;
    double AvgFrameTimeMs = 0.0;
    uint64_t SampleCount = 0;
};

struct UiCommand {
    enum Type : uint8_t { SetVelocity = 0, TogglePause = 1 } type{};
    float fval{}; bool bval{};
};

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

struct SimulationSnapshot {
    uint64_t Tick;      // monotonic tick id
    double Timestamp;   // seconds at tick start
    double TargetTPS;   // intended tick rate (usually 60.0)
    double ActualTPS;   // measured actual tick rate (work time only)
    // Minimal renderable state: a single float position for demo (x in [-1..1])
    float ObjectX;
    float ObjectVX;     // velocity for possible extrapolation
    PerspectiveCamera3D GameCamera;
    OrthographicCamera2D UICamera;
    FrameTimeStats FrameStats; // Frame timing statistics

    // Raw pointer to ECS world snapshot (lifetime managed separately via atomic shared_ptr)
    // DO NOT delete this pointer - it's managed by LatestWorldSnapshot in ApplicationContext
    const ECS* WorldSnapshotPtr = nullptr;
};

// Small atomic for late-latch (latest input state snapshot)
struct InputState {
    double Time;
    double MouseX, MouseY;
};

enum class RendererCommandType : uint8_t { ToggleVSync = 0, TogglePause = 1, Resize = 2 };
struct RendererCommand {
    RendererCommandType Type;
    union {
        struct { uint32_t Width; uint32_t Height; } ResizeParams;
    };
};

struct ApplicationContext {
    // Application settings
    ApplicationSettings Settings{};

    // Game thread settings (Render -> Game via seqlock)
    Seqlock<GameThreadSettings> GameThreadConfig{};

    // Shutdown
    std::atomic<bool> ShutdownRequested{false};

    // Input: Platform -> Game
    static constexpr int InputRingSize = 256;
    SpscRing<InputEvent, InputRingSize> InputRing{};

    // Input: Platform -> ImGui (Renderer Thread)
    static constexpr int ImGuiInputRingSize = 256;
    SpscRing<InputEvent, ImGuiInputRingSize> ImGuiInputRing{};

    // UI: Render -> Game (For future use with Immediate mode UI)
    static constexpr int UiRingSize = 128;
    SpscRing<UiCommand, UiRingSize> UiRing{};

    // ECS Commands: Render -> Game (For entity/component modifications from ImGui)
    static constexpr int ECSCommandRingSize = 128;
    SpscRing<ECSCommand, ECSCommandRingSize> ECSCommandRing{};

    // Platform -> Render (Stuff like pause, vsync, resize)
    static constexpr int RendererCommandRingSize = 16;
    SpscRing<RendererCommand, RendererCommandRingSize> RendererCommandRing{};

    // Late-latched input sample: Platform -> Render
    std::atomic<InputState*> LatestInputStatePtr{nullptr};
    InputState InputStateA{}, InputStateB{};

    // Game -> Render latest snapshot (seqlocked)
    Seqlock<SimulationSnapshot> LatestSnapshot{};
    
    // ECS World snapshot lifetime management (C++20 atomic shared_ptr operations)
    // GameThread: worldSnapshot = state.World.CreateSnapshot(); 
    //             std::atomic_store(&LatestWorldSnapshot, worldSnapshot);
    // RenderThread: auto worldSnapshot = std::atomic_load(&LatestWorldSnapshot);
    std::shared_ptr<const ECS> LatestWorldSnapshot;
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
        } else if (componentData.Type == std::type_index(typeid(MeshComponent))) {
            if (auto* mesh = componentData.Get<MeshComponent>()) {
                world.AddComponent(entity, *mesh);
            }
        }
        // Add more component types as needed
    }

    // Remove a component by type index
    static void RemoveComponentByType(ECS& world, EntityId entity, std::type_index typeIndex) {
        if (typeIndex == std::type_index(typeid(TransformComponent))) {
            world.RemoveComponent<TransformComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(MeshComponent))) {
            world.RemoveComponent<MeshComponent>(entity);
        }
        // Add more component types as needed
    }
};

// #############################################################################
//                           Application Context
// #############################################################################