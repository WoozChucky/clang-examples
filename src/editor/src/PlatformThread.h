#pragma once

#include <atomic>
#include <memory>
#include <GLFW/glfw3.h>

#include "ApplicationContext.h"

class PlatformThread {
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
private:
    std::shared_ptr<ApplicationContext> m_AppContext;
    GLFWwindow* m_Window;
    std::atomic<bool> m_Running;
};