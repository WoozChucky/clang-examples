#include "GameThread.h"

#include <windows.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include <regex>

#ifdef _WIN32
#include <timeapi.h>  // For timeBeginPeriod/timeEndPeriod
#include <intrin.h>   // For _mm_pause
#pragma comment(lib, "winmm.lib")  // Link against winmm.lib for timeBeginPeriod
#endif

#include <GLFW/glfw3.h>
#include <tracy/Tracy.hpp>
#include "tiny_obj_loader.h"

#include "lib.h"
#include "Timing.h"

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
    gameState.PlatformInput = &m_AppContext->InputRing;
    gameState.Settings = &m_AppContext->Settings;

    // Start background worker for model loading
    m_WorkerStop.store(false, std::memory_order_relaxed);
    m_Worker = std::thread(&GameThread::WorkerThreadFunc, this);

    {
        auto cubeEntityId = gameState.World.CreateEntity();
        auto cubeTransform = TransformComponent{.Position = glm::vec3{0.f, 0.f, 0.f}, .Rotation = glm::vec3{0.f}, .Scale = glm::vec3{1.f}};
        auto cubeMesh = MeshComponent{ .MeshId = 0, .Visible = false };
        auto cubeMaterial = MaterialComponent{ .BaseColor = glm::vec4{1.f, 0.f, 0.f, 1.0f} };
        gameState.World.AddComponent(cubeEntityId, cubeTransform);
        gameState.World.AddComponent(cubeEntityId, cubeMaterial);
        gameState.World.AddComponent(cubeEntityId, cubeMesh);
        // Enqueue model loading job to background worker
        EnqueueModelLoadJob(cubeEntityId, "assets/models/cube.obj", "assets/models"); // stanford-bunny
    }

    {
        auto sphereEntityId = gameState.World.CreateEntity();
        auto sphereTransform = TransformComponent{.Position = glm::vec3{-5.f, 0.f, -5.f}, .Rotation = glm::vec3{0.f}, .Scale = glm::vec3{1.f}};
        auto sphereMesh = MeshComponent{ .MeshId = 0, .Visible = false };
        auto sphereMaterial = MaterialComponent{ .BaseColor = glm::vec4{1.f, 0.f, 0.f, 1.0f} };
        gameState.World.AddComponent(sphereEntityId, sphereTransform);
        gameState.World.AddComponent(sphereEntityId, sphereMaterial);
        gameState.World.AddComponent(sphereEntityId, sphereMesh);
        // Enqueue model loading job to background worker
        EnqueueModelLoadJob(sphereEntityId, "assets/models/sphere.obj", "assets/models");
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

	while (Running()) {
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
                ECSCommandProcessor::ProcessCommands(gameState.World, m_AppContext->ECSCommandRing);
            }

            // Drain completed background model loads and forward to Render via GR ring
            {
                ZoneScopedN("Game:ProcessCompletedModelLoads");
                // Move completed results to a local queue to minimize lock time
                std::queue<ModelLoadResult> local;
                {
                    std::lock_guard<std::mutex> lg(m_JobMutex);
                    while (!m_CompletedJobs.empty()) {
                        local.push(std::move(m_CompletedJobs.front()));
                        m_CompletedJobs.pop();
                    }
                }

                while (!local.empty())
                {
                    ModelLoadResult res = std::move(local.front());
                    local.pop();

                    if (!res.success)
                    {
                        SM_ERROR("Model load failed for ticket %llu: %s", (unsigned long long)res.ticketId, res.error.c_str());
                        continue;
                    }

                    // Prepare a RendererCommand but only allocate buffers if we can push
                    RendererCommand cmd{};
                    cmd.Type = RendererCommandType::RequestModel;
                    cmd.TicketId = res.ticketId;
                    cmd.ModelRequest.UseTexture = false;
                    cmd.ModelRequest.Width = 0;
                    cmd.ModelRequest.Height = 0;
                    cmd.ModelRequest.Texture = nullptr;
                    cmd.ModelRequest.TextureSize = 0;

                    // Allocate and copy data
                    cmd.ModelRequest.VertexCount = res.vertices.size();
                    cmd.ModelRequest.IndexCount = res.indices.size();
                    cmd.ModelRequest.Vertices = nullptr;
                    cmd.ModelRequest.Indices = nullptr;

                    if (cmd.ModelRequest.VertexCount > 0)
                    {
                        cmd.ModelRequest.Vertices = static_cast<MeshVertex*>(std::malloc(cmd.ModelRequest.VertexCount * sizeof(MeshVertex)));
                        std::memcpy(cmd.ModelRequest.Vertices, res.vertices.data(), cmd.ModelRequest.VertexCount * sizeof(MeshVertex));
                    }
                    if (cmd.ModelRequest.IndexCount > 0)
                    {
                        cmd.ModelRequest.Indices = static_cast<uint32_t*>(std::malloc(cmd.ModelRequest.IndexCount * sizeof(uint32_t)));
                        std::memcpy(cmd.ModelRequest.Indices, res.indices.data(), cmd.ModelRequest.IndexCount * sizeof(uint32_t));
                    }

                    if (!m_AppContext->GRCommandRing.Push(cmd))
                    {
                        SM_WARN("GRCommandRing full, retrying model upload next frame (ticket %llu)", (unsigned long long)res.ticketId);
                        // Free allocated memory to avoid leaks; requeue the result for retry
                        if (cmd.ModelRequest.Vertices) std::free(cmd.ModelRequest.Vertices);
                        if (cmd.ModelRequest.Indices) std::free(cmd.ModelRequest.Indices);
                        std::lock_guard<std::mutex> lg(m_JobMutex);
                        m_CompletedJobs.push(std::move(res));
                        // Break to avoid tight loop; leave remaining in 'local' for next frame
                        break;
                    }
                }
            }

            {
                ZoneScopedN("Game:ProcessRenderResponses");
                RendererResponse response;
                while (m_AppContext->RGCommandRing.Pop(response)) {
			        switch (response.Type) {
			            case RendererResponseType::ModelUpload: {
                            if (response.Model.Valid) {
                                auto meshComponent = gameState.World.GetComponent<MeshComponent>(response.TicketId);
                                if (!meshComponent) continue;
                                meshComponent->MeshId = response.Model.Handle.Index;
                                meshComponent->Visible = true;
                            }
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

			GameUpdate(&gameState);

			// Update all loaded plugins
			if (m_PluginManager) {
				m_PluginManager->UpdateAll(gameState.DeltaTime);
			}

			if (gameState.QuitRequested) {
				m_Running.store(false, std::memory_order_relaxed);
				m_AppContext->ShutdownRequested.store(true, std::memory_order_relaxed);
				break;
			}

			SimulateStep(targetDt); // advance simulation
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

    GameExit(&gameState);

    gameState.World.Clear();

	if (m_PluginManager) {
		m_PluginManager->ShutdownAll();
	}

    // Restore Windows timer resolution
    #ifdef _WIN32
    timeEndPeriod(1);
    #endif

    // Stop and join worker thread
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

void GameThread::SimulateStep(double dt) {
	// Simple1D motion that bounces in [-1,1]
	m_simX += m_simVX * static_cast<float>(dt);
	if (m_simX >1.0f) { m_simX =1.0f; m_simVX = -std::fabs(m_simVX); }
	if (m_simX < -1.0f) { m_simX = -1.0f; m_simVX = std::fabs(m_simVX); }
}

void GameThread::PublishSnapshot(const GameState& state, const FrameTimeStats& frameStats) {
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
	snap.GameCamera = state.GameCamera;
	snap.UICamera = state.UICamera;
	snap.FrameStats = frameStats;

	// Pass raw pointer through Seqlock (Seqlock requires trivially copyable types)
	// The shared_ptr above keeps this pointer valid
	snap.WorldSnapshotPtr = worldSnapshot.get();

	// Single-writer seqlock publish
    m_AppContext->LatestSnapshot.store(snap);
}

void GameThread::EnqueueModelLoadJob(uint64_t ticketId, const std::string& objPath, const std::string& mtlBaseDir)
{
    ModelLoadJob job;
    job.ticketId = ticketId;
    job.objPath = objPath;
    job.mtlBaseDir = mtlBaseDir;
    {
        std::lock_guard<std::mutex> lg(m_JobMutex);
        m_PendingJobs.push(std::move(job));
    }
    m_JobCv.notify_one();
}

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

        bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, job.objPath.c_str(), job.mtlBaseDir.c_str());
        if (!warn.empty()) {
            SM_WARN("TinyObjLoader warning: %s", warn.c_str());
        }

        const auto loadDuration = std::chrono::duration<double>(Clock::now() - now).count();
        SM_TRACE("Loaded OBJ '%s' in %.3f seconds: %zu vertices, %zu shapes, %zu materials",
                job.objPath.c_str(), loadDuration,
                attrib.vertices.size() / 3, shapes.size(), materials.size());

        if (!ok || !err.empty()) {
            if (!err.empty()) SM_ERROR("TinyObjLoader error: %s", err.c_str());
            result.success = false;
            result.error = err.empty() ? std::string("Failed to load OBJ") : err;
        } else {
            // Build vertices & indices with deduplication
            std::vector<MeshVertex> vertices;
            std::vector<uint32_t> indices;
            std::unordered_map<size_t, uint32_t> vertexMap;  // hash -> index

            auto hashVertex = [](const MeshVertex& v) -> size_t {
                size_t h = 0;
                h ^= std::hash<float>{}(v.px) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<float>{}(v.py) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<float>{}(v.pz) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<float>{}(v.nx) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<float>{}(v.ny) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<float>{}(v.nz) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<float>{}(v.u) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<float>{}(v.v) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            };

            // Count total triangles across all shapes first
            size_t totalTris = 0;
            for (const auto& shape : shapes) {
                for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
                    if (shape.mesh.num_face_vertices[f] == 3) ++totalTris;
                }
            }

            vertices.reserve(totalTris * 3);  // Exact size
            indices.reserve(totalTris * 3);

            for (const auto& shape : shapes) {
                for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
                    const int fv = shape.mesh.num_face_vertices[f];
                    if (fv != 3) continue;

                    for (int v = 0; v < 3; ++v) {
                        const tinyobj::index_t idx = shape.mesh.indices[f * 3 + v];
                        MeshVertex vert{};

                        // Position
                        vert.px = attrib.vertices[3 * idx.vertex_index + 0];
                        vert.py = attrib.vertices[3 * idx.vertex_index + 1];
                        vert.pz = attrib.vertices[3 * idx.vertex_index + 2];

                        // Normal
                        if (idx.normal_index >= 0 && !attrib.normals.empty()) {
                            vert.nx = attrib.normals[3 * idx.normal_index + 0];
                            vert.ny = attrib.normals[3 * idx.normal_index + 1];
                            vert.nz = attrib.normals[3 * idx.normal_index + 2];
                        } else {
                            // Default up normal if not provided
                            vert.nx = 0.0f;
                            vert.ny = 1.0f;
                            vert.nz = 0.0f;
                        }

                        // UV
                        if (idx.texcoord_index >= 0 && !attrib.texcoords.empty()) {
                            vert.u = attrib.texcoords[2 * idx.texcoord_index + 0];
                            vert.v = 1.0f - attrib.texcoords[2 * idx.texcoord_index + 1];
                        } else {
                            vert.u = 0.0f;
                            vert.v = 0.0f;
                        }

                        const size_t hash = hashVertex(vert);
                        auto it = vertexMap.find(hash);
                        if (it != vertexMap.end()) {
                            indices.push_back(it->second);  // Reuse existing vertex
                        } else {
                            uint32_t newIdx = static_cast<uint32_t>(vertices.size());
                            vertices.push_back(vert);
                            vertexMap[hash] = newIdx;
                            indices.push_back(newIdx);
                        }
                    }
                }
            }

            const auto processDuration = std::chrono::duration<double>(Clock::now() - now).count();
            SM_TRACE("Processed model '%s': %zu unique vertices, %zu indices in %.3f seconds",
                    job.objPath.c_str(), vertices.size(), indices.size(), processDuration);

            result.success = true;
            result.vertices = std::move(vertices);
            result.indices = std::move(indices);
        }

        {
            std::lock_guard<std::mutex> lg(m_JobMutex);
            m_CompletedJobs.push(std::move(result));
        }
    }
}

