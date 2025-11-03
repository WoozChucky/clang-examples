#include <algorithm>
#include <GLFW/glfw3.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
#include <mutex>

#include "alloc.h"
#include "lib.h"

#include "Renderer.h"

void platform_debug_break(const char* expr, const char* file, int line, const char* message)
{
    char buffer[2048] = {};
    // Compose a detailed message
    sprintf_s(buffer,
            "Assertion failed!\n\nExpression: %s\nFile: %s\nLine: %d\n\n%s",
            (expr ? expr : "<none>"),
            (file ? file : "<unknown>"),
            line,
            (message ? message : "<no message>"));

    MessageBoxA(nullptr, buffer, "Assertion Failed", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);

    // Break into the debugger (still platform-defined macro)
    DEBUG_BREAK();
}

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
static double TimeNowSec() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

// ---------------------------
// Simple types / constants
// ---------------------------
struct InputEvent {
    enum Type : uint8_t { MouseMove = 0, MouseButton = 1, Key = 2 } TypeId;
    double Time; // seconds
    double MouseX, MouseY;
    int Button; // or Keycode
};

struct SimulationSnapshot {
    uint64_t Tick;      // monotonic tick id
    double Timestamp;   // seconds at tick start
    // Minimal renderable state: a single float position for demo (x in [-1..1])
    float ObjectX;
    float ObjectVX;     // velocity for possible extrapolation
};

// Simple SPSC ring buffer (fixed capacity), lock-free producer/consumer.
// Single producer, single consumer.
template<typename T, size_t N>
class SpscRing {
public:
    SpscRing() {
        SM_ASSERT((N & (N - 1)) == 0, "N must be power of two for mask");
        head.store(0);
        tail.store(0);
        data.resize(N);
    }

    // Return true if pushed, false if full
    bool Push(const T& v) {
        uint64_t h = head.load(std::memory_order_relaxed);
        uint64_t t = tail.load(std::memory_order_acquire); // consumer progress
        if ((h - t) >= N) return false; // full
        data[h & mask] = v;
        head.store(h + 1, std::memory_order_release);
        return true;
    }

    // Return true if popped into out, false if empty
    bool Pop(T& out) {
        uint64_t t = tail.load(std::memory_order_relaxed);
        uint64_t h = head.load(std::memory_order_acquire); // producer progress
        if (t == h) return false; // empty
        out = data[t & mask];
        tail.store(t + 1, std::memory_order_release);
        return true;
    }

    // Check emptiness without popping (not essential)
    bool Empty() const {
        return tail.load(std::memory_order_acquire) == head.load(std::memory_order_acquire);
    }

private:
    static constexpr uint64_t mask = N - 1;
    std::vector<T> data;
    std::atomic<uint64_t> head{}; // producer index
    std::atomic<uint64_t> tail{}; // consumer index
};

// ---------------------------
// Global shared primitives
// ---------------------------
static constexpr int InputRingSize = 64; // power-of-two
static SpscRing<InputEvent, InputRingSize> InputRing;

static constexpr int SnapshotRingSize = 3; // triple buffer
static SimulationSnapshot SnapshotRing[SnapshotRingSize];
static std::atomic<uint64_t> LatestPublishedTick{0}; // 0 == none

// Small atomic for late-latch (latest input state snapshot)
struct InputState {
    double Time;
    double MouseX, MouseY;
};
static std::atomic<InputState*> LatestInputStatePtr{nullptr};
// We'll allocate a simple pair of InputState objects and swap pointers to avoid large atomics.
static InputState InputStateA{}, InputStateB{};

static std::atomic<bool> ShutdownRequested{false};

// ---------------------------
// PlatformThread: GLFW + input
// ---------------------------
class PlatformThread {
public:
    PlatformThread() : Window(nullptr), Running(true) {}

