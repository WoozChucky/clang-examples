#pragma once

#include <typeindex>
#include <memory>
#include <mutex>

#include "Camera.h"
#include "Input.h"
#include "SpscRing.h"
#include "Seqlock.h"
#include "CameraView.h"

#include "ECS.h"
#include "ECSCommands.h"
#include "lib.h"
#include "ServerControl.h"   // kDedicatedServerDefaultPort

struct ApplicationSettings {
    RendererAPI Backend    = RendererAPI::DirectX12;
    uint32_t    windowWidth   = 1920;
    uint32_t    windowHeight  = 1080;
    bool        vsyncEnabled  = true;
    int         aaMode        = 1;   // 0 = Off, 1 = FXAA, 2 = SMAA (see AaModeMigration.h)
    bool        shadowEnabled    = true;
    float       shadowBias       = 0.0015f;
    float       shadowCoverage   = 80.0f;
    float       shadowNearExtend = 50.0f;
    bool        ssaoEnabled      = true;
    float       ssaoRadius       = 0.5f;
    float       ssaoIntensity    = 1.0f;
    float       ssaoPower        = 2.0f;
    float       ssaoBias         = 0.025f;
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
    FrameTimeStats FrameStats; // Frame timing statistics

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
    RequestModel = 6,
    SwapBackend = 7
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
            SubMesh* SubMeshes;
            size_t SubMeshCount;
        } MeshRequest;

        struct {
            uint32_t Width;
            uint32_t Height;
            uint32_t* Texture; // optional RGBA8 pixels (w*h entries)
        } MaterialRequest;

        struct {
            RendererAPI TargetApi;
        } SwapBackend;
    };
};

struct ModelHandle { uint32_t Index; };
struct MeshHandle { uint32_t Index; };
struct MaterialHandle { uint32_t Index; };

enum class RendererResponseType : uint8_t {
    Invalid = 0,
    MeshUpload = 1,
    MaterialUpload = 2,
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
    };
};

struct ApplicationContext {
    // Application settings
    ApplicationSettings Settings{};

    // Game thread settings (Render -> Game via seqlock)
    Seqlock<GameThreadSettings> GameThreadConfig{};

    // Shutdown
    std::atomic<bool> ShutdownRequested{false};

    // Renderer hot-swap coordination (Phase B).
    // RenderThread sets SwapInProgress; GameThread acks via GameThreadPaused.
    std::atomic<bool> SwapInProgress{false};
    std::atomic<bool> GameThreadPaused{false};

    // Editor scene-viewport size, packed (width<<32 | height). 0 => use the OS window size.
    // Written by the editor overlay (RenderThread) each frame; read by the GameThread for the
    // camera aspect + UI ortho. The runtime never writes it, so it keeps the full-window aspect.
    std::atomic<uint64_t> SceneViewportSize{0};

    // Editor scene-viewport top-left in window coords, packed (originX<<32 | originY).
    // Written by the editor overlay (RenderThread); read by the GameThread to map the
    // window-space mouse into UI/viewport space. The runtime never writes it (stays 0).
    std::atomic<uint64_t> SceneViewportOrigin{0};

    // Editor input routing (RenderThread writes, PlatformThread reads). Default true so the
    // runtime and the editor's first frame route all input to the game; the editor overlay
    // overwrites these every frame from the Viewport panel's hover/focus + ImGui capture state.
    std::atomic<bool> GameAcceptsMouse{true};
    std::atomic<bool> GameAcceptsKeyboard{true};

    // True once an ImGui overlay exists to drain ImGuiInputRing (editor only; set by RenderThread
    // when an overlay factory is present). The stripped runtime never sets it, so PlatformThread
    // skips the ImGui-ring fan-out there instead of filling an undrained ring and log-spamming.
    std::atomic<bool> HasImGuiConsumer{false};

    // Selected entity for the editor outline pass. The editor overlay (RenderThread) writes it;
    // OutlineRenderPass (also RenderThread, earlier in the frame) reads it -> 1-frame lag.
    // INVALID_ENTITY (0) = no outline. The runtime never writes it, so it draws no outline.
    std::atomic<uint64_t> SelectedEntity{INVALID_ENTITY};

    // Editor scene-file requests (editor RenderThread sets; GameThread consumes via exchange() each
    // tick). Reload re-reads world.json from disk (replacing the world); New clears to an empty scene.
    // Runtime never sets these, so it is unaffected.
    std::atomic<bool> RequestSceneReload{false};
    std::atomic<bool> RequestSceneNew{false};

    // Dedicated-server target port the in-editor client connects to (and that the editor's
    // ServerSupervisor launched server.exe on). The Dedicated Server panel (RenderThread) writes
    // it on Start; GameThread reads it each tick into SystemContext::serverPort so NetDemoSystem's
    // client retargets. Defaults to the demo port. The runtime never writes it (stays default);
    // server.exe gets its port from --port directly, not via this field.
    std::atomic<uint16_t> NetServerPort{ kDedicatedServerDefaultPort };

    // Editor free-look camera override (editor only). The overlay (RenderThread) writes both each
    // frame; Renderer (RenderThread, earlier in the next frame) reads them -> 1-frame lag, like
    // SelectedEntity. Runtime has no overlay, never writes them, so EditorCameraActive stays false
    // and rendering uses the game's WorldCameraComponent unchanged.
    std::atomic<bool>   EditorCameraActive{false};
    Seqlock<CameraView> EditorCamera{};

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
    // Game -> Render (request meshes, material handles, uploads, etc)
    SpscRing<RendererCommand, RendererCommandRingSize> GRCommandRing{};
    // Render -> Game (responses to mesh/material requests)
    SpscRing<RendererResponse, RendererCommandRingSize> RGCommandRing{};

    // Game -> Render latest snapshot (seqlocked)
    Seqlock<SimulationSnapshot> LatestSnapshot{};

    // ECS World snapshot lifetime management (C++20 atomic shared_ptr operations)
    // GameThread: worldSnapshot = state.World.CreateSnapshot();
    //             std::atomic_store(&LatestWorldSnapshot, worldSnapshot);
    // RenderThread: auto worldSnapshot = std::atomic_load(&LatestWorldSnapshot);
    std::atomic<std::shared_ptr<const ECS>> LatestWorldSnapshot;
};
