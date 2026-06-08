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
#include "Skeleton.h"
#include "Skinning.h"
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
        std::string assetKey;
    };

    struct ModelLoadResult
    {
        uint64_t ticketId{0};
        std::string assetKey;
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
        Skeleton    skeleton{};
        bool        hasSkeleton{false};
        std::string skeletonKey;
        std::vector<SkinnedVertex> skinning; // per-vertex bone idx+weights, aligned to `vertices`; empty if static
    };

    void DrainInputToSingleton(GameState& state);
    void WorkerThreadFunc();
    void EnqueueModelLoadJob(uint64_t ticketId, const std::string& objPath, const std::string& mtlBaseDir, const std::string& assetKey);

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

    // This tick's raw input events, filled by DrainInputToSingleton and exposed read-only
    // via GameState::FrameInputEvents. A member so its storage is stable across ticks.
    std::vector<InputEvent> m_FrameInput;

    // Worker thread & queues
    std::thread m_Worker;
    std::atomic<bool> m_WorkerStop{false};
    std::mutex m_JobMutex;
    std::condition_variable m_JobCv;
    std::queue<ModelLoadJob> m_PendingJobs;
    std::queue<ModelLoadResult> m_CompletedJobs;

    // Monotonic ticket counter for non-entity model loads (startup .obj/.gltf scan).
    // Starts above the EntityId range so IsValidEntity(ticket) deterministically returns
    // false in the response handler — but pendingMeshData keys stay unique, fixing the
    // collision where every startup load shared INVALID_ENTITY (0) and overwrote each
    // other in the cache-pending map. Entity-owned loads keep using the EntityId.
    std::atomic<uint64_t> m_NextLoadTicket{1ULL << 48};
};
