#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <string>

#include "ApplicationContext.h"
#include "DotNetPluginManager.h"
#include "FileWatch.h"
#include "GameLibrary.h"
#include "Systems.h"
#include "GLFW/glfw3.h"

class GameThread {
public:
    explicit GameThread(const std::shared_ptr<ApplicationContext> &appContext);

    void RunLoop();

    void Stop();

private:
    bool Running() const;
    // Main loop helpers
    void SimulateStep(double dt);
    void PublishSnapshot(GameState& state, const FrameTimeStats& frameStats);

    // Background job system (minimal): single worker for model loading
    struct ModelLoadJob
    {
        uint64_t ticketId{0};
        std::string objPath;
        std::string mtlBaseDir;
    };

    struct ModelLoadResult
    {
        uint64_t ticketId{0};
        bool success{false};
        std::string error;
        std::vector<MeshVertex> vertices{};
        std::vector<uint32_t> indices{};
        std::vector<SubMesh> subMeshes{}; // will contain elements if model has multiple sub-meshes
        // For now only 1 material is supported for the whole model, we will expand on this later
        uint32_t Width{0};
        uint32_t Height{0};
        uint32_t* Texture{nullptr}; // optional RGBA8 pixels (w*h entries)
        bool MeshUploaded{false};   // set once the mesh command is on the ring; gates retry re-upload
    };

    void DrainInputToSingleton(GameState& state);
    void WorkerThreadFunc();
    void EnqueueModelLoadJob(uint64_t ticketId, const std::string& objPath, const std::string& mtlBaseDir);

    std::unique_ptr<DotNetPluginManager> m_PluginManager{nullptr};
    SystemScheduler m_Scheduler;   // declared before m_GameLib: GameLibrary's dtor
                                   // clears the scheduler, so the scheduler must outlive it
    GameLibrary m_GameLib;
    std::atomic<bool> m_ReloadPending{false};
    std::unique_ptr<filewatch::FileWatch<std::string>> m_GameDllWatcher;

    std::shared_ptr<ApplicationContext> m_AppContext;
    std::atomic<bool> m_Running;
    uint64_t m_TickCounter;
    float m_simX{0.0f};
    float m_simVX{0.5f};

    // Worker thread & queues
    std::thread m_Worker;
    std::atomic<bool> m_WorkerStop{false};
    std::mutex m_JobMutex;
    std::condition_variable m_JobCv;
    std::queue<ModelLoadJob> m_PendingJobs;
    std::queue<ModelLoadResult> m_CompletedJobs;
};
