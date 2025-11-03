#pragma once

#include <lib.h>

enum class GameStateId : uint32_t {
    Uninitialized = 0,
    MainMenu = 1,
    InLevel = 2,
    InEditor = 3,
    Paused = 4,
};

struct GameState {
    GameStateId StateId;
    double DeltaTime;
    void* PlatformInputHandle;
    void* GameOutputHandle;
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