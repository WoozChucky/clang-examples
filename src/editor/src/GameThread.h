#pragma once

#include <thread>

#include "ApplicationContext.h"
#include "Game.h"
#include "Timing.h"
#include "GLFW/glfw3.h"


using Library = void*;

struct GameLibrary {
	Library Handle{ nullptr };
	GameGetVersionFunc GetVersion{ nullptr };
	GameSetPlatformDebugBreakFunc SetPlatformDebugBreak{ nullptr };
	GameUpdateFunc Update{ nullptr };
	GameResizeFunc Resize{ nullptr };
	GameExitFunc ExitGame{ nullptr };

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

	GameLibrary LoadGameLibrary(std::string_view libraryName);
    void FreeGameLibrary();

    std::shared_ptr<ApplicationContext> m_AppContext;
    std::atomic<bool> m_Running;
    std::atomic<bool> m_LibraryReload{ false };
    uint64_t m_TickCounter;
    float m_simX{0.0f};
    float m_simVX{0.5f};

	GameLibrary m_GameLib;
};
