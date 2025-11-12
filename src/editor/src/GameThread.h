#pragma once

#include <thread>
#include <memory>
#include <string>
#include <atomic>
#include <filesystem>

#include "ApplicationContext.h"
#include "DotNetPluginHost.h"
#include "DotNetPluginManager.h"
#include "Game.h"
#include "Timing.h"
#include "GLFW/glfw3.h"
#include "FileWatch.h"


using Library = void*;

struct GameLibrary {
	Library Handle{ nullptr };
	GameGetVersionFunc GetVersion{ nullptr };
	GameSetPlatformDebugBreakFunc SetPlatformDebugBreak{ nullptr };
	GameUpdateFunc Update{ nullptr };
	GameResizeFunc Resize{ nullptr };
	GameExitFunc ExitGame{ nullptr };

	[[nodiscard]] bool IsValid() const {
		return Handle != nullptr
		&& GetVersion != nullptr
		&& SetPlatformDebugBreak != nullptr
		&& Update != nullptr
		&& Resize != nullptr
		&& ExitGame != nullptr;
	}
};

class GameThread {
public:
	explicit GameThread(const std::shared_ptr<ApplicationContext> &appContext);

	void RunLoop();

	void Stop();

private:
	bool Running() const;
	// Main loop helpers
	void SimulateStep(double dt);
	void PublishSnapshot(const GameState& state, const FrameTimeStats& frameStats);

	// GameCode hot-reload helpers
	void InitHotReload();
	void SetupGameDllWatcher();
	bool CopyGameDllToTempWithRetry(int maxRetries =40, int delayMs =50);
	bool LoadTempGameLibrary();
	void UnloadGameLibrary();
	void ReloadGameLibraryIfRequested();

	GameLibrary LoadGameLibrary(std::string_view libraryName);
	void FreeGameLibrary();

	std::unique_ptr<DotNetPluginManager> m_PluginManager{nullptr};

	std::shared_ptr<ApplicationContext> m_AppContext;
	std::atomic<bool> m_Running;
	std::atomic<bool> m_LibraryReload{ false };
	uint64_t m_TickCounter;
	float m_simX{0.0f};
	float m_simVX{0.5f};

	GameLibrary m_GameLib;
	std::unique_ptr<filewatch::FileWatch<std::string>> m_GameDllWatch;
};