    bool Init() {
        if (!glfwInit()) {
            SM_ERROR("glfwInit failed");
            return false;
        }

        // Create window on platform thread (main thread).
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        Window = glfwCreateWindow(1920, 1080, "Three-Thread Demo", nullptr, nullptr);
        if (!Window) {
            SM_ERROR("glfwCreateWindow failed");
            glfwTerminate();
            return false;
        }

        // Install simple callbacks that push InputEvents into InputRing
        glfwSetWindowUserPointer(Window, this);
        glfwSetCursorPosCallback(Window, [](GLFWwindow* w, double x, double y) {
            // example usage for reference: PlatformThread* self = (PlatformThread*)glfwGetWindowUserPointer(w);
            InputEvent ev{};
            ev.TypeId = InputEvent::MouseMove;
            ev.Time = TimeNowSec();
            ev.MouseX = x; ev.MouseY = y;
            if (!InputRing.Push(ev)) {
                // drop if full (in real engine you'd grow ring or backpressure)
                fprintf(stderr, "InputRing full, dropping evt\n");
            }
            // also update atomic latest input state for late-latch
            const InputState* s = LatestInputStatePtr.load(std::memory_order_acquire);
            if (!s) s = &InputStateA; // default
            // Avoid writing to the currently published object: use the spare one pattern
            InputState* spare = (s == &InputStateA) ? &InputStateB : &InputStateA;
            spare->Time = ev.Time;
            spare->MouseX = ev.MouseX;
            spare->MouseY = ev.MouseY;
            LatestInputStatePtr.store(spare, std::memory_order_release);
        });

        glfwSetMouseButtonCallback(Window, [](GLFWwindow* w, int button, int action, int mods) {
            InputEvent ev{};
            ev.TypeId = InputEvent::MouseButton;
            ev.Time = TimeNowSec();
            ev.Button = button;
            if (!InputRing.Push(ev)) {
                // drop
                fprintf(stderr, "InputRing full, dropping evt\n");
            }
            // no late-latch here
        });

        glfwSetKeyCallback(Window, [](GLFWwindow* w, int key, int scancode, int action, int mods) {
            InputEvent ev{};
            ev.TypeId = InputEvent::Key;
            ev.Time = TimeNowSec();
            ev.Button = key;
            if (!InputRing.Push(ev)) {
                // drop
                fprintf(stderr, "InputRing full, dropping evt\n");
            }
        });

        return true;
    }

    void RunMainLoop() {
        // Main loop: poll events. We keep this responsive and light — push raw events and return quickly.
        while (Running.load(std::memory_order_relaxed)
                && !ShutdownRequested.load(std::memory_order_relaxed)
                && !glfwWindowShouldClose(Window)) {
            glfwPollEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // signal stop if window closed
        Running.store(false, std::memory_order_relaxed);

    }

    void Stop() {
        Running.store(false, std::memory_order_relaxed);
    }

    GLFWwindow* GetWindow() const { return Window; }

    ~PlatformThread() {
        if (Window) {
            glfwDestroyWindow(Window);
            Window = nullptr;
        }
        glfwTerminate();
    }

private:

    GLFWwindow* Window;
    std::atomic<bool> Running;
};

// ---------------------------
// GameThread: fixed 60 Hz simulation
// ---------------------------
class GameThread {
public:
    GameThread() : Running(true), TickCounter(1) {
        // Initialize snapshots empty
        for (int i = 0; i < SnapshotRingSize; ++i) {
            SnapshotRing[i].Tick = 0;
            SnapshotRing[i].Timestamp = 0.0;
            SnapshotRing[i].ObjectX = 0.0f;
            SnapshotRing[i].ObjectVX = 0.5f; // move right at 0.5 units/sec
        }
        LatestPublishedTick.store(0, std::memory_order_release);
    }

    void RunLoop() {
        const double TargetDt = 1.0 / 60.0;
        using namespace std::chrono;
        auto next = Clock::now();
        while (Running.load(std::memory_order_relaxed)
                && !ShutdownRequested.load(std::memory_order_relaxed)) {
            auto start = Clock::now();
            ProcessInput();         // drain InputRing (S -> G)
            SimulateStep(TargetDt); // advance simulation
            PublishSnapshot();      // publish to SnapshotRing (S -> R)

            // sleep until next tick (simple fixed-step)
            next += duration_cast<Clock::duration>(duration<double>(TargetDt));
            std::this_thread::sleep_until(next);
        }
    }

    void Stop() {
        Running.store(false, std::memory_order_relaxed);
    }

private:
    void ProcessInput() {
        InputEvent ev{};
        while (InputRing.Pop(ev)) {
            // For demo, we don't do much: we could react to keys or mouse
            if (ev.TypeId == InputEvent::Key && ev.Button == GLFW_KEY_ESCAPE) {
                ShutdownRequested.store(true, std::memory_order_relaxed);
                break;
            }
            // else ignore
        }
    }

    void SimulateStep(double dt) {
        // Simple 1D motion that bounces in [-1,1]
        // We'll mutate a local state and write into the published slot on PublishSnapshot
        simX += simVX * static_cast<float>(dt);
        if (simX > 1.0f) { simX = 1.0f; simVX = -std::fabs(simVX); }
        if (simX < -1.0f) { simX = -1.0f; simVX = std::fabs(simVX); }
    }

    void PublishSnapshot() {
        uint64_t tick = TickCounter++;
        int idx = int(tick % SnapshotRingSize);
        SimulationSnapshot snap{};
        snap.Tick = tick;
        snap.Timestamp = TimeNowSec();
        snap.ObjectX = simX;
        snap.ObjectVX = simVX;
        // Single-writer: safe to memcpy into ring slot
        SnapshotRing[idx] = snap;

        // Release fence to ensure the ring slot write is visible before we publish the tick.
        std::atomic_thread_fence(std::memory_order_release);
        LatestPublishedTick.store(tick, std::memory_order_release);
    }

