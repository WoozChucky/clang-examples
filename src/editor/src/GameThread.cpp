#include "GameThread.h"

#include <algorithm>
#include <thread>

#include "lib.h"
#include "Timing.h"
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
    const double TargetDt = 1.0 / 60.0;
    auto next = Clock::now();
    while (m_Running.load(std::memory_order_relaxed)
        && !m_AppContext->ShutdownRequested.load(std::memory_order_relaxed))
    {
        auto start = Clock::now();
        (void)start; // currently unused, but kept for potential profiling

        ProcessInput();         // drain InputRing (S -> G)
        SimulateStep(TargetDt); // advance simulation
        PublishSnapshot();      // publish to SnapshotRing (S -> R)

        // sleep until next tick (simple fixed-step)
        next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(TargetDt));
        std::this_thread::sleep_until(next);
    }
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
        if (ev.TypeId == InputEvent::Key && ev.Button == GLFW_KEY_ESCAPE)
        {
            m_AppContext->ShutdownRequested.store(true, std::memory_order_relaxed);
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
