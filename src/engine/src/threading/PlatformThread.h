#pragma once

#include <atomic>
#include <memory>
#include <GLFW/glfw3.h>

#include "Engine.h"
#include "ApplicationContext.h"
#include "Input.h" // InputEventType

class ENGINE_API PlatformThread {
public:
    explicit PlatformThread(const std::shared_ptr<ApplicationContext> &appContext);
    ~PlatformThread();

    bool Init();

    void RunMainLoop();

    void Stop();

    GLFWwindow* GetWindow() const { return m_Window; }

private:
    void OnCursorPositionCallback(double x, double y);
    void OnMouseButtonCallback(int button, int action, int mods);
    void OnMouseWheelCallback(double offsetX, double offsetY);
    void OnKeyCallback(int key, int scancode, int action, int mods);
    void OnTextInputCallback(unsigned int code);
    void OnWindowResizeCallback(int width, int height);

    // Centralizes the game-ring routing decision (cursor-lock + the two GameAccepts* atomics).
    bool ShouldRouteToGame(InputEventType type) const;
    // Fans an event out to the ImGui ring, but only when an ImGui consumer exists to drain it.
    void PushToImGui(const InputEvent& ev) const;
private:
    std::shared_ptr<ApplicationContext> m_AppContext;
    GLFWwindow* m_Window;
    std::atomic<bool> m_Running;
    bool m_CursorLocked = false; // GLFW_CURSOR_DISABLED (play/FPS mode) -> game owns all input
};
