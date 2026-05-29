#include "GameThread.h"
#include "StagingBufferPool.h"

#include <windows.h>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <thread>
#include <chrono>
#include <regex>
#include <unordered_map>

#ifdef _WIN32
#include <timeapi.h>  // For timeBeginPeriod/timeEndPeriod
#include <intrin.h>   // For _mm_pause
#pragma comment(lib, "winmm.lib")  // Link against winmm.lib for timeBeginPeriod
#endif

#include <GLFW/glfw3.h>
#include <tracy/Tracy.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "tiny_obj_loader.h"

#include "lib.h"
#include "MaterialLoader.h"
#include "Timing.h"
#include "assimp/scene.h"
#include "WorldManager.h"
#include "navigation/NavMeshSystem.h"
#include "navigation/NavServicesImpl.h"

using namespace std::chrono_literals;

GameThread::GameThread(const std::shared_ptr<ApplicationContext> &appContext)
 : m_AppContext(appContext), m_Running(true), m_TickCounter(1) {
	 // Publish an initial snapshot so the render thread has something to read.
	 SimulationSnapshot init{};
	 init.Tick =0;
	 init.Timestamp = TimeNowSec();
	 init.ObjectX =0.0f;
	 init.ObjectVX =0.5f;
	 m_AppContext->LatestSnapshot.store(init);
}

