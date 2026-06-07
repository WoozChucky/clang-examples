#include "RenderThread.h"

#include <windows.h>   // MessageBoxA, ExitProcess, UINT

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "lib.h"
#include "Timing.h"
#include "StagingBufferPool.h"

#include <tracy/Tracy.hpp>

RenderThread::RenderThread(const std::shared_ptr<ApplicationContext> &appContext, GLFWwindow* window, RendererAPI api, OverlayFactory overlayFactory)
    : m_AppContext(appContext), m_Window(window), m_Running(true), m_API(api), m_OverlayFactory(std::move(overlayFactory))
{
}

void RenderThread::RunLoop()
{
    tracy::SetThreadName("RenderThread");

    if (!Initialize())
    {
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
        ZoneScopedN("Render");
        RendererCommand cmd{};
        // Platform -> Render commands
        while (m_AppContext->PRCommandRing.Pop(cmd)) {
            switch (cmd.Type) {
                case RendererCommandType::ToggleVSync:
                    m_Renderer->ToggleVSync();
                    SM_TRACE("VSync toggled");
                    break;
                case RendererCommandType::Resize:
                    m_Renderer->Resize(cmd.ResizeParams.Width, cmd.ResizeParams.Height);
                    break;
                case RendererCommandType::SwapBackend: {
                    if (m_AppContext->SwapInProgress.load(std::memory_order_acquire)) {
                        SM_WARN("RenderThread: swap already in progress; dropping duplicate");
                        break;
                    }
                    const RendererAPI target = cmd.SwapBackend.TargetApi;
                    SM_TRACE("RenderThread: SwapBackend -> %d requested", static_cast<int>(target));

                    // 1. Signal GameThread to pause and wait for its ack (5 s timeout).
                    m_AppContext->SwapInProgress.store(true, std::memory_order_release);
                    {
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                        while (!m_AppContext->GameThreadPaused.load(std::memory_order_acquire)) {
                            if (std::chrono::steady_clock::now() > deadline) {
                                SM_ERROR("RenderThread: GameThread did not pause in time");
                                MessageBoxA(nullptr,
                                    "GameThread did not pause for renderer swap; forcing exit.",
                                    "Editor - swap failure", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
                                ExitProcess(static_cast<UINT>(-1));
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                    }

                    // 2. Perform the swap.
                    const bool ok = m_Renderer->SwapBackend(target);

                    // 3. Release GameThread regardless (so it can exit cleanly even on failure).
                    m_AppContext->SwapInProgress.store(false, std::memory_order_release);

                    if (!ok) {
                        SM_ERROR("RenderThread: SwapBackend failed (fatal)");
                        MessageBoxA(nullptr,
                            "Failed to initialize the selected renderer backend.\n"
                            "The editor cannot continue. Restart and choose a different\n"
                            "backend via engine_settings.json or --backend=...",
                            "Editor - swap failure", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
                        ExitProcess(static_cast<UINT>(-1));
                    }
                    SM_TRACE("RenderThread: SwapBackend complete");
                    break;
                }
                default:
                    SM_WARN("RenderThread: Unknown command type: %d", static_cast<int>(cmd.Type));
                    break;
            }
        }

        // Game -> Render commands
        while (m_AppContext->GRCommandRing.Pop(cmd)) {
            switch (cmd.Type) {
                case RendererCommandType::RequestMesh: {
                    SM_TRACE("Mesh Requested (ticket %llu)", cmd.TicketId);

                    const auto meshHandle = m_Renderer->AddMesh(
                        std::string(cmd.MeshRequest.Key),
                        cmd.MeshRequest.Vertices, static_cast<uint32_t>(cmd.MeshRequest.VertexCount),
                        cmd.MeshRequest.Indices, static_cast<uint32_t>(cmd.MeshRequest.IndexCount),
                        cmd.MeshRequest.SubMeshes, static_cast<uint32_t>(cmd.MeshRequest.SubMeshCount)
                    );

                    // Return staging buffers to the pool
                    if (cmd.MeshRequest.Vertices) GetStagingPool().Return(cmd.MeshRequest.Vertices);
                    if (cmd.MeshRequest.Indices) GetStagingPool().Return(cmd.MeshRequest.Indices);
                    if (cmd.MeshRequest.SubMeshes) GetStagingPool().Return(cmd.MeshRequest.SubMeshes);

                    RendererResponse response{};
                    response.Type = RendererResponseType::MeshUpload;
                    response.TicketId = cmd.TicketId;
                    response.Mesh.Valid = meshHandle.Index != UINT64_MAX;
                    response.Mesh.Handle = meshHandle;
                    if (!m_AppContext->RGCommandRing.Push(response)) {
                        SM_ERROR("RenderThread: Failed to push MeshUpload response to ring");
                    }
                    break;
                }
                case RendererCommandType::RequestMaterial: {
                    SM_TRACE("Material Requested (ticket %llu)", (unsigned long long)cmd.TicketId);

                    if (cmd.MaterialRequest.Texture == nullptr) {
                        SM_WARN("RenderThread: Material request has null texture pointer (ticket %llu)", (unsigned long long)cmd.TicketId);
                        break;
                    }

                    const auto materialHandle = m_Renderer->AddMaterial(
                        cmd.MaterialRequest.Texture,
                        cmd.MaterialRequest.Width,
                        cmd.MaterialRequest.Height
                    );

                    // Return staging buffer to the pool
                    if (cmd.MaterialRequest.Texture) GetStagingPool().Return(cmd.MaterialRequest.Texture);

                    RendererResponse response{};
                    response.Type = RendererResponseType::MaterialUpload;
                    response.TicketId = cmd.TicketId;
                    response.Material.Valid = materialHandle.Index != UINT64_MAX;
                    response.Material.Handle = materialHandle;
                    if (!m_AppContext->RGCommandRing.Push(response)) {
                        SM_ERROR("RenderThread: Failed to push MaterialUpload response to ring");
                    }
                    break;
                }
                default:
                    SM_WARN("RenderThread: Unknown command type: %d", static_cast<int>(cmd.Type));
                    break;
            }
        }

        // Game.dll reload barrier: pause here holding NO snapshot, so the GameThread can
        // destroy game-defined component arrays before FreeLibrary. Checked after the command
        // drains and before snapshot acquire, so the previous frame's snapshot is released.
        if (m_AppContext->ReloadInProgress.load(std::memory_order_acquire)) {
            m_AppContext->RenderThreadPausedForReload.store(true, std::memory_order_release);
            while (m_AppContext->ReloadInProgress.load(std::memory_order_acquire)
                   && !m_AppContext->ShutdownRequested.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            m_AppContext->RenderThreadPausedForReload.store(false, std::memory_order_release);
            continue; // re-loop and acquire a fresh (clean) snapshot
        }

        // IMPORTANT: Load ECS snapshot FIRST to acquire reference and prevent deletion
        // This must happen before loading SimulationSnapshot to avoid race condition
        std::shared_ptr<const ECS> worldSnapshot = m_AppContext->LatestWorldSnapshot.load(std::memory_order_acquire);

        // Read latest snapshot from seqlock - retrieved ONCE per render loop
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


        // Render a clear color depending on interpX and mouse X:
        float red = 0.5f + 0.5f * interpX;
        float green = 0.3f + 0.2f * static_cast<float>(std::fmod(1.0 / 640.0, 1.0));
        float blue = 0.2f;

        m_Renderer->Render(renderDelta, red, green, blue, nextSnap, worldSnapshot.get());

        // Advance interpolation baseline
        prevSnap = nextSnap;

        FrameMark;

        // Small yield to avoid starving other threads (not strictly necessary)
        std::this_thread::sleep_for(std::chrono::milliseconds(0));

        // worldSnapshot shared_ptr goes out of scope here, decrementing ref count
    }

    Cleanup();
}

void RenderThread::Stop()
{
    m_Running.store(false, std::memory_order_relaxed);
}

bool RenderThread::Initialize()
{
    m_Renderer = std::make_unique<Renderer>(m_Window, m_AppContext.get());
    if (m_OverlayFactory) {
        m_Renderer->SetOverlay(m_OverlayFactory());
        // Tell PlatformThread an ImGui consumer exists, so it fans input out to the ImGui ring.
        m_AppContext->HasImGuiConsumer.store(true, std::memory_order_relaxed);
    }
    if (!m_Renderer->Init(m_API)) {
        SM_ERROR("RenderThread: Initialize failed");
        return false;
    }
    return true;
}

void RenderThread::Cleanup()
{
    m_Renderer.reset();
}