/*

Same as above but without vertex deduplication for comparison
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

        bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, job.objPath.c_str(), job.mtlBaseDir.c_str());
        if (!warn.empty()) {
            SM_WARN("TinyObjLoader warning: %s", warn.c_str());
        }

        const auto loadDuration = std::chrono::duration<double>(Clock::now() - now).count();
        SM_TRACE("Loaded OBJ '%s' in %.3f seconds: %zu vertices, %zu shapes, %zu materials",
                job.objPath.c_str(), loadDuration,
                attrib.vertices.size() / 3, shapes.size(), materials.size());

        if (!ok || !err.empty()) {
            if (!err.empty()) SM_ERROR("TinyObjLoader error: %s", err.c_str());
            result.success = false;
            result.error = err.empty() ? std::string("Failed to load OBJ") : err;
        } else {
            std::vector<MeshVertex> vertices;
            std::vector<uint32_t> indices;

            // Count total triangles
            size_t totalTris = 0;
            for (const auto& shape : shapes) {
                for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
                    if (shape.mesh.num_face_vertices[f] == 3) ++totalTris;
                }
            }

            vertices.reserve(totalTris * 3);
            indices.reserve(totalTris * 3);

            uint32_t currentIndex = 0;
            for (const auto& shape : shapes) {
                for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
                    const int fv = shape.mesh.num_face_vertices[f];
                    if (fv != 3) continue;

                    for (int v = 0; v < 3; ++v) {
                        const tinyobj::index_t idx = shape.mesh.indices[f * 3 + v];
                        MeshVertex vert{};

                        // Position
                        vert.px = attrib.vertices[3 * idx.vertex_index + 0];
                        vert.py = attrib.vertices[3 * idx.vertex_index + 1];
                        vert.pz = attrib.vertices[3 * idx.vertex_index + 2];

                        // Normal
                        if (idx.normal_index >= 0 && !attrib.normals.empty()) {
                            vert.nx = attrib.normals[3 * idx.normal_index + 0];
                            vert.ny = attrib.normals[3 * idx.normal_index + 1];
                            vert.nz = attrib.normals[3 * idx.normal_index + 2];
                        } else {
                            vert.nx = 0.0f;
                            vert.ny = 1.0f;
                            vert.nz = 0.0f;
                        }

                        // UV
                        if (idx.texcoord_index >= 0 && !attrib.texcoords.empty()) {
                            vert.u = attrib.texcoords[2 * idx.texcoord_index + 0];
                            vert.v = 1.0f - attrib.texcoords[2 * idx.texcoord_index + 1];
                        } else {
                            vert.u = 0.0f;
                            vert.v = 0.0f;
                        }

                        vertices.push_back(vert);
                        indices.push_back(currentIndex++);
                    }
                }
            }

            const auto processDuration = std::chrono::duration<double>(Clock::now() - now).count();
            SM_TRACE("Processed model '%s': %zu vertices, %zu indices in %.3f seconds",
                    job.objPath.c_str(), vertices.size(), indices.size(), processDuration);

            result.success = true;
            result.vertices = std::move(vertices);
            result.indices = std::move(indices);
        }

        {
            std::lock_guard<std::mutex> lg(m_JobMutex);
            m_CompletedJobs.push(std::move(result));
        }
    }
}
*/
