#pragma once
#include "ApplicationContext.h"
#include "ECS.h"
#include "Systems.h"

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
#define GAME_API_VERSION 9u

// GameStateId moved to src/common/include/GameStateId.h (included via ECS.h) so ECS
// components + engine code can reference it.

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
};

using GameGetVersionFunc = uint32_t(*)();
using GameUpdateFunc = void(*)(GameState* state);
using GameResizeFunc = void(*)(uint32_t width, uint32_t height);
using GameExitFunc = void(*)(GameState* state);
using GameRegisterSystemsFunc = void(*)(SystemScheduler*);

extern "C"
{
    EXPORT_FN uint32_t GameGetVersion();

    EXPORT_FN void GameUpdate(GameState* state);

    EXPORT_FN void GameResize(uint32_t width, uint32_t height);

    EXPORT_FN void GameExit(GameState* state);

    EXPORT_FN void GameRegisterSystems(SystemScheduler* scheduler);
}
