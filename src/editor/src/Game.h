#pragma once

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

#include "Camera.h"

enum class GameStateId : uint32_t {
    Uninitialized = 0,
    MainMenu = 1,
    InLevel = 2,
    InEditor = 3,
    Paused = 4,
};

struct ApplicationSettings;

struct GameState {
    GameStateId StateId = GameStateId::Uninitialized;
    double DeltaTime = 0.0;
    void* PlatformInputHandle = nullptr;
    void* GameOutputHandle = nullptr;
    const ApplicationSettings* Settings = nullptr;
    bool QuitRequested = false;
    PerspectiveCamera3D GameCamera {};
    OrthographicCamera2D UICamera {};
};

using GameGetVersionFunc = uint32_t(*)();
using GameDebugBreakFn = void(*)(const char* expr, const char* file, int line, const char* message);
using GameSetPlatformDebugBreakFunc = void(*)(GameDebugBreakFn);
using GameUpdateFunc = void(*)(GameState* state);
using GameResizeFunc = void(*)(uint32_t width, uint32_t height);
using GameExitFunc = void(*)();

extern "C"
{
    EXPORT_FN uint32_t GameGetVersion();

    EXPORT_FN void GameSetPlatformDebugBreak(GameDebugBreakFn fn);

    EXPORT_FN void GameUpdate(GameState* state);

    EXPORT_FN void GameResize(uint32_t width, uint32_t height);

    EXPORT_FN void GameExit();
}