#include "renderer_vulkan.h"

#include "lib.h"

#include <nvrhi/validation.h>

static std::vector<const char *> stringSetToVector(const std::unordered_set<std::string>& set)
{
    std::vector<const char *> ret;
    for(const auto& s : set)
    {
        ret.push_back(s.c_str());
    }

    return ret;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL vulkanDebugCallback(
    vk::DebugReportFlagsEXT flags,
    vk::DebugReportObjectTypeEXT objType,
    uint64_t obj,
    size_t location,
    int32_t code,
    const char* layerPrefix,
    const char* msg,
    void* userData)
{
    SM_WARN("[Vulkan: location=0x%zx code=%d, layerPrefix='%s'] %s", location, code, layerPrefix, msg);

    return VK_FALSE;
}

void vulkan::create_internal_instance(RendererBackend* backend) {

    const auto vk = reinterpret_cast<RendererBackendVulkan *>(backend);
    if (!vk) {
        SM_ERROR("RendererBackendVulkan is null");
        return;
    }

#if defined(_DEBUG)
    vk->enabledExtensions.instance.insert("VK_EXT_debug_report");
    vk->enabledExtensions.layers.insert("VK_LAYER_KHRONOS_validation");
#endif

    vk->m_dynamicLoader = std::make_unique<VulkanDynamicLoader>();

    //TODO(Nuno): This should be retrieved from the platform layer (ie. headless)
    vk->enabledExtensions.instance.insert(VK_KHR_SURFACE_EXTENSION_NAME);
    vk->enabledExtensions.instance.insert(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr =
        vk->m_dynamicLoader->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

    std::unordered_set<std::string> requiredExtensions = vk->enabledExtensions.instance;

    // figure out which optional extensions are supported
    for(const auto& instanceExt : vk::enumerateInstanceExtensionProperties())
    {
        const std::string& name = instanceExt.extensionName;
        if (vk->optionalExtensions.instance.contains(name))
        {
            vk->enabledExtensions.instance.insert(name);
        }

        requiredExtensions.erase(name);
    }

    if (!requiredExtensions.empty()) {
        for (const auto& ext : requiredExtensions) {
            SM_ERROR("Vulkan instance extension not supported: %s", ext.c_str());
        }
        SM_ASSERT(false, "Not all required Vulkan instance extensions are supported");
        return;
    }

    SM_TRACE("Creating Vulkan instance with extensions:");
    for (const auto& ext : vk->enabledExtensions.instance) {
        SM_TRACE("  %s", ext.c_str());
    }

    std::unordered_set<std::string> requiredLayers = vk->enabledExtensions.layers;

    for(const auto& layer : vk::enumerateInstanceLayerProperties())
    {
        const std::string& name = layer.layerName;
        if (vk->enabledExtensions.layers.contains(name))
        {
            vk->enabledExtensions.layers.insert(name);
        }

        requiredLayers.erase(name);
    }

    if (!requiredLayers.empty()) {
        for (const auto& layer : requiredLayers) {
            SM_ERROR("Vulkan layer not supported: %s", layer.c_str());
        }
        SM_ASSERT(false, "Not all required Vulkan layers are supported");
        return;
    }

    SM_TRACE("Creating Vulkan instance with layers:");
    for (const auto& layer : vk->enabledExtensions.layers) {
        SM_TRACE("  %s", layer.c_str());
    }

    auto instanceExtVec = stringSetToVector(vk->enabledExtensions.instance);
    auto layerVec = stringSetToVector(vk->enabledExtensions.layers);

    auto applicationInfo = vk::ApplicationInfo();

    vk::Result res = vk::enumerateInstanceVersion(&applicationInfo.apiVersion);

    if (res != vk::Result::eSuccess)
    {
        SM_TRACE("Call to vkEnumerateInstanceVersion failed, error code = %s", nvrhi::vulkan::resultToString(VkResult(res)));
        return;
    }

    const uint32_t minimumVulkanVersion = VK_MAKE_API_VERSION(0, 1, 3, 0);

    // Check if the Vulkan API version is sufficient.
    if (applicationInfo.apiVersion < minimumVulkanVersion)
    {
        SM_TRACE("The Vulkan API version supported on the system (%d.%d.%d) is too low, at least %d.%d.%d is required.",
            VK_API_VERSION_MAJOR(applicationInfo.apiVersion), VK_API_VERSION_MINOR(applicationInfo.apiVersion), VK_API_VERSION_PATCH(applicationInfo.apiVersion),
            VK_API_VERSION_MAJOR(minimumVulkanVersion), VK_API_VERSION_MINOR(minimumVulkanVersion), VK_API_VERSION_PATCH(minimumVulkanVersion));
        return;
    }

    // Spec says: A non-zero variant indicates the API is a variant of the Vulkan API and applications will typically need to be modified to run against it.
    if (VK_API_VERSION_VARIANT(applicationInfo.apiVersion) != 0)
    {
        SM_TRACE("The Vulkan API supported on the system uses an unexpected variant: %d.", VK_API_VERSION_VARIANT(applicationInfo.apiVersion));
        return;
    }

    // Create the vulkan instance
    const vk::InstanceCreateInfo info = vk::InstanceCreateInfo()
        .setEnabledLayerCount(static_cast<uint32_t>(layerVec.size()))
        .setPpEnabledLayerNames(layerVec.data())
        .setEnabledExtensionCount(static_cast<uint32_t>(instanceExtVec.size()))
        .setPpEnabledExtensionNames(instanceExtVec.data())
        .setPApplicationInfo(&applicationInfo);

    res = vk::createInstance(&info, nullptr, &vk->m_VulkanInstance);
    if (res != vk::Result::eSuccess)
    {
        SM_TRACE("Failed to create a Vulkan instance, error code = %s", nvrhi::vulkan::resultToString(static_cast<VkResult>(res)));
        return;
    }

    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk->m_VulkanInstance);
}

void installDebugCallback(RendererBackendVulkan* vk)
{
    auto info = vk::DebugReportCallbackCreateInfoEXT()
                    .setFlags(vk::DebugReportFlagBitsEXT::eError |
                              vk::DebugReportFlagBitsEXT::eWarning |
                            //   vk::DebugReportFlagBitsEXT::eInformation |
                              vk::DebugReportFlagBitsEXT::ePerformanceWarning)
                    .setPfnCallback(vulkanDebugCallback)
                    .setPUserData(vk);

    vk::Result res = vk->m_VulkanInstance.createDebugReportCallbackEXT(&info, nullptr, &vk->m_DebugReportCallback);
    SM_ASSERT(res == vk::Result::eSuccess, "Failed to create Vulkan debug callback");
}

bool createWindowSurface(RendererBackendVulkan* vk, void* platform)
{
    // Here we need to obtain the VkSurfaceKHR from the platform layer (currently this is only Win32)
    vk::Win32SurfaceCreateInfoKHR createInfo = {};
    createInfo.hwnd = static_cast<HWND>(platform);
    createInfo.hinstance = GetModuleHandle(nullptr);

    auto result = vk->m_VulkanInstance.createWin32SurfaceKHR(&createInfo, nullptr, &vk->m_WindowSurface, VULKAN_HPP_DEFAULT_DISPATCHER);
    if (static_cast<VkResult>(result) != VK_SUCCESS) {
        SM_ERROR("Failed to create Vulkan Win32 surface: %d", result);
        return false;
    }

    return true;
}

nvrhi::DeviceHandle vulkan::create_device(RendererBackend* backend, void* platform) {
    const auto vk = reinterpret_cast<RendererBackendVulkan *>(backend);
    if (!vk) {
        SM_ERROR("RendererBackendVulkan is null");
        return nullptr;
    }

#if defined(_DEBUG)
    installDebugCallback(vk);
#endif

    if (vk->m_Settings.swapChainFormat == nvrhi::Format::SRGBA8_UNORM)
        vk->m_Settings.swapChainFormat = nvrhi::Format::SBGRA8_UNORM;
    else if (vk->m_Settings.swapChainFormat == nvrhi::Format::RGBA8_UNORM)
        vk->m_Settings.swapChainFormat = nvrhi::Format::BGRA8_UNORM;

    createWindowSurface(vk, platform);

    // pick physical device
    // find queue families
    // create logical device

    //vk->m_DeviceDesc

    vk->m_Device = nvrhi::vulkan::createDevice(vk->m_DeviceDesc);

#if defined(_DEBUG)
    vk->m_Device = nvrhi::validation::createValidationLayer(vk->m_Device);
#endif

    return vk->m_Device;
}

void vulkan::create_swapchain(RendererBackend* backend, HWND hWnd, int width, int height) {}
nvrhi::CommandListHandle vulkan::create_command_list(RendererBackend* backend) { return nullptr;}
void vulkan::renderer_resize_swapchain(RendererBackend* backend, int width, int height) {}
bool vulkan::renderer_begin_frame(RendererBackend* backend) { return false;}
nvrhi::IFramebuffer* vulkan::renderer_get_framebuffer(RendererBackend* backend, int32_t index) { return nullptr;}
bool vulkan::renderer_present(RendererBackend* backend) { return false;}
void vulkan::renderer_update_avg_frame_time(RendererBackend* backend, double elapsedTime) {}
uint32_t* vulkan::renderer_get_frame_index(RendererBackend* backend) { return nullptr;}
void vulkan::renderer_backend_shutdown(RendererBackend* backend) {}