void GameThread::RunLoop() {
    tracy::SetThreadName("GameThread");

	// Increase Windows timer resolution for more accurate sleep
	// This improves sleep_for/sleep_until from ~15ms to ~1ms granularity
	#ifdef _WIN32
	timeBeginPeriod(1);
	#endif

	// Initialize plugin system
	m_PluginManager = std::make_unique<DotNetPluginManager>();
	if (m_PluginManager->Initialize("assets/plugins/PluginCore.runtimeconfig.json")) {
		m_PluginManager->LoadPluginsFromDirectory("assets/plugins");
	}

    GameState gameState{};
    gameState.Settings = &m_AppContext->Settings;

    // Load default world before any GameUpdate call. Guarded by WorldLoaded so
    // reload (which doesn't reconstruct GameState) doesn't reload the world.
    if (!gameState.WorldLoaded) {
        if (WorldManager::LoadWorldSnapshot(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH, &gameState.World)) {
            gameState.WorldLoaded = true;
            SM_TRACE("GameThread: default world loaded from '%s'", WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH);

            // Try the disk bake first; fall back to a runtime Rebuild on miss/stale/corrupt.
            // TryLoadFromDisk now handles any class count (multi-class container bake), so the
            // previous ClassCount>1 bypass is gone.
            if (!NavMeshSystem::Instance().TryLoadFromDisk(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH)) {
                if (!m_AppContext->ECSCommandRing.Push(ECSCommand::RebuildNavMesh())) {
                    SM_WARN("GameThread: ECSCommandRing full when posting initial RebuildNavMesh");
                }
            }
        } else {
            SM_WARN("GameThread: default world '%s' not loaded (file missing or invalid)", WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH);
        }
    }

    // Seed singleton components so systems and game code can always find them.
    gameState.World.SetSingleton(InputStateComponent{});
    gameState.World.SetSingleton(ViewportComponent{
        m_AppContext->Settings.windowWidth, m_AppContext->Settings.windowHeight });
    gameState.World.SetSingleton(UICameraComponent{});

    // Initial load of Game.dll. If it fails, editor still runs without game logic
    // until the file watcher (installed in T14) picks up a subsequent rebuild.
    m_GameLib.SetScheduler(&m_Scheduler);
    if (!m_GameLib.LoadOrReload("Game.dll", &gameState)) {
        SM_ERROR("GameThread: initial Game.dll load failed. "
                 "Editor will run without game logic until Game.dll becomes loadable.");
    }

    // File watcher: detects Game.dll changes on a background thread.
    // Callback sets m_ReloadPending; GameThread drains it at the top of each tick.
    // CWD at runtime is RUNTIME_DIR (set via VS_DEBUGGER_WORKING_DIRECTORY in CMake).
    try {
        m_GameDllWatcher = std::make_unique<filewatch::FileWatch<std::string>>(
            std::string("."),                       // watch CWD = RUNTIME_DIR
            std::regex(R"(^Game\.dll$)"),           // exact filename match
            [this](const std::string& /*file*/, const filewatch::Event evt) {
                if (evt == filewatch::Event::modified ||
                    evt == filewatch::Event::added) {
                    m_ReloadPending.store(true, std::memory_order_release);
                }
            });
        SM_TRACE("GameThread: filewatch installed on './Game.dll'");
    } catch (const std::exception& e) {
        SM_ERROR("GameThread: filewatch setup failed: %s. Hot-reload disabled.", e.what());
    }

    // Start background worker for model loading
    m_WorkerStop.store(false, std::memory_order_relaxed);
    m_Worker = std::thread(&GameThread::WorkerThreadFunc, this);

    {
        // Loop trough all *.obj files in assets/models and enqueue load jobs
        const std::string modelDir = "assets/models";
        for (const auto& entry : std::filesystem::directory_iterator(modelDir)) {
            if (entry.is_regular_file() && (entry.path().extension() == ".obj") || (entry.path().extension() == ".gltf")) {
                const std::string objPath = entry.path().string();
                const std::string mtlBaseDir = entry.path().parent_path().string();
                // Unique non-entity ticket per startup load — prevents pendingMeshData
                // key collision in the Spec 5 mesh-cache flow. IsValidEntity stays false.
                EnqueueModelLoadJob(m_NextLoadTicket.fetch_add(1, std::memory_order_relaxed),
                                    objPath, mtlBaseDir);
            }
        }
    }

	// Initialize default settings
	GameThreadSettings threadSettings{};
	threadSettings.TargetTPS = 60.0;
	threadSettings.SpinThresholdMicros = 500;
	threadSettings.EnableFrameTimeTracking = true;
	m_AppContext->GameThreadConfig.store(threadSettings);

	double targetDt = 1.0 / threadSettings.TargetTPS;
	gameState.TargetTPS = threadSettings.TargetTPS;

	auto nextFrameTime = Clock::now();
	auto lastFrameTime = nextFrameTime;

	// TPS tracking variables
	auto tpsStartTime = Clock::now();
	uint64_t tickCount = 0;
	constexpr double tpsUpdateInterval = 1.0; // Update ActualTPS every second

	// Frame time tracking
	FrameTimeStats frameStats{};
	frameStats.MinFrameTimeMs = 1000.0; // Initialize to high value
	frameStats.MaxFrameTimeMs = 0.0;
	frameStats.AvgFrameTimeMs = 0.0;
	frameStats.SampleCount = 0;

    // NavServices function-pointer table — engine-provided nav API surface
    // that game systems (NavObstacleSync, NavAgentSystem) call through
    // instead of NavMeshSystem::Instance() directly. Restores Game.dll's
    // GameState-only boundary (Game.dll no longer links Engine.dll).
    NavServices navServices{};
    NavServicesImpl::Init(navServices);

    // Spec 5: per-MeshId verts/indices in flight from the mesh-upload command
    // (GameThread sends to RenderThread) → the MeshUpload response (RenderThread
    // sends back). Keyed by ticketId (entityId) at upload time, transferred to
    // NavMeshSystem's MeshId-keyed cache when the response arrives. Function-
    // scope: lives for the entire RunLoop, sole owner is GameThread.
    struct PendingMeshData {
        std::vector<MeshVertex> Vertices;
        std::vector<uint32_t>   Indices;
    };
    std::unordered_map<EntityId, PendingMeshData> pendingMeshData;

	while (Running()) {
		// Renderer hot-swap: pause here while RenderThread rebuilds the device.
		if (m_AppContext->SwapInProgress.load(std::memory_order_acquire)) {
			ZoneScopedN("Game:SwapPause");
			m_AppContext->GameThreadPaused.store(true, std::memory_order_release);
			while (m_AppContext->SwapInProgress.load(std::memory_order_acquire)
			       && Running()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			m_AppContext->GameThreadPaused.store(false, std::memory_order_release);
			// Reset frame pacing so the resumed tick doesn't see a huge dt.
			nextFrameTime = Clock::now();
			lastFrameTime = nextFrameTime;
		}

		// Drain reload flag BEFORE input/commands/game logic so the rest of the tick
		// runs on the new code.
		if (m_ReloadPending.exchange(false, std::memory_order_acquire)) {
			ZoneScopedN("Game:Reload");
			if (m_GameLib.LoadOrReload("Game.dll", &gameState)) {
				SM_TRACE("GameThread: Game.dll reloaded successfully");
			}
			// On failure, GameLibrary already logged and kept the previous module.
		}

		// Read latest settings from render thread
		const GameThreadSettings currentSettings = m_AppContext->GameThreadConfig.load();

		// Update target if changed
		if (currentSettings.TargetTPS != threadSettings.TargetTPS) {
			threadSettings = currentSettings;
			targetDt = 1.0 / threadSettings.TargetTPS;
			gameState.TargetTPS = threadSettings.TargetTPS;
			// Reset timing to avoid jumps
			nextFrameTime = Clock::now();
			lastFrameTime = nextFrameTime;
		}

		const auto frameStart = Clock::now();
		const double actualDt = std::chrono::duration<double>(frameStart - lastFrameTime).count();
	 	lastFrameTime = frameStart;

		// Track frame time variance
		if (threadSettings.EnableFrameTimeTracking) {
			const double frameTimeMs = actualDt * 1000.0;
			frameStats.MinFrameTimeMs = std::min(frameStats.MinFrameTimeMs, frameTimeMs);
			frameStats.MaxFrameTimeMs = std::max(frameStats.MaxFrameTimeMs, frameTimeMs);
			// Exponential moving average
			const double alpha = 0.05; // Smoothing factor
			if (frameStats.SampleCount == 0) {
				frameStats.AvgFrameTimeMs = frameTimeMs;
			} else {
				frameStats.AvgFrameTimeMs = alpha * frameTimeMs + (1.0 - alpha) * frameStats.AvgFrameTimeMs;
			}
			frameStats.SampleCount++;
		}

        {
            ZoneScopedN("Game:FixedUpdate");

            // Process ECS commands from RenderThread (ImGui modifications)
            {
                ZoneScopedN("Game:ProcessECSCommands");
                // Engine-side hook: ECSCommandType::RebuildNavMesh dispatches here.
                // Geometry=Mesh entities pull CPU data from NavMeshSystem's mesh
                // cache (populated by GameThread when MeshUpload responses arrive
                // from RenderThread); cache miss → SM_WARN + skip in NavMeshBuilder.
                ECSCommandHooks hooks;
                hooks.OnRebuildNavMesh = [](ECS& w) {
                    const auto* cfg = w.GetSingleton<NavMeshConfigComponent>();
                    NavMeshConfigComponent defaultCfg{};
                    NavMeshSystem::Instance().Rebuild(w, cfg ? *cfg : defaultCfg);
                };
                hooks.OnBakeNavMesh = [](ECS&) {
                    // Spec 4: editor 'Bake to Disk' button. Saves the currently-published
                    // NavMesh without rerunning Recast. World path is set by WorldManager
                    // after LoadWorldSnapshot — empty path → SM_WARN inside SaveCurrentToDisk.
                    NavMeshSystem::Instance().SaveCurrentToDisk();
                };
                ECSCommandProcessor::ProcessCommands(gameState.World, m_AppContext->ECSCommandRing, hooks);
            }

            // Editor scene-file requests (fire-and-forget atomics from the File menu).
            if (m_AppContext->RequestSceneNew.exchange(false, std::memory_order_relaxed)) {
                const std::vector<EntityId> ids = gameState.World.GetActiveEntities(); // copy before destroy
                for (EntityId e : ids) gameState.World.DestroyEntity(e);
                SM_TRACE("GameThread: new (empty) scene");
            }
            if (m_AppContext->RequestSceneReload.exchange(false, std::memory_order_relaxed)) {
                if (WorldManager::LoadWorldSnapshot(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH, &gameState.World)) {
                    SM_TRACE("GameThread: reloaded world from '%s'", WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH);
                } else {
                    SM_WARN("GameThread: reload failed for '%s'", WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH);
                }
            }

            // Drain completed background model loads and forward to Render via GR ring
            {
                ZoneScopedN("Game:ProcessCompletedModelLoads");
                // Move completed results to a local queue to minimize lock time
                std::queue<ModelLoadResult> local;
                {
                    std::scoped_lock lg(m_JobMutex);
                    while (!m_CompletedJobs.empty()) {
                        local.push(std::move(m_CompletedJobs.front()));
                        m_CompletedJobs.pop();
                    }
                }

                auto requeueAndStop = [this](ModelLoadResult& cur,
                                             std::queue<ModelLoadResult>& remaining) {
                    std::scoped_lock lg(m_JobMutex);
                    m_CompletedJobs.push(std::move(cur));
                    while (!remaining.empty()) {
                        m_CompletedJobs.push(std::move(remaining.front()));
                        remaining.pop();
                    }
                };

                while (!local.empty())
                {
                    ModelLoadResult res = std::move(local.front());
                    local.pop();

                    if (!res.success)
                    {
                        SM_ERROR("Model load failed for ticket %llu: %s", (unsigned long long)res.ticketId, res.error.c_str());
                        if (res.Texture) { GetStagingPool().Return(res.Texture); res.Texture = nullptr; }
                        continue;
                    }

                    if (!res.MeshUploaded)
                    {
                        // Send mesh upload command
                        RendererCommand meshCmd{};
                        meshCmd.Type = RendererCommandType::RequestMesh;
                        meshCmd.TicketId = res.ticketId; // Use entity ID as ticket
                        meshCmd.MeshRequest.VertexCount = res.vertices.size();
                        meshCmd.MeshRequest.IndexCount = res.indices.size();
                        meshCmd.MeshRequest.SubMeshCount = res.subMeshes.size() > 1 ? res.subMeshes.size() : 0;
                        meshCmd.MeshRequest.Vertices = nullptr;
                        meshCmd.MeshRequest.Indices = nullptr;
                        meshCmd.MeshRequest.SubMeshes = nullptr;

                        if (meshCmd.MeshRequest.VertexCount > 0)
                        {
                            meshCmd.MeshRequest.Vertices = static_cast<MeshVertex*>(GetStagingPool().Acquire(meshCmd.MeshRequest.VertexCount * sizeof(MeshVertex)));
                            std::memcpy(meshCmd.MeshRequest.Vertices, res.vertices.data(), meshCmd.MeshRequest.VertexCount * sizeof(MeshVertex));
                        }
                        if (meshCmd.MeshRequest.IndexCount > 0)
                        {
                            meshCmd.MeshRequest.Indices = static_cast<uint32_t*>(GetStagingPool().Acquire(meshCmd.MeshRequest.IndexCount * sizeof(uint32_t)));
                            std::memcpy(meshCmd.MeshRequest.Indices, res.indices.data(), meshCmd.MeshRequest.IndexCount * sizeof(uint32_t));
                        }
                        if (meshCmd.MeshRequest.SubMeshCount > 0)
                        {
                            meshCmd.MeshRequest.SubMeshes = static_cast<SubMesh*>(GetStagingPool().Acquire(meshCmd.MeshRequest.SubMeshCount * sizeof(SubMesh)));
                            std::memcpy(meshCmd.MeshRequest.SubMeshes, res.subMeshes.data(), meshCmd.MeshRequest.SubMeshCount * sizeof(SubMesh));
                        }

                        if (!m_AppContext->GRCommandRing.Push(meshCmd))
                        {
                            SM_WARN("GRCommandRing full, retrying mesh upload next frame (ticket %llu)", (unsigned long long)res.ticketId);
                            if (meshCmd.MeshRequest.Vertices) GetStagingPool().Return(meshCmd.MeshRequest.Vertices);
                            if (meshCmd.MeshRequest.Indices) GetStagingPool().Return(meshCmd.MeshRequest.Indices);
                            if (meshCmd.MeshRequest.SubMeshes) GetStagingPool().Return(meshCmd.MeshRequest.SubMeshes);
                            requeueAndStop(res, local);
                            break;
                        }
                        res.MeshUploaded = true; // mesh is on the ring; a retry must not re-upload it

                        // Spec 5: stash verts/indices keyed by ticketId. When the MeshUpload
                        // response arrives with the assigned MeshId, we transfer ownership
                        // into NavMeshSystem's cache (see ProcessRenderResponses below).
                        // Vectors are no longer needed by the upload path (staging-pool
                        // memcpy already happened above).
                        pendingMeshData[res.ticketId] = PendingMeshData{
                            std::move(res.vertices),
                            std::move(res.indices)
                        };
                    }

                    if (!res.Texture)
                        continue; // No texture to upload

                    // Send material upload command
                    RendererCommand materialCmd{};
                    materialCmd.Type = RendererCommandType::RequestMaterial;
                    materialCmd.TicketId = res.ticketId; // Same ticket ID to associate with entity
                    materialCmd.MaterialRequest.Width = res.Width;
                    materialCmd.MaterialRequest.Height = res.Height;
                    materialCmd.MaterialRequest.Texture = res.Texture;

                    if (!m_AppContext->GRCommandRing.Push(materialCmd))
                    {
                        SM_WARN("GRCommandRing full, retrying material upload next frame (ticket %llu)", (unsigned long long)res.ticketId);
                        // Keep res.Texture (the only copy) for the retry; do NOT Return it here.
                        requeueAndStop(res, local);
                        break;
                    }
                }
            }

            {
                ZoneScopedN("Game:ProcessRenderResponses");
                RendererResponse response;
                while (m_AppContext->RGCommandRing.Pop(response)) {
			        switch (response.Type) {
			            case RendererResponseType::MeshUpload: {
                            if (!response.Mesh.Valid) break;

                            // Spec 5: transfer the verts/indices we stashed at upload
                            // time into NavMeshSystem's MeshId-keyed cache. Must run
                            // BEFORE the IsValidEntity gate — startup .obj/.gltf loads
                            // use non-entity tickets and would otherwise skip the cache
                            // populate entirely.
                            auto it = pendingMeshData.find(response.TicketId);
                            if (it != pendingMeshData.end()) {
                                NavMeshSystem::Instance().StoreMeshCpuData(
                                    response.Mesh.Handle.Index,
                                    std::move(it->second.Vertices),
                                    std::move(it->second.Indices));
                                pendingMeshData.erase(it);
                            }

                            if (!gameState.World.IsValidEntity(response.TicketId)) continue;

                            if (!gameState.World.HasComponent<MeshComponent>(response.TicketId)) {
                                gameState.World.AddComponent(response.TicketId, MeshComponent{});
                            }
                            gameState.World.Modify<MeshComponent>(response.TicketId, [&](auto& m) {
                                m.MeshId  = response.Mesh.Handle.Index;
                                m.Visible = true;
                            });

                            SM_TRACE("GameThread: MeshUpload complete for entity %llu, meshId=%u",
                                     response.TicketId, response.Mesh.Handle.Index);
                            break;
                        }
                        case RendererResponseType::MaterialUpload: {
                            if (!response.Material.Valid) break;
                            if (!gameState.World.IsValidEntity(response.TicketId)) continue;

                            gameState.World.Modify<MaterialComponent>(response.TicketId, [&](auto& m) {
                                m.MaterialId = response.Material.Handle.Index;
                                m.Flags     |= 1u;
                            });

                            SM_TRACE("GameThread: MaterialUpload complete for entity %llu, materialId=%u",
                                     (unsigned long long)response.TicketId, response.Material.Handle.Index);
                            break;
                        }
                        default: {
			                SM_WARN("GameThread: Unhandled RendererResponseType %d", static_cast<int>(response.Type));
			                break;
			            }
                    }
			    }
			}

			gameState.DeltaTime = std::min(actualDt, targetDt * 2.0); // clamp to prevent spiral of death
		    gameState.GameTime = TimeNowSec();

			DrainInputToSingleton(gameState);

			if (m_GameLib.IsValid()) {
                m_GameLib.Update(&gameState);
            }

			{
                SystemContext sysCtx{ gameState.World, gameState.DeltaTime, gameState.GameTime, &navServices };
                m_Scheduler.Run(sysCtx);
            }

            // Drive dtTileCache::update so this tick's NavObstacleSyncSystem add/remove
            // calls actually apply (re-bake affected tiles, update obstacle state). Single
            // call per tick per Spec 2 (drain-loop policy deferred to perf measurement).
            NavMeshSystem::Instance().Tick(static_cast<float>(gameState.DeltaTime));

			// Update all loaded plugins
			if (m_PluginManager) {
				m_PluginManager->UpdateAll(gameState.DeltaTime);
			}

			if (const auto* app = gameState.World.GetSingleton<AppControlComponent>(); app && app->QuitRequested) {
				m_Running.store(false, std::memory_order_relaxed);
				m_AppContext->ShutdownRequested.store(true, std::memory_order_relaxed);
				break;
			}

			SimulateStep(targetDt); // advance simulation

			// Keep Viewport + UI camera synced to the window each tick (cheap;
			// removes the need for a separate resize signal).
			const uint64_t sv = m_AppContext->SceneViewportSize.load(std::memory_order_relaxed);
			const uint32_t vw = sv ? uint32_t(sv >> 32) : m_AppContext->Settings.windowWidth;
			const uint32_t vh = sv ? uint32_t(sv & 0xffffffffu) : m_AppContext->Settings.windowHeight;
			const uint64_t so = m_AppContext->SceneViewportOrigin.load(std::memory_order_relaxed);
			const uint32_t ox = uint32_t(so >> 32);
			const uint32_t oy = uint32_t(so & 0xffffffffu);
			gameState.World.ModifySingleton<ViewportComponent>([&](ViewportComponent& v){ v.Width = vw; v.Height = vh; v.OriginX = ox; v.OriginY = oy; });
			gameState.World.ModifySingleton<UICameraComponent>([&](UICameraComponent& ui){
				ui.Projection = glm::orthoRH_ZO(0.0f, float(vw), float(vh), 0.0f, -1.0f, 1.0f);
				ui.View = glm::mat4(1.0f);
			});

			PublishSnapshot(gameState, frameStats); // publish to SnapshotRing (S -> R)
		}

		const auto workEnd = Clock::now();

		// Calculate ActualTPS: how many ticks we've completed over wall-clock time
		tickCount++;
		const double elapsedWallTime = std::chrono::duration<double>(workEnd - tpsStartTime).count();
		if (elapsedWallTime >= tpsUpdateInterval) {
			// ActualTPS = ticks per second (based on wall-clock time including sleep)
			gameState.ActualTPS = static_cast<double>(tickCount) / elapsedWallTime;
			tickCount = 0;
			tpsStartTime = workEnd;
		}

		// Advance target time
	 	nextFrameTime += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(targetDt));

		// Hybrid sleep/spin approach for accurate timing
		{
			ZoneScopedN("FramePacing");
			const auto now = Clock::now();

			// If we're already late, skip sleep entirely
			if (now >= nextFrameTime) {
				nextFrameTime = now;
				continue;
			}

			const auto timeUntilTarget = nextFrameTime - now;
			const auto spinThreshold = std::chrono::microseconds(threadSettings.SpinThresholdMicros);

			// Coarse sleep: sleep until we're close to the target (within spin threshold)
			if (timeUntilTarget > spinThreshold) {
				const auto sleepDuration = timeUntilTarget - spinThreshold;
				std::this_thread::sleep_for(sleepDuration);
			}

			// Fine spin-wait: busy-wait for the remaining time with CPU yield hints
			while (Clock::now() < nextFrameTime) {
				#ifdef _WIN32
				_mm_pause(); // x86/x64 CPU hint for spin-wait loops
				#else
				std::this_thread::yield();
				#endif
			}
		}
	}

    m_GameLib.Unload(&gameState);

    gameState.World.Clear();

	if (m_PluginManager) {
		m_PluginManager->ShutdownAll();
	}

    // Restore Windows timer resolution
    #ifdef _WIN32
    timeEndPeriod(1);
    #endif

    // Stop and join the worker thread
    m_WorkerStop.store(true, std::memory_order_relaxed);
    m_JobCv.notify_all();
    if (m_Worker.joinable())
        m_Worker.join();
}

