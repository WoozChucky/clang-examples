#pragma once

#include <thread>

#include "ApplicationContext.h"
#include "Game.h"
#include "Timing.h"
#include "GLFW/glfw3.h"


using Library = void*;

struct GameLibrary {
    Library Handle;
    GameGetVersionFunc GetVersion;
    GameSetPlatformDebugBreakFunc SetPlatformDebugBreak;
    GameUpdateFunc Update;
    GameResizeFunc Resize;
    GameExitFunc ExitGame;

    [[nodiscard]] bool IsValid() const {
        return Handle != nullptr
            && GetVersion != nullptr
            && SetPlatformDebugBreak != nullptr
            && Update != nullptr
            && Resize != nullptr
            && ExitGame != nullptr;
    }
};

class GameThread {
public:
    explicit GameThread(const std::shared_ptr<ApplicationContext> &appContext);

    void RunLoop();

    void Stop();

private:
    void ProcessInput();

    void SimulateStep(double dt);

    void PublishSnapshot();

    std::shared_ptr<ApplicationContext> m_AppContext;
    std::atomic<bool> m_Running;
    uint64_t m_TickCounter;
    float m_simX{0.0f};
    float m_simVX{0.5f};
};
