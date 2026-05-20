#include "Application.h"

#include "lib.h"
#include "utilities/SettingsManager.h"

bool Application::Init(std::optional<RendererAPI> backendOverride) {
    m_AppContext = std::make_shared<ApplicationContext>();

    // Load persisted settings (file may not exist; defaults stay in place).
    SettingsManager::Load(SettingsManager::DEFAULT_SETTINGS_PATH, &m_AppContext->Settings);

    // CLI override wins for this run; never written to disk.
    if (backendOverride.has_value()) {
        m_AppContext->Settings.Backend = *backendOverride;
        SM_TRACE("Application: CLI override → backend=%s",
                 SettingsManager::BackendToString(*backendOverride));
    }

    if (m_AppContext->Settings.Backend == RendererAPI::Invalid) {
        SM_ERROR("Application: resolved backend is Invalid; aborting");
        return false;
    }

    m_PlatformThread = std::make_unique<PlatformThread>(m_AppContext);
    if (!m_PlatformThread->Init()) {
        return false;
    }

    m_GameThread = std::make_unique<GameThread>(m_AppContext);
    m_RenderThread = std::make_unique<RenderThread>(
        m_AppContext,
        m_PlatformThread->GetWindow(),
        m_AppContext->Settings.Backend);
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