void GameThread::Stop() {
    m_Running.store(false, std::memory_order_relaxed);
    m_WorkerStop.store(true, std::memory_order_relaxed);
    m_JobCv.notify_all();
}

bool GameThread::Running() const {
	return m_Running.load(std::memory_order_relaxed)
		 && !m_AppContext->ShutdownRequested.load(std::memory_order_relaxed);
}

void GameThread::SimulateStep(const double dt) {
	// Simple1D motion that bounces in [-1,1]
	m_simX += m_simVX * static_cast<float>(dt);
	if (m_simX >1.0f) { m_simX =1.0f; m_simVX = -std::fabs(m_simVX); }
	if (m_simX < -1.0f) { m_simX = -1.0f; m_simVX = std::fabs(m_simVX); }
}

void GameThread::PublishSnapshot(GameState& state, const FrameTimeStats& frameStats) {
	ZoneScopedN("PublishSnapshot");
	const uint64_t tick = m_TickCounter++;

	// Create a read-only snapshot of the ECS world
	std::shared_ptr<const ECS> worldSnapshot = state.World.CreateSnapshot();

	// Store the snapshot atomically (C++20 atomic shared_ptr operations)
	// This keeps the snapshot alive while RenderThread might be reading it
	m_AppContext->LatestWorldSnapshot.store(worldSnapshot, std::memory_order_release);

	SimulationSnapshot snap{};
	snap.Tick = tick;
	snap.Timestamp = TimeNowSec();
	snap.TargetTPS = state.TargetTPS;  // Intended tick rate (60.0)
	snap.ActualTPS = state.ActualTPS;  // Measured tick rate (work time only)
	snap.ObjectX = m_simX;
	snap.ObjectVX = m_simVX;
	snap.FrameStats = frameStats;

	// Single-writer seqlock publish
    m_AppContext->LatestSnapshot.store(snap);
}

