#pragma once
#include "PlatformThread.h"
#include "GameThread.h"
#include "RenderThread.h"

class Application {
public:
    Application() = default;
    ~Application() = default;

    bool Init();

    void Run();

private:
    std::shared_ptr<ApplicationContext> m_AppContext;

    // Main thread
    std::unique_ptr<PlatformThread>     m_PlatformThread;

    std::unique_ptr<GameThread>         m_GameThread;
    std::thread m_ThreadGame;

    std::unique_ptr<RenderThread>       m_RenderThread;
    std::thread m_ThreadRender;
};
