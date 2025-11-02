// ThreeThreadGlfwExample.cpp
// Build (Linux, with glfw installed):
//   g++ ThreeThreadGlfwExample.cpp -std=c++17 -O2 -pthread -lglfw -lGL -o three_thread_example
// On Windows adapt linking to glfw3 and opengl32, and ensure GLEW/GL loader if needed.
// Notes: For macOS, GLFW must be initialized and window created on the main thread (this example does).
//        Also on macOS you may need to link with -framework OpenGL and adjust includes.

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

#include <nvrhi/nvrhi.h>

#include "alloc.h"

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
        static_assert((N & (N - 1)) == 0, "N must be power of two for mask");
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
    std::atomic<uint64_t> head; // producer index
    std::atomic<uint64_t> tail; // consumer index
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

// ---------------------------
// PlatformThread: GLFW + input
// ---------------------------
class PlatformThread {
public:
    PlatformThread() : Window(nullptr), Running(true) {}

    bool Init() {
        if (!glfwInit()) {
            std::cerr << "glfwInit failed\n";
            return false;
        }

        // Create window on platform thread (main thread).
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        // Create a windowed mode window and its OpenGL context
        Window = glfwCreateWindow(640, 480, "Three-Thread Demo", nullptr, nullptr);
        if (!Window) {
            std::cerr << "glfwCreateWindow failed\n";
            glfwTerminate();
            return false;
        }

        // Install simple callbacks that push InputEvents into InputRing
        glfwSetWindowUserPointer(Window, this);
        glfwSetCursorPosCallback(Window, [](GLFWwindow* w, double x, double y) {
            PlatformThread* self = (PlatformThread*)glfwGetWindowUserPointer(w);
            InputEvent ev;
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
            InputEvent ev;
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
            InputEvent ev;
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
        while (Running && !glfwWindowShouldClose(Window)) {
            glfwPollEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // signal stop if window closed
        Running = false;
    }

    void Stop() {
        Running = false;
    }

    GLFWwindow* GetWindow() { return Window; }

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
        while (Running) {
            auto start = Clock::now();
            ProcessInput();         // drain InputRing (S -> G)
            SimulateStep(TargetDt); // advance simulation
            PublishSnapshot();      // publish to SnapshotRing (S -> R)

            // sleep until next tick (simple fixed-step)
            next += duration_cast<Clock::duration>(duration<double>(TargetDt));
            std::this_thread::sleep_until(next);
        }
    }

    void Stop() { Running = false; }

private:
    void ProcessInput() {
        InputEvent ev;
        while (InputRing.Pop(ev)) {
            // For demo, we don't do much: we could react to keys or mouse
            if (ev.TypeId == InputEvent::Key && ev.Button == GLFW_KEY_ESCAPE) {
                // For real program we might signal to quit; omitted here
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
        SimulationSnapshot snap;
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

    bool Running;
    uint64_t TickCounter;
    float simX{0.0f};
    float simVX{0.5f};
};

class RendererFrontend {

};

enum class RendererAPI : uint8_t {
    Invalid,
    DirectX12,
    DirectX11,
    Vulkan,
};

class RendererBackend {
public:
    RendererBackend() = default;
    virtual ~RendererBackend() = default;

    virtual bool Init() = 0;
    virtual void Shutdown(uint32_t timeoutMs) = 0;
    virtual RendererAPI GetAPI() const = 0;
private:
};

class RendererBackendDX12 final : public RendererBackend {
public:
    RendererBackendDX12() = default;
    ~RendererBackendDX12() override = default;
    bool Init() override {
        // DX12-specific initialization code
        return true;
    }
    void Shutdown(uint32_t timeoutMs) override {
        // DX12-specific shutdown code
    }
    RendererAPI GetAPI() const override {
        return RendererAPI::DirectX12;
    }
private:
};

class Renderer {
public:
    explicit Renderer(const RendererAPI api) {
        m_API = api;
    }
    ~Renderer() {
        Shutdown(5000);
    }

    bool Init() {
        switch (m_API) {
            case RendererAPI::DirectX11:
                break;
            case RendererAPI::DirectX12:
                m_Backend = new RendererBackendDX12();
                break;
            case RendererAPI::Vulkan:
                break;
            default:
                // TODO(Nuno): log error
                return false;
        }

        if (!m_Backend) {
            // TODO(Nuno): log error
            return false;
        }

        if (!m_Backend->Init()) {
            delete m_Backend;
            m_Backend = nullptr;
            return false;
        }
        return true;
    }

    void Shutdown(const uint32_t timeoutMs) {
        if (m_Backend) {
            m_Backend->Shutdown(timeoutMs);
        }
        delete m_Backend;
        m_Backend = nullptr;
    }
private:
    RendererAPI         m_API = RendererAPI::Invalid;
    nvrhi::DeviceHandle m_Device = nullptr;
    RendererBackend*    m_Backend = nullptr;
};



// ---------------------------
// RenderThread: vsync-driven rendering and interpolation
// ---------------------------
class RenderThread {
public:
    RenderThread(GLFWwindow* window) : Window(window), Running(true) {}

    void RunLoop() {

        int frame = 0;
        while (Running) {
            // Poll latest published tick (acquire)
            uint64_t published = LatestPublishedTick.load(std::memory_order_acquire);
            if (published == 0) {
                // nothing yet
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // Choose prev/next ticks to interpolate between
            uint64_t nextTick = published;
            uint64_t prevTick = (published > 1) ? (published - 1) : published;

            SimulationSnapshot prevSnap = SnapshotRing[prevTick % SnapshotRingSize];
            SimulationSnapshot nextSnap = SnapshotRing[nextTick % SnapshotRingSize];

            // Acquire fence to ensure we saw fully-written snapshots
            std::atomic_thread_fence(std::memory_order_acquire);

            double now = TimeNowSec();
            double t0 = prevSnap.Timestamp;
            double t1 = nextSnap.Timestamp;
            double alpha = 0.0;
            if (t1 > t0) {
                alpha = (now - t0) / (t1 - t0);
            }
            if (!(alpha >= 0.0)) alpha = 0.0;
            if (alpha > 1.0) alpha = 1.0;

            // Simple linear interpolation of the object position
            float interpX = float((1.0 - alpha) * prevSnap.ObjectX + alpha * nextSnap.ObjectX);

            // Late-latch: sample latest atomic input state (if any)
            InputState* s = LatestInputStatePtr.load(std::memory_order_acquire);
            double mx = 0.0;
            double my = 0.0;
            if (s) { mx = s->MouseX; my = s->MouseY; }

            // Render a clear color depending on interpX and mouse X:
            float red = 0.5f + 0.5f * interpX;
            float green = 0.3f + 0.2f * float(std::fmod(mx / 640.0, 1.0));
            float blue = 0.2f;

            // ... render code

            // Advance frame index
            frame = (frame + 1) % 3;

            // Small yield to avoid starving other threads (not strictly necessary)
            std::this_thread::sleep_for(std::chrono::milliseconds(0));
        }

        // cleanup code
    }

    void Stop() { Running = false; }

private:
    GLFWwindow* Window;
    bool Running;
    Renderer* m_Renderer = nullptr;
};

// ---------------------------
// Main: assemble threads
// ---------------------------
int main() {
    std::atexit([](){ DumpAllocations(); });
    std::cout << "Three-thread GLFW demo starting...\n";
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
    });

    // Now start Render thread (it will make context current on its own thread)
    RenderThread renderer(win);
    std::thread renderThread([&renderer]() {
        renderer.RunLoop();
    });

    // Run platform main loop (this thread handles OS events)
    platform.RunMainLoop();

    // User closed window -> stop others
    std::cout << "Window closed: shutting down...\n";

    // Signal other threads
    game.Stop();
    renderer.Stop();

    // Join threads
    if (gameThread.joinable()) gameThread.join();
    if (renderThread.joinable()) renderThread.join();

    InputRing.~SpscRing();

    std::cout << "Shutdown complete.\n";
    return 0;
}
