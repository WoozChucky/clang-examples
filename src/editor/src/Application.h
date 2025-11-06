#pragma once
#include "PlatformThread.h"
#include "GameThread.h"
#include "RenderThread.h"

class Application {
public:
    Application() = default;
    ~Application() = default;

    bool Init() {
        m_AppContext = std::make_shared<ApplicationContext>();
        m_AppContext->Settings.windowWidth = 1920;
        m_AppContext->Settings.windowHeight = 1080;
        m_AppContext->Settings.vsyncEnabled = true;

        m_PlatformThread = std::make_unique<PlatformThread>(m_AppContext);
        if (!m_PlatformThread->Init()) {
            return false;
        }

        m_AppContext->LatestInputStatePtr.store(&m_AppContext->InputStateA, std::memory_order_release);

        m_GameThread = std::make_unique<GameThread>(m_AppContext);
        m_RenderThread = std::make_unique<RenderThread>(m_AppContext, m_PlatformThread->GetWindow(), RendererAPI::DirectX12);
        return true;
    }

    void Run() {

        m_ThreadGame = std::thread([this]() {
            m_GameThread->RunLoop();
            SM_TRACE("Game thread exiting...");
        });
        m_ThreadRender = std::thread([this]() {
            m_RenderThread->RunLoop();
            SM_TRACE("Render thread exiting...");
        });

        m_PlatformThread->RunMainLoop();

        m_AppContext->ShutdownRequested.store(true, std::memory_order_relaxed);

        m_GameThread->Stop();
        m_RenderThread->Stop();

        if (m_ThreadGame.joinable()) {
            m_ThreadGame.join();
        }
        if (m_ThreadRender.joinable()) {
            m_ThreadRender.join();
        }

        m_GameThread.reset();
        m_RenderThread.reset();
    }

private:
    std::shared_ptr<ApplicationContext> m_AppContext;

    // Main thread
    std::unique_ptr<PlatformThread>     m_PlatformThread;

    std::unique_ptr<GameThread>         m_GameThread;
    std::thread m_ThreadGame;

    std::unique_ptr<RenderThread>       m_RenderThread;
    std::thread m_ThreadRender;
};
