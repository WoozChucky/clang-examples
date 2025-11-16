#pragma once

#include <queue>
#include <unordered_set>

#include "RendererBackend.h"

#include <nvrhi/vulkan.h>


#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#endif
#include <vulkan/vulkan.hpp>

using nvrhi::RefCountPtr;

class RendererBackendVulkan final : public RendererBackend {
public:
    explicit RendererBackendVulkan(RendererBackendSettings &settings, GLFWwindow* window)
        : RendererBackend(settings, window)
    {}
    ~RendererBackendVulkan() override = default;
    bool Init() override;
    void Shutdown(uint32_t timeoutMs) override;
    [[nodiscard]] RendererAPI GetAPI() const override;
    nvrhi::DeviceHandle CreateDevice() override;
    void CreateSwapChain(uint32_t width, uint32_t height) override;
    void ResizeSwapChain(uint32_t width, uint32_t height) override;
    nvrhi::ITexture* GetCurrentBackBuffer() override;
    nvrhi::ITexture* GetBackBuffer(uint32_t index) override;
    uint32_t GetCurrentBackBufferIndex() override;
    uint32_t GetBackBufferCount() override;
    bool BeginFrame() override;
    bool Present() override;
    nvrhi::ShaderHandle CreateShaderFromMemory(
        nvrhi::ShaderType shaderType,
        const char* content,
        size_t contentSize,
        const char* entryPoint,
        const char* targetName) override;
protected:
    void DestroyDeviceAndSwapChain() override;
    nvrhi::DeviceHandle GetDevice() override;

private:
    void InstallDebugCallback();
    bool CreateWindowSurface();
    bool PickPhysicalDevice();
    bool FindQueueFamilies(vk::PhysicalDevice physicalDevice);
    bool CreateVkDevice();
    bool CreateSwapChain();
    void DestroySwapChain();

    void CreateRenderTargets();
    void ReleaseRenderTargets();
    void ResizeSwapChain();

    struct VulkanExtensionSet
    {
        std::unordered_set<std::string> instance;
        std::unordered_set<std::string> layers;
        std::unordered_set<std::string> device;
    };

private:
    nvrhi::vulkan::DeviceDesc                       m_DeviceDesc;
    bool                                            m_TearingSupported = false;

    std::vector<nvrhi::TextureHandle>               m_RhiSwapChainBuffers;

    UINT64                                          m_FrameCount = 1;
    uint32_t                                        m_FrameIndex = 0;

    std::vector<nvrhi::FramebufferHandle>           m_SwapChainFramebuffers;
    bool                                            m_ResizeRequested = false;

    std::string                                     m_RendererString{};

    vk::Semaphore m_LastAcquireSemaphore{};
    bool m_FrameHadWork = false;

    // Vulkan-specific members
    VulkanExtensionSet enabledExtensions = {
        // instance
        {
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME
        },
        // layers
        { },
        // device
        {
            VK_KHR_MAINTENANCE1_EXTENSION_NAME
        },
    };

    VulkanExtensionSet optionalExtensions = {
        // instance
        {
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
            VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME,
        },
        // layers
        { },
        // device
        {
            VK_EXT_DEBUG_MARKER_EXTENSION_NAME,
            VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
            VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
            VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME,
            VK_KHR_MAINTENANCE_4_EXTENSION_NAME,
            VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME,
            VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
            VK_NV_MESH_SHADER_EXTENSION_NAME,
            VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME,
        }
    };

    std::unordered_set<std::string> m_RayTracingExtensions = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME
    };

    vk::Instance m_VulkanInstance;
    vk::DebugReportCallbackEXT m_DebugReportCallback;

    vk::PhysicalDevice m_VulkanPhysicalDevice;
    int m_GraphicsQueueFamily = -1;
    int m_ComputeQueueFamily = -1;
    int m_TransferQueueFamily = -1;
    int m_PresentQueueFamily = -1;

    vk::Device m_VulkanDevice;
    vk::Queue m_GraphicsQueue;
    vk::Queue m_ComputeQueue;
    vk::Queue m_TransferQueue;
    vk::Queue m_PresentQueue;

    vk::SurfaceKHR m_WindowSurface;

    vk::SurfaceFormatKHR m_SwapChainFormat;
    vk::SwapchainKHR m_SwapChain;
    bool m_SwapChainMutableFormatSupported = false;

    struct SwapChainImage
    {
        vk::Image image;
        nvrhi::TextureHandle rhiHandle;
    };

    std::vector<SwapChainImage> m_SwapChainImages;
    uint32_t m_SwapChainIndex = static_cast<uint32_t>(-1);

    nvrhi::vulkan::DeviceHandle m_NvrhiDevice;
    nvrhi::DeviceHandle m_ValidationLayer;

    std::vector<vk::Semaphore> m_AcquireSemaphores;
    std::vector<vk::Semaphore> m_PresentSemaphores;
    uint32_t m_AcquireSemaphoreIndex = 0;

    std::queue<nvrhi::EventQueryHandle> m_FramesInFlight;
    std::vector<nvrhi::EventQueryHandle> m_QueryPool;

    bool m_BufferDeviceAddressSupported = false;

#if VK_HEADER_VERSION >= 301
    typedef vk::detail::DynamicLoader VulkanDynamicLoader;
#else
    typedef vk::DynamicLoader VulkanDynamicLoader;
#endif

    std::unique_ptr<VulkanDynamicLoader> m_dynamicLoader;
    // End of Vulkan-specific members
};
