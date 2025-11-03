#pragma once

#include <thread>

#include "ApplicationContext.h"
#include "Timing.h"
#include "GLFW/glfw3.h"

class GameThread {
public:
    explicit GameThread(const std::shared_ptr<ApplicationContext> &appContext);

    void RunLoop();

    void Stop();

private:
    void ProcessInput();

    void SimulateStep(double dt);

    void PublishSnapshot();

    std::shared_ptr<ApplicationContext> m_AppContext;
    std::atomic<bool> m_Running;
    uint64_t m_TickCounter;
    float m_simX{0.0f};
    float m_simVX{0.5f};
};
