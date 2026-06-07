#include "ServerApplication.h"

#include <chrono>
#include <filesystem>
#include <regex>
#include <thread>

#include "FileWatch.h"   // match GameThread.h's filewatch include

#include "lib.h"
#include "Timing.h"
#include "WorldManager.h"
#include "ServerControl.h"   // kDedicatedServerDefaultPort (port guard + bind plumbing)
#include "utilities/SettingsManager.h"
#include "navigation/NavMeshSystem.h"
#include "navigation/NavServicesImpl.h"
#include "network/NetSubsystem.h"
#include "network/NetServicesImpl.h"
#include "ECSCommands.h"

using Clock = std::chrono::steady_clock;

ServerApplication::~ServerApplication() { Shutdown(); }

bool ServerApplication::Init(const Config& cfg) {
    if (m_Initialized) return true;
    m_Config = cfg;
    if (m_Config.targetTps <= 0.0) {
        SM_WARN("ServerApplication: invalid targetTps %.2f; resetting to 60", m_Config.targetTps);
        m_Config.targetTps = 60.0;
    }
    if (m_Config.port == 0) {
        SM_WARN("ServerApplication: port 0; using default %u", (unsigned)kDedicatedServerDefaultPort);
        m_Config.port = kDedicatedServerDefaultPort;
    }
    if (m_Config.worldPath.empty())
        m_Config.worldPath = WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH;

    m_AppContext = std::make_shared<ApplicationContext>();
    SettingsManager::Load(SettingsManager::DEFAULT_SETTINGS_PATH, &m_AppContext->Settings);

    m_GameState.Settings = &m_AppContext->Settings;
    m_GameState.Role     = AppRole::Server;
    m_GameState.TargetTPS = m_Config.targetTps;

    m_GameState.World.SetSingleton(InputStateComponent{});

    NavServicesImpl::Init(m_NavServices);
    NetServicesImpl::Init(m_NetServices);
    NetSubsystem::Instance().Init();

    m_GameLib.SetScheduler(&m_Scheduler);
    if (!m_GameLib.LoadOrReload("Game.dll", &m_GameState)) {
        SM_ERROR("ServerApplication: initial Game.dll load failed; server will idle without game logic");
    }

    // Load the world AFTER LoadOrReload so GameRegisterComponents has registered
    // game-owned component serializers first — otherwise LoadEntityComponents
    // drops unregistered game components. LoadOrReload must also stay after the
    // service inits above (game system ctors may need them).
    if (WorldManager::LoadWorldSnapshot(m_Config.worldPath, &m_GameState.World)) {
        m_GameState.WorldLoaded = true;
        SM_TRACE("ServerApplication: world loaded from '%s'", m_Config.worldPath.c_str());
        if (!NavMeshSystem::Instance().TryLoadFromDisk(m_Config.worldPath)) {
            SM_WARN("ServerApplication: no fresh disk navmesh for '%s'; running without nav "
                    "(headless server cannot rebake mesh-source geometry)", m_Config.worldPath.c_str());
        }
    } else {
        SM_WARN("ServerApplication: world '%s' not loaded (missing/invalid)", m_Config.worldPath.c_str());
    }

    InstallFilewatch();

    m_Initialized = true;
    SM_TRACE("ServerApplication: initialized (role=Server, port=%u)", (unsigned)m_Config.port);
    return true;
}

bool ServerApplication::InstallFilewatch() {
    try {
        m_Watcher = new filewatch::FileWatch<std::string>(
            std::string("."),
            std::regex(R"(^Game\.dll$)"),
            [this](const std::string&, const filewatch::Event evt) {
                if (evt == filewatch::Event::modified || evt == filewatch::Event::added)
                    m_ReloadPending.store(true, std::memory_order_release);
            });
        SM_TRACE("ServerApplication: filewatch installed on './Game.dll'");
        return true;
    } catch (const std::exception& e) {
        SM_ERROR("ServerApplication: filewatch setup failed: %s. Hot-reload disabled.", e.what());
        return false;
    }
}

void ServerApplication::Tick() {
    if (!m_Initialized) return;

    if (m_ReloadPending.exchange(false, std::memory_order_acquire)) {
        NetSubsystem::Instance().ReleaseGameResidentConnections();
        if (m_GameLib.LoadOrReload("Game.dll", &m_GameState))
            SM_TRACE("ServerApplication: Game.dll reloaded");
    }

    const double dt = 1.0 / m_Config.targetTps;
    m_GameState.DeltaTime = dt;
    m_GameState.GameTime  = TimeNowSec();

    if (m_GameLib.IsValid())
        m_GameLib.Update(&m_GameState);

    SystemContext ctx{ m_GameState.World, m_GameState.DeltaTime, m_GameState.GameTime,
                       &m_NavServices, &m_NetServices, m_GameState.Role, m_Config.port };
    m_Scheduler.Run(ctx);

    NavMeshSystem::Instance().Tick(static_cast<float>(dt));

    if (const auto* app = m_GameState.World.GetSingleton<AppControlComponent>(); app && app->QuitRequested)
        RequestShutdown();
}

void ServerApplication::Run() {
    if (!m_Initialized) { SM_ERROR("ServerApplication::Run before Init"); return; }
    SM_TRACE("ServerApplication: entering run loop (%.0f TPS)", m_Config.targetTps);

    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / m_Config.targetTps));
    auto next = Clock::now();

    while (!m_StopRequested.load(std::memory_order_relaxed)) {
        Tick();
        next += period;
        const auto now = Clock::now();
        if (now < next) std::this_thread::sleep_for(next - now);
        else            next = now;
    }
    SM_TRACE("ServerApplication: run loop exited");
}

void ServerApplication::Shutdown() {
    if (!m_Initialized) return;
    m_Initialized = false;

    if (m_Watcher) {
        delete static_cast<filewatch::FileWatch<std::string>*>(m_Watcher);
        m_Watcher = nullptr;
    }

    NetSubsystem::Instance().Shutdown();   // BEFORE GameLib.Unload (ordering gotcha)

    m_GameLib.Unload(&m_GameState);
    m_GameState.World.Clear();
    SM_TRACE("ServerApplication: shutdown complete");
}
