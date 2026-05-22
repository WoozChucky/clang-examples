#include "PlatformThread.h"

#include <chrono>
#include <thread>
#include <utility>

#include "lib.h"
#include "Timing.h"
#include "InputRouting.h"

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
    m_Window = glfwCreateWindow(m_AppContext->Settings.windowWidth, m_AppContext->Settings.windowHeight, "Three-Thread Demo", nullptr, nullptr);
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

// Push to game thread (gated: editor routes scene input only when the Viewport wants it;
// cursor-lock/play mode routes everything; ImGui always gets the event regardless).
bool PlatformThread::ShouldRouteToGame(InputEventType type) const
{
    return RouteInputToGame(type, m_CursorLocked,
                            m_AppContext->GameAcceptsMouse.load(std::memory_order_relaxed),
                            m_AppContext->GameAcceptsKeyboard.load(std::memory_order_relaxed));
}

void PlatformThread::OnCursorPositionCallback(double x, double y) {
    InputEvent ev{};
    ev.Type = InputEventType::MouseMove;
    ev.Time = TimeNowSec();
    ev.MouseMoveEvent.X = x; ev.MouseMoveEvent.Y = y;

    // Push to game thread (gated)
    if (ShouldRouteToGame(ev.Type)) {
        if (!m_AppContext->InputRing.Push(ev)) {
            SM_WARN("InputRing full, dropping evt");
        }
    }

    // Push to ImGui (renderer thread)
    if (!m_AppContext->ImGuiInputRing.Push(ev)) {
        SM_WARN("ImGuiInputRing full, dropping evt");
    }
}

void PlatformThread::OnMouseButtonCallback(int button, int action, int mods) {
    InputEvent ev{};
    ev.Type = InputEventType::MouseButton;
    ev.Time = TimeNowSec();
    ev.MouseButtonEvent.Button = static_cast<Button>(button);
    ev.MouseButtonEvent.Action = static_cast<InputAction>(action);

    // Push to game thread (gated)
    if (ShouldRouteToGame(ev.Type)) {
        if (!m_AppContext->InputRing.Push(ev)) {
            SM_WARN("InputRing full, dropping evt");
        }
    }

    // Push to ImGui (renderer thread)
    if (!m_AppContext->ImGuiInputRing.Push(ev)) {
        SM_WARN("ImGuiInputRing full, dropping evt");
    }
}

void PlatformThread::OnMouseWheelCallback(double offsetX, double offsetY) {
    InputEvent ev{};
    ev.Type = InputEventType::MouseWheel;
    ev.Time = TimeNowSec();
    ev.MouseScrollEvent.OffsetX = offsetX;
    ev.MouseScrollEvent.OffsetY = offsetY;

    // Push to game thread (gated)
    if (ShouldRouteToGame(ev.Type)) {
        if (!m_AppContext->InputRing.Push(ev)) {
            SM_WARN("InputRing full, dropping evt");
        }
    }

    // Push to ImGui (renderer thread)
    if (!m_AppContext->ImGuiInputRing.Push(ev)) {
        SM_WARN("ImGuiInputRing full, dropping evt");
    }
}

void PlatformThread::OnKeyCallback(int key, int scancode, int action, int mods) {
    InputEvent ev{};
    ev.Type = InputEventType::Key;
    ev.Time = TimeNowSec();
    ev.KeyEvent.Key = static_cast<KeyCode>(key);
    ev.KeyEvent.Action = static_cast<InputAction>(action);
    ev.KeyEvent.Modifier = static_cast<KeyModifier>(mods);

    // Push to game thread (gated)
    if (ShouldRouteToGame(ev.Type)) {
        if (!m_AppContext->InputRing.Push(ev)) {
            SM_WARN("InputRing full, dropping evt");
        }
    }

    // Push to ImGui (renderer thread)
    if (!m_AppContext->ImGuiInputRing.Push(ev)) {
        SM_WARN("ImGuiInputRing full, dropping evt");
    }

    if (key == GLFW_KEY_T && action == GLFW_PRESS) {
        // Toggle cursor lock: DISABLED = play/FPS mode (game owns all input), NORMAL = editor.
        m_CursorLocked = !m_CursorLocked;
        glfwSetInputMode(m_Window, GLFW_CURSOR,
                         m_CursorLocked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        SM_TRACE(m_CursorLocked ? "Cursor disabled" : "Cursor normal");
    }

    if (key == GLFW_KEY_F5 && action == GLFW_PRESS) {
        SM_TRACE("F5 pressed - toggling VSync");
        RendererCommand cmd{RendererCommandType::ToggleVSync};
        if (!m_AppContext->PRCommandRing.Push(cmd)) {
            SM_WARN("RendererCommandRing full, dropping cmd");
        }
    }
}

void PlatformThread::OnTextInputCallback(unsigned int code) {
    InputEvent ev{};
    ev.Type = InputEventType::TextInput;
    ev.Time = TimeNowSec();
    ev.TextEvent.Key = code;

    // Push to game thread (gated)
    if (ShouldRouteToGame(ev.Type)) {
        if (!m_AppContext->InputRing.Push(ev)) {
            SM_WARN("InputRing full, dropping evt");
        }
    }

    // Push to ImGui (renderer thread)
    if (!m_AppContext->ImGuiInputRing.Push(ev)) {
        SM_WARN("ImGuiInputRing full, dropping evt");
    }
}

void PlatformThread::OnWindowResizeCallback(int width, int height) {
    SM_TRACE("Window resized to %dx%d", width, height);
    RendererCommand cmd{RendererCommandType::Resize};
    cmd.ResizeParams.Width = width;
    cmd.ResizeParams.Height = height;
    if (!m_AppContext->PRCommandRing.Push(cmd)) {
        SM_WARN("RendererCommandRing full, dropping cmd");
    }
}
