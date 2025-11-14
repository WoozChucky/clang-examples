#pragma once

#include <memory>
#include <atomic>

#include "ApplicationContext.h"
#include "DotNetPluginManager.h"
#include <Game.h>
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
	void PublishSnapshot(const GameState& state, const FrameTimeStats& frameStats);

	std::unique_ptr<DotNetPluginManager> m_PluginManager{nullptr};

	std::shared_ptr<ApplicationContext> m_AppContext;
	std::atomic<bool> m_Running;
	uint64_t m_TickCounter;
	float m_simX{0.0f};
	float m_simVX{0.5f};
};