void GameThread::DrainInputToSingleton(GameState& state) {
    if (!state.World.GetSingleton<InputStateComponent>()) {
        state.World.SetSingleton(InputStateComponent{});
    }
    double prevX = 0.0, prevY = 0.0;
    if (const auto* in = state.World.GetSingleton<InputStateComponent>()) {
        prevX = in->MouseX;
        prevY = in->MouseY;
    }

    state.World.ModifySingleton<InputStateComponent>([&](InputStateComponent& s) {
        std::memset(s.Pressed, 0, sizeof(s.Pressed));
        s.Wheel = 0;
        std::memset(s.MousePressed, 0, sizeof(s.MousePressed));
        InputEvent ev{};
        while (m_AppContext->InputRing.Pop(ev)) {
            if (ev.Type == InputEventType::Key) {
                const int k = static_cast<int>(ev.KeyEvent.Key);
                if (k >= 0 && k <= KEY_LAST) {
                    if (ev.KeyEvent.Action == PRESS || ev.KeyEvent.Action == REPEAT) s.KeysDown[k] = true;
                    if (ev.KeyEvent.Action == RELEASE) s.KeysDown[k] = false;
                    if (ev.KeyEvent.Action == PRESS) s.Pressed[k] = true;
                }
            } else if (ev.Type == InputEventType::MouseMove) {
                s.MouseX = ev.MouseMoveEvent.X;
                s.MouseY = ev.MouseMoveEvent.Y;
            } else if (ev.Type == InputEventType::MouseWheel) {
                s.Wheel = static_cast<int32_t>(ev.MouseScrollEvent.OffsetY);
            } else if (ev.Type == InputEventType::MouseButton) {
                const int b = static_cast<int>(ev.MouseButtonEvent.Button);
                if (b >= 0 && b <= MOUSE_BUTTON_LAST) {
                    if (ev.MouseButtonEvent.Action == PRESS)   { s.MouseDown[b] = true; s.MousePressed[b] = true; }
                    if (ev.MouseButtonEvent.Action == RELEASE) {  s.MouseDown[b] = false; }
                }
            }
        }
        s.MouseDX = s.MouseX - prevX;
        s.MouseDY = s.MouseY - prevY;
    });
}

