#pragma once

#include "SpscRing.h"
#include "Seqlock.h"

struct InputEvent {
    enum Type : uint8_t { MouseMove = 0, MouseButton = 1, Key = 2 } TypeId;
    double Time; // seconds
    double MouseX, MouseY;
    int Button; // or Keycode
    int Action;
    int Mods;
};

struct UiCommand {
    enum Type : uint8_t { SetVelocity = 0, TogglePause = 1 } type{};
    float fval{}; bool bval{};
};

struct SimulationSnapshot {
    uint64_t Tick;      // monotonic tick id
    double Timestamp;   // seconds at tick start
    // Minimal renderable state: a single float position for demo (x in [-1..1])
    float ObjectX;
    float ObjectVX;     // velocity for possible extrapolation
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
    // Shutdown
    std::atomic<bool> ShutdownRequested{false};

    // Input: Platform -> Game
    static constexpr int InputRingSize = 256;
    SpscRing<InputEvent, InputRingSize> InputRing{};

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