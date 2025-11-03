#include "RenderThread.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "lib.h"
#include "Timing.h"

RenderThread::RenderThread(const std::shared_ptr<ApplicationContext> &appContext, GLFWwindow* window, RendererAPI api)
    : m_AppContext(appContext), m_Window(window), m_Running(true), m_API(api)
{
}

void RenderThread::RunLoop()
{
    if (!Initialize())
    {
        SM_ERROR("RenderThread: Initialize failed");
        return;
    }

    double lastRenderTime = TimeNowSec(); // init once before loop
    const double maxRenderDelta = 0.1;    // clamp large pauses (100 ms)
    const double maxExtrapolationSec = 0.02; // clamp extrapolation to ~1 frame at 60Hz

    SimulationSnapshot prevSnap{};
    bool havePrev = false;

    while (m_Running.load(std::memory_order_relaxed)
        && !m_AppContext->ShutdownRequested.load(std::memory_order_relaxed))
    {
        RendererCommand cmd{};
        while (m_AppContext->RendererCommandRing.Pop(cmd)) {
            switch (cmd.Type) {
                case RendererCommandType::Resize:
                    m_Renderer->Resize(cmd.ResizeParams.Width, cmd.ResizeParams.Height);
                    break;
                default:
                    SM_WARN("RenderThread: Unknown command type: %d", static_cast<int>(cmd.Type));
                    break;
            }
        }

        // Read latest snapshot from seqlock
        SimulationSnapshot nextSnap = m_AppContext->LatestSnapshot.load();
        if (!havePrev) { prevSnap = nextSnap; havePrev = true; }

        // Compute render time / delta
        const double now = TimeNowSec();
        const double renderDelta = std::clamp(now - lastRenderTime, 0.0, maxRenderDelta);
        lastRenderTime = now;

        const double t0 = prevSnap.Timestamp;
        const double t1 = nextSnap.Timestamp;
        double alpha = 0.0;
        if (t1 > t0) {
            alpha = (now - t0) / (t1 - t0);
            if (alpha > 1.0) {
                const double extrap = std::min(now - t1, maxExtrapolationSec);
                const double frameSec = (t1 - t0);
                alpha = (frameSec > 0.0) ? 1.0 + (extrap / frameSec) : 1.0;
            }
        }
        // For most interpolation math we clamp alpha to [0,1] and separately handle extrapolation
        const double interpAlpha = std::clamp(alpha, 0.0, 1.0);

        // Simple linear interpolation of the object position
        const auto interpX = static_cast<float>((1.0 - interpAlpha) * prevSnap.ObjectX + interpAlpha * nextSnap.ObjectX);

        // Late-latch: sample latest atomic input state (if any)
        InputState* s = m_AppContext->LatestInputStatePtr.load(std::memory_order_acquire);
        double mx = 0.0;
        double my = 0.0;
        if (s) { mx = s->MouseX; my = s->MouseY; }

        // Render a clear color depending on interpX and mouse X:
        float red = 0.5f + 0.5f * interpX;
        float green = 0.3f + 0.2f * static_cast<float>(std::fmod(mx / 640.0, 1.0));
        float blue = 0.2f;

        m_Renderer->Render(renderDelta, red, green, blue);

        // Advance interpolation baseline
        prevSnap = nextSnap;

        // Small yield to avoid starving other threads (not strictly necessary)
        std::this_thread::sleep_for(std::chrono::milliseconds(0));
    }

    Cleanup();
}

void RenderThread::Stop()
{
    m_Running.store(false, std::memory_order_relaxed);
}

bool RenderThread::Initialize()
{
    m_Renderer = std::make_unique<Renderer>(m_Window);
    if (!m_Renderer->Init(m_API)) {
        SM_ERROR("RenderThread: Initialize failed");
        return false;
    }
    return true;
}

void RenderThread::Cleanup()
{
    SM_WARN("Dont forget to add resource cleanup when more stuff is added");
}
