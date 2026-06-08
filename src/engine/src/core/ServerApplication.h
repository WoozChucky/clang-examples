#pragma once
#include <atomic>
#include <memory>
#include <string>

#include "Engine.h"
#include "ApplicationContext.h"
#include "GameLibrary.h"
#include "Systems.h"        // SystemScheduler
#include "Game.h"           // GameState

class ENGINE_API ServerApplication {
public:
    struct Config {
        uint16_t    port = 0;
        std::string worldPath;
        double      targetTps = 60.0;
    };

    ServerApplication() = default;
    ~ServerApplication();

    ServerApplication(const ServerApplication&) = delete;
    ServerApplication& operator=(const ServerApplication&) = delete;

    bool Init(const Config& cfg);
    void Tick();
    void Run();
    void RequestShutdown() { m_StopRequested.store(true, std::memory_order_relaxed); }
    bool StopRequested() const { return m_StopRequested.load(std::memory_order_relaxed); }
    void Shutdown();

private:
    bool InstallFilewatch();

    std::shared_ptr<ApplicationContext> m_AppContext;
    GameLibrary       m_GameLib;
    SystemScheduler   m_Scheduler;
    GameState         m_GameState{};
    Config            m_Config{};

    NavServices       m_NavServices{};
    NetServices       m_NetServices{};
    AnimServices      m_AnimServices{};

    std::atomic<bool> m_StopRequested{false};
    std::atomic<bool> m_ReloadPending{false};
    bool              m_Initialized = false;

    void*             m_Watcher = nullptr;
};
