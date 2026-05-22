#pragma once

#include <thread>

#include "ApplicationContext.h"
#include "Renderer.h"
#include "IOverlay.h"


class RenderThread {
public:
    explicit RenderThread(const std::shared_ptr<ApplicationContext> &appContext, GLFWwindow* window, RendererAPI api, OverlayFactory overlayFactory = {});

    void RunLoop();

    void Stop();

private:

    bool Initialize();

    void Cleanup();

    std::shared_ptr<ApplicationContext> m_AppContext;
    GLFWwindow* m_Window;
    std::atomic<bool> m_Running;
    std::unique_ptr<Renderer> m_Renderer {};
    RendererAPI m_API = RendererAPI::Invalid;
    OverlayFactory m_OverlayFactory;
};