void GameThread::EnqueueModelLoadJob(uint64_t ticketId, const std::string& objPath, const std::string& mtlBaseDir)
{
    ModelLoadJob job;
    job.ticketId = ticketId;
    job.objPath = objPath;
    job.mtlBaseDir = mtlBaseDir;
    {
        std::scoped_lock lg(m_JobMutex);
        m_PendingJobs.push(std::move(job));
    }
    m_JobCv.notify_one();
}

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <filesystem>

void GameThread::WorkerThreadFunc()
{
    tracy::SetThreadName("ModelWorker");
    for (;;)
    {
        ModelLoadJob job;
        {
            std::unique_lock<std::mutex> ul(m_JobMutex);
            m_JobCv.wait(ul, [&]{ return m_WorkerStop.load(std::memory_order_relaxed) || !m_PendingJobs.empty(); });
            if (m_WorkerStop.load(std::memory_order_relaxed) && m_PendingJobs.empty())
                break;
            job = std::move(m_PendingJobs.front());
            m_PendingJobs.pop();
        }

        ModelLoadResult result;
        result.ticketId = job.ticketId;

        tinyobj::attrib_t attrib;
        std::vector<tinyobj::shape_t> shapes;
        std::vector<tinyobj::material_t> materials;
        std::string warn;
        std::string err;

        const auto now = Clock::now();

        Assimp::Importer importer;

        const aiScene* scene = importer.ReadFile(job.objPath,
            aiProcess_Triangulate |
            aiProcess_GenSmoothNormals |
            // aiProcess_FlipUVs |
            aiProcess_JoinIdenticalVertices);

        if (!scene) {
            SM_ERROR("Assimp failed to load scene '%s': %s", job.objPath.c_str(), importer.GetErrorString());
            result.success = false;
            result.error = std::string("Assimp failed to load scene: ") + importer.GetErrorString();
            {
                std::scoped_lock lg(m_JobMutex);
                m_CompletedJobs.push(std::move(result));
            }
            continue;
        }

        auto processMesh = [&](aiMesh* mesh, const aiScene* sceneRef) {
            // Process mesh data if needed
            std::vector<MeshVertex> vertices;
            std::vector<uint32_t> indices;

            for (size_t i = 0; i < mesh->mNumVertices; ++i) {
                MeshVertex vertex{};
                vertex.px = mesh->mVertices[i].x;
                vertex.py = mesh->mVertices[i].y;
                vertex.pz = mesh->mVertices[i].z;
                if (mesh->HasNormals()) {
                    vertex.nx = mesh->mNormals[i].x;
                    vertex.ny = mesh->mNormals[i].y;
                    vertex.nz = mesh->mNormals[i].z;
                } else {
                    vertex.nx = 0.0f; vertex.ny = 0.0f; vertex.nz = 0.0f;
                }
                if (mesh->HasTextureCoords(0)) {
                    vertex.u = mesh->mTextureCoords[0][i].x;
                    vertex.v = mesh->mTextureCoords[0][i].y;
                } else {
                    vertex.u = 0.0f; vertex.v = 0.0f;
                }

                vertices.push_back(vertex);
            }

            for (size_t i = 0; i < mesh->mNumFaces; ++i) {
                aiFace face = mesh->mFaces[i];
                for (size_t j = 0; j < face.mNumIndices; ++j) {
                    indices.push_back(face.mIndices[j]);
                }
            }

            if (mesh->mMaterialIndex >= 0 && mesh->mMaterialIndex < UINT32_MAX) {
                aiMaterial* material = sceneRef->mMaterials[mesh->mMaterialIndex];
                if (material) {
                    aiString texPath;
                    for (size_t i = 0; i < material->mNumProperties; ++i) {
                        auto prop = material->mProperties[i];
                        // SM_TRACE("Material property: key='%s', semantic=%d, index=%d type=%d length=%u", prop->mKey.C_Str(), prop->mSemantic, prop->mIndex, static_cast<int>(prop->mType), prop->mDataLength);
                    }
                    // The material might be a texture image, or a color
                    auto color = aiColor4D{};
                    if (AI_SUCCESS == material->Get(AI_MATKEY_COLOR_DIFFUSE, color)) {
                        // SM_TRACE("Material diffuse color: r=%.3f g=%.3f b=%.3f a=%.3f", color.r, color.g, color.b, color.a);
                    }

                    if (material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
                        std::string fullTexPath = std::string(texPath.C_Str());

                        std::vector<uint32_t> pixels;
                        uint32_t width = 0;
                        uint32_t height = 0;
                        std::string error;
                        if (!MaterialLoader::LoadMaterialFromFile(fullTexPath.c_str(), pixels, width, height, error)) {
                            SM_WARN("Failed to load material '%s': %s", fullTexPath.c_str(), error.c_str());
                        } else {
                            result.Width = width;
                            result.Height = height;
                            if (result.Texture) { GetStagingPool().Return(result.Texture); result.Texture = nullptr; }
                            if (!pixels.empty()) {
                                const size_t texBytes = static_cast<size_t>(width) * height * sizeof(uint32_t);
                                result.Texture = static_cast<uint32_t*>(GetStagingPool().Acquire(texBytes));
                                std::memcpy(result.Texture, pixels.data(), texBytes);
                            }
                        }
                    }
                }
            }

            return std::make_tuple(vertices, indices);
        };

        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> indices;

        std::function<void(const aiNode*, const aiScene*, std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices)> processNode
        = [&](const aiNode* node, const aiScene* sceneRef, std::vector<MeshVertex>& totalVertices, std::vector<uint32_t>& totalIndices) {

            const auto totalMeshes = node->mNumMeshes;

            for (size_t m = 0; m < totalMeshes; ++m) {
                aiMesh* mesh = sceneRef->mMeshes[node->mMeshes[m]];
                auto [vertex, index] = processMesh(mesh, sceneRef);
                result.subMeshes.push_back(SubMesh{
                    .IndexStart = static_cast<uint32_t>(totalIndices.size()),
                    .IndexCount = static_cast<uint32_t>(index.size())}
                );
                totalVertices.insert(totalVertices.end(), vertex.begin(), vertex.end());
                totalIndices.insert(totalIndices.end(), index.begin(), index.end());
            }

            for (size_t i = 0; i < node->mNumChildren; i++) {
                processNode(node->mChildren[i], sceneRef, totalVertices, totalIndices);
            }
        };

        processNode(scene->mRootNode, scene, result.vertices, result.indices);

        result.success = result.vertices.size() > 0;

        {
            std::scoped_lock lg(m_JobMutex);
            m_CompletedJobs.push(std::move(result));
        }
    }
}