    std::atomic<bool> Running;
    uint64_t TickCounter;
    float simX{0.0f};
    float simVX{0.5f};
};

class RendererFrontend {

};

// ---------------------------
// RenderThread: vsync-driven rendering and interpolation
// ---------------------------
class RenderThread {
public:
    explicit RenderThread(GLFWwindow* window, RendererAPI api) : m_Window(window), m_Running(true), m_API(api) {}

    void RunLoop() {

        if (!Initialize()) {
            SM_ERROR("RenderThread: Initialize failed");
            return;
        }

        int frame = 0;
        double lastRenderTime = TimeNowSec(); // init once before loop
        const double maxRenderDelta = 0.1;    // clamp large pauses (100 ms)
        const double maxExtrapolationSec = 0.02; // clamp extrapolation to ~1 frame at 60Hz

        while (m_Running.load(std::memory_order_relaxed)
                && !ShutdownRequested.load(std::memory_order_relaxed)) {
            // Poll latest published tick (acquire)
            SimulationSnapshot prevSnap{}, nextSnap{};
            uint64_t published{};
            for (;;) {
                published = LatestPublishedTick.load(std::memory_order_acquire);
                if (published == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }

                uint64_t prevTick = (published > 1) ? (published - 1) : published;
                SimulationSnapshot prev = SnapshotRing[prevTick % SnapshotRingSize];
                SimulationSnapshot next = SnapshotRing[published % SnapshotRingSize];

                // Re-read and retry if producer advanced (prevents reading a slot being overwritten)
                std::atomic_thread_fence(std::memory_order_acquire);
                uint64_t confirm = LatestPublishedTick.load(std::memory_order_acquire);
                if (confirm == published) { prevSnap = prev; nextSnap = next; break; }
            }

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
            InputState* s = LatestInputStatePtr.load(std::memory_order_acquire);
            double mx = 0.0;
            double my = 0.0;
            if (s) { mx = s->MouseX; my = s->MouseY; }

            // Render a clear color depending on interpX and mouse X:
            float red = 0.5f + 0.5f * interpX;
            float green = 0.3f + 0.2f * static_cast<float>(std::fmod(mx / 640.0, 1.0));
            float blue = 0.2f;

            m_Renderer->Render(renderDelta, red, green, blue);

            // Advance frame index
            frame = (frame + 1) % 3;

            // Small yield to avoid starving other threads (not strictly necessary)
            std::this_thread::sleep_for(std::chrono::milliseconds(0));
        }

        Cleanup();
    }

    void Stop() { m_Running.store(false, std::memory_order_relaxed); }

private:

    bool Initialize() {
        m_Renderer = std::make_unique<Renderer>(m_Window);
        if (!m_Renderer->Init(m_API)) {
            SM_ERROR("RenderThread: Initialize failed");
            return false;
        }
        return true;
    }

    void Cleanup() {
        SM_WARN("Dont forget to add resource cleanup when more stuff is added")
    }

    GLFWwindow* m_Window;
    std::atomic<bool> m_Running;
    std::unique_ptr<Renderer> m_Renderer {};
    RendererAPI m_API = RendererAPI::Invalid;
};

// ---------------------------
// Main: assemble threads
// ---------------------------
int main() {
    std::atexit([](){ DumpAllocations(); });
    SM_TRACE("Three-thread GLFW demo starting...");
    // Platform thread is main thread
    PlatformThread platform;
    if (!platform.Init()) return -1;
    GLFWwindow* win = platform.GetWindow();

    // Prepare input state pointers
    LatestInputStatePtr.store(&InputStateA, std::memory_order_release);

    // Start Game thread
    GameThread game;
    std::thread gameThread([&game]() {
        game.RunLoop();
        SM_TRACE("Game thread exiting...");
    });

    // Now start Render thread (it will make context current on its own thread)
    RenderThread renderer(win, RendererAPI::DirectX12);
    std::thread renderThread([&renderer]() {
        renderer.RunLoop();
        SM_TRACE("Render thread exiting...");
    });

    // Run platform main loop (this thread handles OS events)
    platform.RunMainLoop();

    // User closed window -> stop others
    SM_TRACE("Window closed: shutting down...");

    // Signal other threads
    game.Stop();
    renderer.Stop();

    // Join threads
    if (gameThread.joinable()) gameThread.join();
    if (renderThread.joinable()) renderThread.join();

    InputRing.~SpscRing();

    SM_TRACE("Shutdown complete.");
    return 0;
}
