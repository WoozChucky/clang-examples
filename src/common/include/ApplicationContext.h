#pragma once

#include <typeindex>
#include <memory>
#include <mutex>

#include "Camera.h"
#include "Input.h"
#include "SpscRing.h"
#include "Seqlock.h"

#include "ECS.h"
#include "ECSCommands.h"

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

struct MeshVertex
{
    float px, py, pz;   // POSITION
    float nx, ny, nz;   // NORMAL
    float u, v;         // TEXCOORD0
};

enum class RendererCommandType : uint8_t {
    Invalid = 0,
    ToggleVSync = 1,
    TogglePause = 2,
    Resize = 3,
    RequestMesh = 4,
    RequestMaterial = 5,
    RequestModel = 6
};
struct RendererCommand {
    RendererCommandType Type{};
    uint64_t TicketId{0};
    union {
        struct {
            uint32_t Width;
            uint32_t Height;
        } ResizeParams;

        struct {
            MeshVertex* Vertices;
            size_t VertexCount;
            uint32_t* Indices;
            size_t IndexCount;
        } MeshRequest;

        struct {
            uint32_t Width;
            uint32_t Height;
            uint32_t* Texture; // optional RGBA8 pixels (w*h entries)
            size_t TextureSize;
        } MaterialRequest;

        struct {
            MeshVertex* Vertices;
            size_t VertexCount;
            uint32_t* Indices;
            size_t IndexCount;
            bool UseTexture;
            uint32_t Width;
            uint32_t Height;
            uint32_t* Texture; // optional RGBA8 pixels (w*h entries)
            size_t TextureSize;
        } ModelRequest;
    };
};

struct ModelHandle { uint32_t Index; };
struct MeshHandle { uint32_t Index; };
struct MaterialHandle { uint32_t Index; };

enum class RendererResponseType : uint8_t {
    Invalid = 0,
    MeshUpload = 1,
    MaterialUpload = 2,
    ModelUpload = 3
};
struct RendererResponse {
    RendererResponseType Type{};
    uint64_t TicketId{};
    union {
        struct {
            bool Valid;
            MeshHandle Handle;
        } Mesh;

        struct {
            bool Valid;
            MaterialHandle Handle;
        } Material;

        struct {
            bool Valid;
            ModelHandle Handle;
            //MeshHandle Mesh;
            //MaterialHandle Material;
        } Model;
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

    // ECS Commands: Render -> Game (For entity/component modifications from ImGui)
    static constexpr int ECSCommandRingSize = 128;
    SpscRing<ECSCommand, ECSCommandRingSize> ECSCommandRing{};


    static constexpr int RendererCommandRingSize = 64;
    // Platform -> Render (Stuff like pause, vsync, resize)
    SpscRing<RendererCommand, RendererCommandRingSize> PRCommandRing{};
    // Game -> Render (request meshes, texture handles, uploads, etc)
    SpscRing<RendererCommand, RendererCommandRingSize> GRCommandRing{};
    // Render -> Game (responses to mesh/model requests)
    SpscRing<RendererResponse, RendererCommandRingSize> RGCommandRing{};

    // Game -> Render latest snapshot (seqlocked)
    Seqlock<SimulationSnapshot> LatestSnapshot{};

    // ECS World snapshot lifetime management (C++20 atomic shared_ptr operations)
    // GameThread: worldSnapshot = state.World.CreateSnapshot();
    //             std::atomic_store(&LatestWorldSnapshot, worldSnapshot);
    // RenderThread: auto worldSnapshot = std::atomic_load(&LatestWorldSnapshot);
    std::atomic<std::shared_ptr<const ECS>> LatestWorldSnapshot;
};
