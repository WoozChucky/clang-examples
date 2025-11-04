#include "GameThread.h"

#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <thread>

#include "lib.h"
#include "Timing.h"
#include "FileWatch.h"
#include "GLFW/glfw3.h"

GameThread::GameThread(const std::shared_ptr<ApplicationContext> &appContext)
    : m_AppContext(appContext), m_Running(true), m_TickCounter(1)
{
    // Publish an initial snapshot so the render thread has something to read.
    SimulationSnapshot init{};
    init.Tick = 0;
    init.Timestamp = TimeNowSec();
    init.ObjectX = 0.0f;
    init.ObjectVX = 0.5f;
    m_AppContext->LatestSnapshot.store(init);
}

void GameThread::RunLoop()
{
	m_GameLib = LoadGameLibrary("GameCode.dll");
    GameState gameState{};
	gameState.PlatformInputHandle = &m_AppContext->InputRing;

    filewatch::FileWatch<std::string> fw("Game.dll",
        std::regex("Game\\.dll", std::regex_constants::icase),
        [this](const std::string& file, const filewatch::Event eventType)
        {
            if (eventType == filewatch::Event::modified) {
                SM_TRACE("Game.dll modified, reloading...");
				m_LibraryReload.store(true, std::memory_order_relaxed);
            }
		});

	constexpr double targetDt = 1.0 / 60.0;
    auto next = Clock::now();
    while (m_Running.load(std::memory_order_relaxed)
        && !m_AppContext->ShutdownRequested.load(std::memory_order_relaxed))
    {
        const auto start = Clock::now();
        (void)start; // currently unused, but kept for potential profiling

        if (m_LibraryReload.load(std::memory_order_relaxed)) {
			// Perform hot-reload
			FreeGameLibrary();

            // Copy to a temp file to avoid locking issues
            std::string tempFile = "GameCode.dll";
            std::filesystem::copy_file("Game.dll", tempFile, std::filesystem::copy_options::overwrite_existing);

			m_GameLib = LoadGameLibrary(tempFile);
			m_LibraryReload.store(false, std::memory_order_relaxed);
        }

		gameState.DeltaTime = targetDt;

        if (m_GameLib.IsValid()) {
			m_GameLib.Update(&gameState);
        }

        if (gameState.QuitRequested) {
            m_Running.store(false, std::memory_order_relaxed);
			m_AppContext->ShutdownRequested.store(true, std::memory_order_relaxed);
            break;
		}

        ProcessInput();         // drain InputRing (S -> G)
        SimulateStep(targetDt); // advance simulation
        PublishSnapshot();      // publish to SnapshotRing (S -> R)

        // sleep until next tick (simple fixed-step)
        next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(targetDt));
        std::this_thread::sleep_until(next);
    }

    if (m_GameLib.IsValid()) {
		m_GameLib.ExitGame();
    }

    FreeGameLibrary();
}

void GameThread::Stop()
{
    m_Running.store(false, std::memory_order_relaxed);
}

void GameThread::ProcessInput()
{
    InputEvent ev{};
    while (m_AppContext->InputRing.Pop(ev))
    {
        // For demo, we don't do much: we could react to keys or mouse
        if (ev.TypeId == InputEvent::Key && ev.Button == GLFW_KEY_ESCAPE && ev.Action == GLFW_RELEASE)
        {
            //m_AppContext->ShutdownRequested.store(true, std::memory_order_relaxed);
            break;
        }
        // else ignore
    }
}

void GameThread::SimulateStep(double dt)
{
    // Simple 1D motion that bounces in [-1,1]
    m_simX += m_simVX * static_cast<float>(dt);
    if (m_simX > 1.0f) { m_simX = 1.0f; m_simVX = -std::fabs(m_simVX); }
    if (m_simX < -1.0f) { m_simX = -1.0f; m_simVX = std::fabs(m_simVX); }
}

void GameThread::PublishSnapshot()
{
    const uint64_t tick = m_TickCounter++;

    SimulationSnapshot snap{};
    snap.Tick = tick;
    snap.Timestamp = TimeNowSec();
    snap.ObjectX = m_simX;
    snap.ObjectVX = m_simVX;

    // Single-writer seqlock publish
    m_AppContext->LatestSnapshot.store(snap);
}

GameLibrary GameThread::LoadGameLibrary(const std::string_view libraryName)
{
	GameLibrary GameLib{};
	const Library libHandle = LoadLibraryA(libraryName.data());
    if (libHandle == nullptr) {
        SM_ERROR("Failed to load Game.dll")
		return GameLib;
    }
	GameLib.Handle = libHandle;
	GameLib.GetVersion = reinterpret_cast<GameGetVersionFunc>(GetProcAddress(static_cast<HMODULE>(libHandle), "GameGetVersion"));
	GameLib.SetPlatformDebugBreak = reinterpret_cast<GameSetPlatformDebugBreakFunc>(GetProcAddress(static_cast<HMODULE>(libHandle), "GameSetPlatformDebugBreak"));
	GameLib.Update = reinterpret_cast<GameUpdateFunc>(GetProcAddress(static_cast<HMODULE>(libHandle), "GameUpdate"));
	GameLib.Resize = reinterpret_cast<GameResizeFunc>(GetProcAddress(static_cast<HMODULE>(libHandle), "GameResize"));
	GameLib.ExitGame = reinterpret_cast<GameExitFunc>(GetProcAddress(static_cast<HMODULE>(libHandle), "GameExit"));
    if (!GameLib.IsValid()) {
        SM_ERROR("Failed to load Game.dll functions");
        FreeLibrary(static_cast<HMODULE>(libHandle));
    }
	// Set platform debug break function
	GameLib.SetPlatformDebugBreak(platform_debug_break);

    return GameLib;
}

void GameThread::FreeGameLibrary()
{
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
