#pragma once

#include <queue>
#include <unordered_set>
#include <string>

#include <vulkan/vulkan.hpp>

#include <nvrhi/vulkan.h>

#include "renderer_common.h"

using nvrhi::RefCountPtr;

#if VK_HEADER_VERSION >= 301
typedef vk::detail::DynamicLoader VulkanDynamicLoader;
#else
typedef vk::DynamicLoader VulkanDynamicLoader;
#endif

struct VulkanExtensionSet
{
    std::unordered_set<std::string> instance;
    std::unordered_set<std::string> layers;
    std::unordered_set<std::string> device;
};

typedef struct RendererBackendVulkan {
    RendererBackendAPI                              m_Api = RendererBackendAPI::Vulkan;
    DebugMessageCallback*                           m_MessageCallback;
    nvrhi::DeviceHandle                             m_Device;
    nvrhi::CommandListHandle                        m_CommandList;
    nvrhi::vulkan::DeviceDesc                       m_DeviceDesc;
    HWND                                            m_hWnd = nullptr;
    bool                                            m_TearingSupported = false;

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

    std::vector<vk::Semaphore> m_AcquireSemaphores;
    std::vector<vk::Semaphore> m_PresentSemaphores;
    uint32_t m_AcquireSemaphoreIndex = 0;

    std::queue<nvrhi::EventQueryHandle> m_FramesInFlight;
    std::vector<nvrhi::EventQueryHandle> m_QueryPool;

    bool m_BufferDeviceAddressSupported = false;

    std::unique_ptr<VulkanDynamicLoader> m_dynamicLoader;
    // End of Vulkan-specific members

    UINT64                                          m_FrameCount = 1;

    RendererBackendSettings                         m_Settings;

    bool                                            m_ResizeRequested;
    double m_AverageFrameTime = 0.0;
    double m_AverageTimeUpdateInterval = 0.5;
    double m_FrameTimeSum = 0.0;
    int m_NumberOfAccumulatedFrames = 0;

    uint32_t m_FrameIndex = 0;

    std::vector<nvrhi::FramebufferHandle> m_SwapChainFramebuffers;

} RendererBackendVulkan;

namespace vulkan {
    void create_internal_instance(RendererBackend* backend);
    nvrhi::DeviceHandle create_device(RendererBackend* backend, void* platform);
    void create_swapchain(RendererBackend* backend, HWND hWnd, int width, int height);
    nvrhi::CommandListHandle create_command_list(RendererBackend* backend);
    void renderer_resize_swapchain(RendererBackend* backend, int width, int height);
    bool renderer_begin_frame(RendererBackend* backend);
    nvrhi::IFramebuffer* renderer_get_framebuffer(RendererBackend* backend, int32_t index = -1);
    bool renderer_present(RendererBackend* backend);
    void renderer_update_avg_frame_time(RendererBackend* backend, double elapsedTime);
    uint32_t* renderer_get_frame_index(RendererBackend* backend);
    void renderer_backend_shutdown(RendererBackend* backend);
}