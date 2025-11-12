#pragma once

#include <Camera.h>
#include <Input.h>
#include <SpscRing.h>
#include <Seqlock.h>

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

    // Platform -> Render (Stuff like pause, vsync, resize)
    static constexpr int RendererCommandRingSize = 16;
    SpscRing<RendererCommand, RendererCommandRingSize> RendererCommandRing{};

    // Late-latched input sample: Platform -> Render
    std::atomic<InputState*> LatestInputStatePtr{nullptr};
    InputState InputStateA{}, InputStateB{};

    // Game -> Render latest snapshot (seqlocked)
    Seqlock<SimulationSnapshot> LatestSnapshot{};
};