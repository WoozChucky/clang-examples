#include "PlatformThread.h"

#include <chrono>
#include <thread>
#include <utility>

#include "lib.h"
#include "Timing.h"

#include <tracy/Tracy.hpp>

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

    glfwSetWindowUserPointer(m_Window, this);

    glfwSetCursorPosCallback(m_Window, [](GLFWwindow* w, const double x, const double y) {
        auto* self = static_cast<PlatformThread *>(glfwGetWindowUserPointer(w));
        self->OnCursorPositionCallback(x, y);
    });

    glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* w, const int button, const int action, const int mods) {
        auto* self = static_cast<PlatformThread *>(glfwGetWindowUserPointer(w));
        self->OnMouseButtonCallback(button, action, mods);
    });

    glfwSetScrollCallback(m_Window, [](GLFWwindow* w, const double x, const double y) {
        auto* self = static_cast<PlatformThread *>(glfwGetWindowUserPointer(w));
        self->OnMouseWheelCallback(x, y);
    });

    glfwSetKeyCallback(m_Window, [](GLFWwindow* w, int key, int scancode, int action, int mods) {
        auto* self = static_cast<PlatformThread *>(glfwGetWindowUserPointer(w));
        self->OnKeyCallback(key, scancode, action, mods);
    });

    glfwSetCharCallback(m_Window, [](GLFWwindow* w, unsigned int code) {
        auto* self = static_cast<PlatformThread *>(glfwGetWindowUserPointer(w));
        self->OnTextInputCallback(code);
    });

    glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* w, int width, int height) {
        auto* self = static_cast<PlatformThread *>(glfwGetWindowUserPointer(w));
        self->OnWindowResizeCallback(width, height);
    });

    return true;
}

void PlatformThread::RunMainLoop() {
    tracy::SetThreadName("MainThread");

    // Main loop: poll events. We keep this responsive and light — push raw events and return quickly.
    while (m_Running.load(std::memory_order_relaxed)
            && !m_AppContext->ShutdownRequested.load(std::memory_order_relaxed)
            && !glfwWindowShouldClose(m_Window)) {
        ZoneScopedN("PollWindowEvents");
        glfwPollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // signal stop if window closed
    m_Running.store(false, std::memory_order_relaxed);
}

void PlatformThread::Stop() {
    m_Running.store(false, std::memory_order_relaxed);
}

void PlatformThread::OnCursorPositionCallback(double x, double y) {
    InputEvent ev{};
    ev.Type = InputEventType::MouseMove;
    ev.Time = TimeNowSec();
    ev.MouseMoveEvent.X = x; ev.MouseMoveEvent.Y = y;
    if (!m_AppContext->InputRing.Push(ev)) {
        // drop if full (in real engine you'd grow ring or backpressure)
        SM_WARN("InputRing full, dropping evt");
    }
    // also update atomic latest input state for late-latch
    const InputState* s = m_AppContext->LatestInputStatePtr.load(std::memory_order_acquire);
    if (!s) s = &m_AppContext->InputStateA; // default
    // Avoid writing to the currently published object: use the spare one pattern
    InputState* spare = (s == &m_AppContext->InputStateA) ? &m_AppContext->InputStateB : &m_AppContext->InputStateA;
    spare->Time = ev.Time;
    spare->MouseX = ev.MouseMoveEvent.X;
    spare->MouseY = ev.MouseMoveEvent.Y;
    m_AppContext->LatestInputStatePtr.store(spare, std::memory_order_release);
}

void PlatformThread::OnMouseButtonCallback(int button, int action, int mods) {
    InputEvent ev{};
    ev.Type = InputEventType::MouseButton;
    ev.Time = TimeNowSec();
    ev.MouseButtonEvent.Button = static_cast<Button>(button);
    ev.MouseButtonEvent.Action = static_cast<InputAction>(action);
    if (!m_AppContext->InputRing.Push(ev)) {
        // drop
        SM_WARN("InputRing full, dropping evt");
    }
    // no late-latch here
}

void PlatformThread::OnMouseWheelCallback(double offsetX, double offsetY) {
    InputEvent ev{};
    ev.Type = InputEventType::MouseWheel;
    ev.Time = TimeNowSec();
    ev.MouseScrollEvent.OffsetX = offsetX;
    ev.MouseScrollEvent.OffsetY = offsetY;
    if (!m_AppContext->InputRing.Push(ev)) {
        // drop
        SM_WARN("InputRing full, dropping evt");
    }
    SM_TRACE("Mouse wheel: offsetX=%.2f offsetY=%.2f", offsetX, offsetY);
}

void PlatformThread::OnKeyCallback(int key, int scancode, int action, int mods) {
    InputEvent ev{};
    ev.Type = InputEventType::Key;
    ev.Time = TimeNowSec();
    ev.KeyEvent.Key = static_cast<KeyCode>(key);
    ev.KeyEvent.Action = static_cast<InputAction>(action);
    ev.KeyEvent.Modifier = static_cast<KeyModifier>(mods);
    if (!m_AppContext->InputRing.Push(ev)) {
        // drop
        SM_WARN("InputRing full, dropping evt");
    }

    if (key == GLFW_KEY_T && action == GLFW_PRESS) {
        // Example: toggle cursor mode on T key
        const int mode = glfwGetInputMode(m_Window, GLFW_CURSOR);
        if (mode == GLFW_CURSOR_NORMAL) {
            glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            SM_TRACE("Cursor disabled");
        } else {
            glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            SM_TRACE("Cursor normal");
        }
    }
}

void PlatformThread::OnTextInputCallback(unsigned int code) {
    InputEvent ev{};
    ev.Type = InputEventType::TextInput;
    ev.Time = TimeNowSec();
    ev.TextEvent.Key = code;
    if (!m_AppContext->InputRing.Push(ev)) {
        // drop
        SM_WARN("InputRing full, dropping evt");
    }
}

void PlatformThread::OnWindowResizeCallback(int width, int height) {
    SM_TRACE("Window resized to %dx%d", width, height);
    RendererCommand cmd{RendererCommandType::Resize};
    cmd.ResizeParams.Width = width;
    cmd.ResizeParams.Height = height;
    if (!m_AppContext->RendererCommandRing.Push(cmd)) {
        SM_WARN("RendererCommandRing full, dropping cmd");
    }
}
