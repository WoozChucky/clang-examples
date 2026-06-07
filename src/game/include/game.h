#pragma once
#include "ApplicationContext.h"
#include "input.h"
#include <cstddef>
#include "ECS.h"
#include "Systems.h"
#include "AppRole.h"
#include "GameStates.h"

#ifdef _WIN32
#define DEBUG_BREAK() __debugbreak()
#define EXPORT_FN __declspec(dllexport)
#elif __linux__
#define DEBUG_BREAK() __builtin_debugtrap()
#define EXPORT_FN
#elif __APPLE__
#define DEBUG_BREAK() __builtin_trap()
#define EXPORT_FN
#endif

// Bump every time GameState layout changes or any export signature changes.
// Editor compares against the compiled-in value at load time; mismatch rejects
// the reload and keeps the previous Game.dll active.
#define GAME_API_VERSION 22u

// GameStateId lives in the game-owned GameStates.h (next to this header). The engine
// stores the current state as an opaque uint32_t (GameStateComponent.Current).

struct ApplicationSettings;

struct GameState {
    GameStateId StateId = GameStateId::Uninitialized;
    double GameTime = 0.0;
    double DeltaTime = 0.0;
    double TargetTPS = 60.0;  // Intended tick rate
    double ActualTPS = 0.0;   // Measured actual tick rate (work time only)
    void* GameOutputHandle = nullptr;
    const ApplicationSettings* Settings = nullptr;
    ECS World{};
    bool     WorldLoaded            = false;
    AppRole  Role                   = AppRole::Client;  // set by bootstrap; Server only in server.exe

    // Raw input events for THIS tick (incl TextInput), in arrival order, set by the engine
    // before GameUpdate; valid only for the current tick (Count 0 => none). The digested
    // convenience state still lives in the InputStateComponent singleton; this is for
    // consumers that need raw text/keys (e.g. text fields). Read via g_GameState in systems.
    const InputEvent* FrameInputEvents     = nullptr;
    std::size_t       FrameInputEventCount = 0;
};

using GameGetVersionFunc = uint32_t(*)();
using GameUpdateFunc = void(*)(GameState* state);
using GameResizeFunc = void(*)(uint32_t width, uint32_t height);
using GameExitFunc = void(*)(GameState* state);
using GameRegisterSystemsFunc = void(*)(SystemScheduler*);
using GameRegisterComponentsFunc = void(*)();

extern "C"
{
    EXPORT_FN uint32_t GameGetVersion();

    EXPORT_FN void GameUpdate(GameState* state);

    EXPORT_FN void GameResize(uint32_t width, uint32_t height);

    EXPORT_FN void GameExit(GameState* state);

    EXPORT_FN void GameRegisterSystems(SystemScheduler* scheduler);

    // Re-register game-owned ECS component types (serializers + reload-preservation strategies).
    // Called by the host after every (re)load, before reload-preserved components are restored.
    EXPORT_FN void GameRegisterComponents();
}
