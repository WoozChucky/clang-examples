#include "Application.h"

#include "lib.h"
#include "utilities/SettingsManager.h"
#include "RenderStats.h"

bool Application::Init(std::optional<RendererAPI> backendOverride, OverlayFactory overlayFactory) {
    m_AppContext = std::make_shared<ApplicationContext>();

    // Load persisted settings (file may not exist; defaults stay in place).
    SettingsManager::Load(SettingsManager::DEFAULT_SETTINGS_PATH, &m_AppContext->Settings);

    // CLI override wins for this run; never written to disk.
    if (backendOverride.has_value()) {
        m_AppContext->Settings.Backend = *backendOverride;
        SM_TRACE("Application: CLI override → backend=%s",
                 SettingsManager::BackendToString(*backendOverride));
    }

    // Seed the live anti-aliasing toggle from the persisted setting (both exes
    // boot through here; the RenderThread reads GetAntiAliasingSettings()).
    GetAntiAliasingSettings().Mode = static_cast<AAMode>(m_AppContext->Settings.aaMode);

    // Seed the live shadow settings from the persisted engine-tier values (both exes boot here;
    // the RenderThread reads GetShadowSettings()).
    {
        ShadowSettings& sh = GetShadowSettings();
        sh.Enabled      = m_AppContext->Settings.shadowEnabled;
        sh.ShadowDistance = m_AppContext->Settings.shadowDistance;
        sh.NearExtend   = m_AppContext->Settings.shadowNearExtend;
        sh.NormalOffset = m_AppContext->Settings.shadowNormalOffset;
        sh.PcfRadius    = m_AppContext->Settings.shadowPcfRadius;
    }
    {
        SsaoSettings& ao = GetSsaoSettings();
        ao.Enabled   = m_AppContext->Settings.ssaoEnabled;
        ao.Radius    = m_AppContext->Settings.ssaoRadius;
        ao.Intensity = m_AppContext->Settings.ssaoIntensity;
        ao.Power     = m_AppContext->Settings.ssaoPower;
        ao.Bias      = m_AppContext->Settings.ssaoBias;
    }

    if (m_AppContext->Settings.Backend == RendererAPI::Invalid) {
        SM_ERROR("Application: resolved backend is Invalid; aborting");
        return false;
    }
    if (m_AppContext->Settings.Backend == RendererAPI::DirectX11) {
        SM_ERROR("Application: DirectX11 backend is not implemented; aborting");
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
        m_AppContext->Settings.Backend,
        std::move(overlayFactory));
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
