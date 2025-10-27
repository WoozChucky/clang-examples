#pragma once

typedef struct RendererBackend RendererBackend;

class DebugMessageCallback : public nvrhi::IMessageCallback {
public:
    void message(nvrhi::MessageSeverity severity, const char* messageText) override;
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