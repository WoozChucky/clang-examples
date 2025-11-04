#include "PlatformThread.h"

#include <chrono>
#include <thread>
#include <utility>

#include "lib.h"
#include "Timing.h"

PlatformThread::PlatformThread(const std::shared_ptr<ApplicationContext> &appContext)
    : m_AppContext(appContext), m_Window(nullptr), m_Running(true) {

}

PlatformThread::~PlatformThread() {
    if (m_Window) {
        glfwDestroyWindow(m_Window);
        m_Window = nullptr;
    }
    glfwTerminate();
}

bool PlatformThread::Init() {
    if (!glfwInit()) {
        SM_ERROR("glfwInit failed");
        return false;
    }

    // Create window on platform thread (main thread).
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_Window = glfwCreateWindow(1920, 1080, "Three-Thread Demo", nullptr, nullptr);
    if (!m_Window) {
        SM_ERROR("glfwCreateWindow failed");
        glfwTerminate();
        return false;
    }

    // Install simple callbacks that push InputEvents into InputRing
    glfwSetWindowUserPointer(m_Window, this);
    glfwSetCursorPosCallback(m_Window, [](GLFWwindow* w, double x, double y) {
        const auto* self = static_cast<PlatformThread *>(glfwGetWindowUserPointer(w));
        InputEvent ev{};
        ev.TypeId = InputEvent::MouseMove;
        ev.Time = TimeNowSec();
        ev.MouseX = x; ev.MouseY = y;
        if (!self->m_AppContext->InputRing.Push(ev)) {
            // drop if full (in real engine you'd grow ring or backpressure)
            SM_WARN("InputRing full, dropping evt");
        }
        // also update atomic latest input state for late-latch
        const InputState* s = self->m_AppContext->LatestInputStatePtr.load(std::memory_order_acquire);
        if (!s) s = &self->m_AppContext->InputStateA; // default
        // Avoid writing to the currently published object: use the spare one pattern
        InputState* spare = (s == &self->m_AppContext->InputStateA) ? &self->m_AppContext->InputStateB : &self->m_AppContext->InputStateA;
        spare->Time = ev.Time;
        spare->MouseX = ev.MouseX;
        spare->MouseY = ev.MouseY;
        self->m_AppContext->LatestInputStatePtr.store(spare, std::memory_order_release);
    });

    glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* w, int button, int action, int mods) {
        const auto* self = static_cast<PlatformThread *>(glfwGetWindowUserPointer(w));
        InputEvent ev{};
        ev.TypeId = InputEvent::MouseButton;
        ev.Time = TimeNowSec();
        ev.Button = button;
        if (!self->m_AppContext->InputRing.Push(ev)) {
            // drop
            SM_WARN("InputRing full, dropping evt");
        }
        // no late-latch here
    });

    glfwSetKeyCallback(m_Window, [](GLFWwindow* w, int key, int scancode, int action, int mods) {
        const auto* self = static_cast<PlatformThread *>(glfwGetWindowUserPointer(w));
        InputEvent ev{};
        ev.TypeId = InputEvent::Key;
        ev.Time = TimeNowSec();
        ev.Button = key;
        ev.Action = action;
		ev.Mods = mods;
        if (!self->m_AppContext->InputRing.Push(ev)) {
            // drop
            SM_WARN("InputRing full, dropping evt");
        }
    });

    // resize callback
    glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* w, int width, int height) {
        SM_TRACE("Window resized to %dx%d", width, height);
        const auto* self = static_cast<PlatformThread *>(glfwGetWindowUserPointer(w));
        RendererCommand cmd{RendererCommandType::Resize};
        cmd.ResizeParams.Width = width;
        cmd.ResizeParams.Height = height;
        if (!self->m_AppContext->RendererCommandRing.Push(cmd)) {
            SM_WARN("RendererCommandRing full, dropping cmd");
        }
    });

    return true;
}

void PlatformThread::RunMainLoop() {
    // Main loop: poll events. We keep this responsive and light — push raw events and return quickly.
    while (m_Running.load(std::memory_order_relaxed)
            && !m_AppContext->ShutdownRequested.load(std::memory_order_relaxed)
            && !glfwWindowShouldClose(m_Window)) {
        glfwPollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // signal stop if window closed
    m_Running.store(false, std::memory_order_relaxed);
}

void PlatformThread::Stop() {
    m_Running.store(false, std::memory_order_relaxed);
}