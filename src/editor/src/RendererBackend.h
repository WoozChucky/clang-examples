#pragma once
#include <lib.h>
#include <nvrhi/nvrhi.h>
#include <GLFW/glfw3.h>

enum class RendererAPI : uint8_t {
    Invalid,
    DirectX12,
    DirectX11,
    Vulkan,
};

typedef struct RendererBackendSettings {
    uint32_t                         refreshRate = 0;
    uint32_t                         swapChainBufferCount = 3;
    nvrhi::Format                    swapChainFormat = nvrhi::Format::SRGBA8_UNORM;
    uint32_t                         swapChainSampleCount = 1;
    uint32_t                         swapChainSampleQuality = 0;
    uint32_t                         maxFramesInFlight = 2;
    uint32_t                         backBufferWidth = 1920;
    uint32_t                         backBufferHeight = 1080;
    bool                             vsyncEnabled = true;
} RendererBackendSettings;

class RendererBackend {
public:
    explicit RendererBackend(const RendererBackendSettings &settings, GLFWwindow* window)
        : m_Settings(settings), m_Window(window)
    {}
    virtual ~RendererBackend() = default;

    virtual bool Init() = 0;
    virtual void Shutdown(uint32_t timeoutMs) = 0;
    [[nodiscard]] virtual RendererAPI GetAPI() const = 0;
    virtual nvrhi::DeviceHandle CreateDevice() = 0;
    virtual void CreateSwapChain(uint32_t width, uint32_t height) = 0;
    virtual nvrhi::CommandListHandle CreateCommandList() = 0;
    virtual void ResizeSwapChain(uint32_t width, uint32_t height) = 0;
    virtual nvrhi::ITexture* GetCurrentBackBuffer() = 0;
    virtual nvrhi::ITexture* GetBackBuffer(uint32_t index) = 0;
    virtual uint32_t GetCurrentBackBufferIndex() = 0;
    virtual uint32_t GetBackBufferCount() = 0;
    virtual uint32_t* GetFrameIndexPtr() = 0;
    virtual nvrhi::IFramebuffer* GetFrameBuffer(int32_t index) = 0;
    virtual bool BeginFrame() = 0;
    virtual bool Present() = 0;
protected:
    constexpr static uint32_t   SHUTDOWN_TIMEOUT = 5000;
    RendererBackendSettings     m_Settings {};
    GLFWwindow*                 m_Window;

    virtual void DestroyDeviceAndSwapChain() = 0;
};

struct DefaultMessageCallback : nvrhi::IMessageCallback {
    static DefaultMessageCallback& GetInstance() {
        static DefaultMessageCallback Instance;
        return Instance;
    }

    static const char* messageFromSeverity(const nvrhi::MessageSeverity severity) {
        switch (severity) {
            case nvrhi::MessageSeverity::Info:
                return "INFO";
            case nvrhi::MessageSeverity::Warning:
                return "WARNING";
            case nvrhi::MessageSeverity::Error:
                return "ERROR";
            case nvrhi::MessageSeverity::Fatal:
                return "FATAL";
            default:
                return "UNKNOWN";
        }
    }

    void message(nvrhi::MessageSeverity severity, const char* messageText) override {
        SM_TRACE("[NVRHI] %s -> %s", messageFromSeverity(severity), messageText);
    }
};