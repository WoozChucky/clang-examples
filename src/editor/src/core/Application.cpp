#include "Application.h"

bool Application::Init() {
    m_AppContext = std::make_shared<ApplicationContext>();
    m_AppContext->Settings.windowWidth = 1920;
    m_AppContext->Settings.windowHeight = 1080;
    m_AppContext->Settings.vsyncEnabled = true;

    m_PlatformThread = std::make_unique<PlatformThread>(m_AppContext);
    if (!m_PlatformThread->Init()) {
        return false;
    }

    m_GameThread = std::make_unique<GameThread>(m_AppContext);
    m_RenderThread = std::make_unique<RenderThread>(m_AppContext, m_PlatformThread->GetWindow(), RendererAPI::Vulkan);
    return true;
}

void Application::Run() {

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


    m_RenderThread->Stop();
    if (m_ThreadRender.joinable()) {
        m_ThreadRender.join();
    }

    m_GameThread->Stop();
    if (m_ThreadGame.joinable()) {
        m_ThreadGame.join();
    }

    // Clear any remaining snapshot references
    m_AppContext->LatestWorldSnapshot.store(nullptr);

    m_GameThread.reset();
    m_RenderThread.reset();
}
