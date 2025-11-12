#include "GameThread.h"

#include <Windows.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include <regex>

#include "lib.h"
#include "Timing.h"
#include "GLFW/glfw3.h"

#include <tracy/Tracy.hpp>

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

	 // Initialize hot-reload system (file watcher + initial copy & load)
	 InitHotReload();

	// Initialize plugin system
	m_PluginManager = std::make_unique<DotNetPluginManager>();
	if (m_PluginManager->Initialize("assets/plugins/PluginCore.runtimeconfig.json")) {
		m_PluginManager->LoadPluginsFromDirectory("assets/plugins");
	}

	 GameState gameState{};
	 gameState.PlatformInputHandle = &m_AppContext->InputRing;
	 gameState.Settings = &m_AppContext->Settings;

	 constexpr double targetDt =1.0 /60.0;
	auto nextFrameTime = Clock::now();
	auto lastFrameTime = nextFrameTime;

	 while (Running())
	 {
		const auto frameStart = Clock::now();
		const double actualDt = std::chrono::duration<double>(frameStart - lastFrameTime).count();
	 	lastFrameTime = frameStart;

		{
			ZoneScopedN("Game:FixedUpdate");
			// Handle requested reloads
			ReloadGameLibraryIfRequested();

			gameState.DeltaTime = std::min(actualDt, targetDt * 2.0); // clamp to prevent spiral of death

			if (m_GameLib.IsValid()) {
				ZoneScopedN("Game:DLLUpdate");
				m_GameLib.Update(&gameState);
			}

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
			PublishSnapshot(gameState); // publish to SnapshotRing (S -> R)

		}

	 	nextFrameTime += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(targetDt));
	 	std::this_thread::sleep_until(nextFrameTime);

	 	const auto now = Clock::now();
	 	if (now > nextFrameTime) {
	 		nextFrameTime = now;
	 	}
	}

	if (m_GameLib.IsValid()) {
		m_GameLib.ExitGame();
	}

	if (m_PluginManager) {
		m_PluginManager->ShutdownAll();
	}

	UnloadGameLibrary();
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

void GameThread::PublishSnapshot(const GameState& state) {
	ZoneScopedN("PublishSnapshot");
	const uint64_t tick = m_TickCounter++;

	SimulationSnapshot snap{};
	snap.Tick = tick;
	snap.Timestamp = TimeNowSec();
	snap.ObjectX = m_simX;
	snap.ObjectVX = m_simVX;
	snap.GameCamera = state.GameCamera;
	snap.UICamera = state.UICamera;

	// Single-writer seqlock publish
	m_AppContext->LatestSnapshot.store(snap);
}

void GameThread::InitHotReload() {
	// Start watcher first so any immediate changes are captured
	SetupGameDllWatcher();

	// Ensure we load from the temp copy, never directly from Game.dll
	if (!CopyGameDllToTempWithRetry()) {
		SM_WARN("Could not copy Game.dll to GameCode.dll on startup. Continuing without game code loaded.");
	}

	LoadTempGameLibrary();
}

void GameThread::SetupGameDllWatcher() {
	try {
		 m_GameDllWatch = std::make_unique<filewatch::FileWatch<std::string>>(
		 std::string("Game.dll"),
		 std::regex("Game\\.dll", std::regex_constants::icase),
		 [this](const std::string& /*file*/, const filewatch::Event eventType) {
				switch (eventType) {
					case filewatch::Event::modified:
					case filewatch::Event::renamed_new:
						SM_TRACE("Game.dll changed, scheduling reload...");
						m_LibraryReload.store(true, std::memory_order_relaxed);
						break;
					default:
						break;
				}
			});
	}
	catch (const std::exception& e) {
 		SM_ERROR("Failed to create Game.dll watcher: %s", e.what());
	}
}

bool GameThread::CopyGameDllToTempWithRetry(int maxRetries, int delayMs) {

	const std::string src = "Game.dll";
	const std::string dst = "GameCode.dll";

	std::error_code ec;

	// Retry copy to handle linker still writing/locking the file
	for (int attempt =0; attempt < maxRetries; ++attempt) {
		ec.clear();
		// Overwrite temp copy every time
		std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
		if (!ec) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
	}

	SM_ERROR("Failed to copy %s to %s after %d attempts: %s", src.c_str(), dst.c_str(), maxRetries, ec.message().c_str());
	return false;
}

bool GameThread::LoadTempGameLibrary() {
	// Always load from the temp filename
	m_GameLib = LoadGameLibrary("GameCode.dll");
	if (!m_GameLib.IsValid()) {
		SM_WARN("GameCode.dll could not be loaded or missing required exports.");
		return false;
	}
	return true;
}

void GameThread::UnloadGameLibrary() {
	FreeGameLibrary();	
}

void GameThread::ReloadGameLibraryIfRequested() {
	ZoneScopedN("HotReload");
	if (!m_LibraryReload.load(std::memory_order_relaxed)) {
		return;
	}

	// Perform hot-reload sequence
	SM_TRACE("Hot-reloading GameCode.dll...");
	UnloadGameLibrary();

	if (!CopyGameDllToTempWithRetry()) {
		SM_ERROR("Hot-reload aborted: failed to copy Game.dll.");
		m_LibraryReload.store(false, std::memory_order_relaxed);
		return;
	}

	LoadTempGameLibrary();
	m_LibraryReload.store(false, std::memory_order_relaxed);
}

GameLibrary GameThread::LoadGameLibrary(const std::string_view libraryName) {
	GameLibrary GameLib{};

	const Library libHandle = LoadLibraryA(libraryName.data());
	if (libHandle == nullptr) {
		SM_ERROR("Failed to load %s", libraryName.data());
		return GameLib;
	}

	GameLib.Handle = libHandle;
	GameLib.GetVersion = reinterpret_cast<GameGetVersionFunc>(GetProcAddress(static_cast<HMODULE>(libHandle), "GameGetVersion"));
	GameLib.SetPlatformDebugBreak = reinterpret_cast<GameSetPlatformDebugBreakFunc>(GetProcAddress(static_cast<HMODULE>(libHandle), "GameSetPlatformDebugBreak"));
	GameLib.Update = reinterpret_cast<GameUpdateFunc>(GetProcAddress(static_cast<HMODULE>(libHandle), "GameUpdate"));
	GameLib.Resize = reinterpret_cast<GameResizeFunc>(GetProcAddress(static_cast<HMODULE>(libHandle), "GameResize"));
	GameLib.ExitGame = reinterpret_cast<GameExitFunc>(GetProcAddress(static_cast<HMODULE>(libHandle), "GameExit"));

	if (!GameLib.IsValid()) {
		SM_ERROR("Failed to load game functions from %s", libraryName.data());
		FreeLibrary(static_cast<HMODULE>(libHandle));
		return {};
	}
	// Set platform debug break function
	GameLib.SetPlatformDebugBreak(platform_debug_break);

	return GameLib;
}

void GameThread::FreeGameLibrary() {
	if (m_GameLib.Handle != nullptr) {
		FreeLibrary(static_cast<HMODULE>(m_GameLib.Handle));
		m_GameLib.Handle = nullptr;
		m_GameLib.SetPlatformDebugBreak = nullptr;
		m_GameLib.GetVersion = nullptr;
		m_GameLib.Update = nullptr;
		m_GameLib.Resize = nullptr;
		m_GameLib.ExitGame = nullptr;
	}
}
