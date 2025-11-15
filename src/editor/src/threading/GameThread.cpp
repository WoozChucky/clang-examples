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

    auto textEntityId = gameState.World.CreateEntity();
    auto transform = TransformComponent{.Position = glm::vec3{200.f, 550.f, 0.f}, .Rotation = glm::vec3{0.f}, .Scale = glm::vec3{1.f}};
    auto text = TextComponent{.Text = "Hello, Thread!", .Color = glm::vec4{1.0f, 1.0f, 1.0f, 1.0f}, .FontSize = 48};
    gameState.World.AddComponent(textEntityId, transform);
    gameState.World.AddComponent(textEntityId, text);

    auto cubeEntityId = gameState.World.CreateEntity();
    auto cubeTransform = TransformComponent{.Position = glm::vec3{0.f, 0.f, 0.f}, .Rotation = glm::vec3{0.f}, .Scale = glm::vec3{1.f}};
    auto cubeMesh = MeshComponent{ .MeshId = 0, .Visible = false };
    gameState.World.AddComponent(cubeEntityId, cubeTransform);
    gameState.World.AddComponent(cubeEntityId, cubeMesh);

	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string warn;
	std::string err;
	tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, "assets/models/cube.obj", "assets/models");
	if (!warn.empty()) {
	    SM_WARN("TinyObjLoader warning: %s", warn.c_str());
	}
	if (!err.empty()) {
	    SM_ERROR("TinyObjLoader error: %s", err.c_str());
	}

    // Prepare vertices and indices
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t indexBase = 0;

    for (const auto& shape : shapes) {
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            const int fv = shape.mesh.num_face_vertices[f];
            if (fv != 3) {
                SM_WARN("Non-triangle face (fv=%d), skipping", fv);
                continue;
            }

            for (int v = 0; v < 3; ++v) {
                const tinyobj::index_t idx = shape.mesh.indices[f * 3 + v];

                MeshVertex vert{};
                // Position
                vert.px = attrib.vertices[3 * idx.vertex_index + 0];
                vert.py = attrib.vertices[3 * idx.vertex_index + 1];
                vert.pz = attrib.vertices[3 * idx.vertex_index + 2];

                // UV (if available)
                if (idx.texcoord_index >= 0 && !attrib.texcoords.empty()) {
                    vert.u = attrib.texcoords[2 * idx.texcoord_index + 0];
                    vert.v = 1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]; // flip Y if needed
                } else {
                    vert.u = 0.0f;
                    vert.v = 0.0f;
                }

                vertices.push_back(vert);
                indices.push_back(indexBase++);
            }
        }
        if (!shape.mesh.material_ids.empty()) {
            const int mat_id = shape.mesh.material_ids[0];
            if (mat_id >= 0 && mat_id < static_cast<int>(materials.size())) {
                const auto& mat = materials[mat_id];
                SM_TRACE("Material: %s", mat.name.c_str());

                auto baseColor = glm::vec4(1.0f);
                baseColor.r = mat.diffuse[0];
                baseColor.g = mat.diffuse[1];
                baseColor.b = mat.diffuse[2];
                baseColor.a = 1.0f;

                auto cubeMaterial = MaterialComponent{
                    .MaterialId = 0,
                    .TextureId = 0,
                    .BaseColor = baseColor,
                    .Flags = 0
                };
                gameState.World.AddComponent(cubeEntityId, cubeMaterial);
            } else {
                SM_WARN("Invalid material_id %d", mat_id);
            }
        }
    }

    /*
    static const MeshVertex cubeVertices[24] = {
        // Front (+Z)
        {-1.f,-1.f, 1.f, 0.f,0.f},
        { 1.f,-1.f, 1.f, 1.f,0.f},
        { 1.f, 1.f, 1.f, 1.f,1.f},
        {-1.f, 1.f, 1.f, 0.f,1.f},
        // Back (-Z)
        { 1.f,-1.f,-1.f, 0.f,0.f},
        {-1.f,-1.f,-1.f, 1.f,0.f},
        {-1.f, 1.f,-1.f, 1.f,1.f},
        { 1.f, 1.f,-1.f, 0.f,1.f},
        // Left (-X)
        {-1.f,-1.f,-1.f, 0.f,0.f},
        {-1.f,-1.f, 1.f, 1.f,0.f},
        {-1.f, 1.f, 1.f, 1.f,1.f},
        {-1.f, 1.f,-1.f, 0.f,1.f},
        // Right (+X)
        { 1.f,-1.f, 1.f, 0.f,0.f},
        { 1.f,-1.f,-1.f, 1.f,0.f},
        { 1.f, 1.f,-1.f, 1.f,1.f},
        { 1.f, 1.f, 1.f, 0.f,1.f},
        // Top (+Y)
        {-1.f, 1.f, 1.f, 0.f,0.f},
        { 1.f, 1.f, 1.f, 1.f,0.f},
        { 1.f, 1.f,-1.f, 1.f,1.f},
        {-1.f, 1.f,-1.f, 0.f,1.f},
        // Bottom (-Y)
        {-1.f,-1.f,-1.f, 0.f,0.f},
        { 1.f,-1.f,-1.f, 1.f,0.f},
        { 1.f,-1.f, 1.f, 1.f,1.f},
        {-1.f,-1.f, 1.f, 0.f,1.f},
    };

    static const uint32_t cubeIndices[36] = {
        0,1,2, 2,3,0,        // Front
        4,5,6, 6,7,4,        // Back
        8,9,10, 10,11,8,     // Left
        12,13,14, 14,15,12,  // Right
        16,17,18, 18,19,16,  // Top
        20,21,22, 22,23,20   // Bottom
    };
    */

    RendererCommand cmd{};
    cmd.Type = RendererCommandType::RequestModel;
    cmd.TicketId = cubeEntityId;
    /*
    // Vertices
    cmd.ModelRequest.VertexCount = std::size(cubeVertices);
    cmd.ModelRequest.Vertices = static_cast<MeshVertex*>(
        std::malloc(cmd.ModelRequest.VertexCount * sizeof(MeshVertex)));
    std::memcpy(cmd.ModelRequest.Vertices, cubeVertices,
                cmd.ModelRequest.VertexCount * sizeof(MeshVertex));

    // Indices
    cmd.ModelRequest.IndexCount = std::size(cubeIndices);
    cmd.ModelRequest.Indices = static_cast<uint32_t*>(
        std::malloc(cmd.ModelRequest.IndexCount * sizeof(uint32_t)));
    std::memcpy(cmd.ModelRequest.Indices, cubeIndices,
                cmd.ModelRequest.IndexCount * sizeof(uint32_t));

    */

    cmd.ModelRequest.VertexCount = vertices.size();
    cmd.ModelRequest.Vertices = static_cast<MeshVertex*>(
        std::malloc(cmd.ModelRequest.VertexCount * sizeof(MeshVertex)));
    std::memcpy(cmd.ModelRequest.Vertices, vertices.data(),
                cmd.ModelRequest.VertexCount * sizeof(MeshVertex));

    cmd.ModelRequest.IndexCount = indices.size();
    cmd.ModelRequest.Indices = static_cast<uint32_t*>(
        std::malloc(cmd.ModelRequest.IndexCount * sizeof(uint32_t)));
    std::memcpy(cmd.ModelRequest.Indices, indices.data(),
                cmd.ModelRequest.IndexCount * sizeof(uint32_t));

    cmd.ModelRequest.UseTexture = false;
    cmd.ModelRequest.Width = 0;
    cmd.ModelRequest.Height = 0;
    cmd.ModelRequest.Texture = nullptr;
    cmd.ModelRequest.TextureSize = 0;
    m_AppContext->GRCommandRing.Push(cmd);

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
}

void GameThread::Stop() {
	m_Running.store(false, std::memory_order_relaxed);
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
