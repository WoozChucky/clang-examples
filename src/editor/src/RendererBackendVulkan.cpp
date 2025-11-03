#include "RendererBackendVulkan.h"

#include <sstream>

#include <nvrhi/validation.h>

// Define the Vulkan dynamic dispatcher - this needs to occur in exactly one cpp file in the program.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

static std::vector<const char *> stringSetToVector(const std::unordered_set<std::string>& set);
template <typename T>
static std::vector<T> setToVector(const std::unordered_set<T>& set)
{
    std::vector<T> ret;
    for(const auto& s : set)
    {
        ret.push_back(s);
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
    void* userData);

static constexpr uint32_t kComputeQueueIndex = 0;
static constexpr uint32_t kGraphicsQueueIndex = 0;
static constexpr uint32_t kPresentQueueIndex = 0;
static constexpr uint32_t kTransferQueueIndex = 0;

bool RendererBackendVulkan::Init() {

#if defined(_DEBUG)
    enabledExtensions.instance.insert("VK_EXT_debug_report");
    enabledExtensions.layers.insert("VK_LAYER_KHRONOS_validation");
#endif

    m_dynamicLoader = std::make_unique<VulkanDynamicLoader>();

    //TODO(Nuno): This should be retrieved from the platform layer (ie. headless)
    //enabledExtensions.instance.insert(VK_KHR_SURFACE_EXTENSION_NAME);
    //enabledExtensions.instance.insert(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr =
        m_dynamicLoader->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

    if (!glfwVulkanSupported()) {
        SM_ERROR("GLFW vulkan support not supported");
        return false;
    }

    // add any extensions required by GLFW
    uint32_t glfwExtCount;
    const char **glfwExt = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    assert(glfwExt);

    for(uint32_t i = 0; i < glfwExtCount; i++)
    {
        enabledExtensions.instance.insert(std::string(glfwExt[i]));
    }

    std::unordered_set<std::string> requiredExtensions = enabledExtensions.instance;

    // figure out which optional extensions are supported
    for(const auto& instanceExt : vk::enumerateInstanceExtensionProperties())
    {
        const std::string& name = instanceExt.extensionName;
        if (optionalExtensions.instance.contains(name))
        {
            enabledExtensions.instance.insert(name);
        }

        requiredExtensions.erase(name);
    }

    if (!requiredExtensions.empty()) {
        for (const auto& ext : requiredExtensions) {
            SM_ERROR("Vulkan instance extension not supported: %s", ext.c_str());
        }
        SM_ASSERT(false, "Not all required Vulkan instance extensions are supported");
        return false;
    }

    SM_TRACE("Creating Vulkan instance with extensions:");
    for (const auto& ext : enabledExtensions.instance) {
        SM_TRACE("  %s", ext.c_str());
    }

    std::unordered_set<std::string> requiredLayers = enabledExtensions.layers;

    for(const auto& layer : vk::enumerateInstanceLayerProperties())
    {
        const std::string& name = layer.layerName;
        if (enabledExtensions.layers.contains(name))
        {
            enabledExtensions.layers.insert(name);
        }

        requiredLayers.erase(name);
    }

    if (!requiredLayers.empty()) {
        for (const auto& layer : requiredLayers) {
            SM_ERROR("Vulkan layer not supported: %s", layer.c_str());
        }
        SM_ASSERT(false, "Not all required Vulkan layers are supported");
        return false;
    }

    SM_TRACE("Creating Vulkan instance with layers:");
    for (const auto& layer : enabledExtensions.layers) {
        SM_TRACE("  %s", layer.c_str());
    }

    auto instanceExtVec = stringSetToVector(enabledExtensions.instance);
    auto layerVec = stringSetToVector(enabledExtensions.layers);

    auto applicationInfo = vk::ApplicationInfo();

    vk::Result res = vk::enumerateInstanceVersion(&applicationInfo.apiVersion);

    if (res != vk::Result::eSuccess)
    {
        SM_TRACE("Call to vkEnumerateInstanceVersion failed, error code = %s", nvrhi::vulkan::resultToString(VkResult(res)));
        return false;
    }

    const uint32_t minimumVulkanVersion = VK_MAKE_API_VERSION(0, 1, 3, 0);

    // Check if the Vulkan API version is sufficient.
    if (applicationInfo.apiVersion < minimumVulkanVersion)
    {
        SM_TRACE("The Vulkan API version supported on the system (%d.%d.%d) is too low, at least %d.%d.%d is required.",
            VK_API_VERSION_MAJOR(applicationInfo.apiVersion), VK_API_VERSION_MINOR(applicationInfo.apiVersion), VK_API_VERSION_PATCH(applicationInfo.apiVersion),
            VK_API_VERSION_MAJOR(minimumVulkanVersion), VK_API_VERSION_MINOR(minimumVulkanVersion), VK_API_VERSION_PATCH(minimumVulkanVersion));
        return false;
    }

    // Spec says: A non-zero variant indicates the API is a variant of the Vulkan API and applications will typically need to be modified to run against it.
    if (VK_API_VERSION_VARIANT(applicationInfo.apiVersion) != 0)
    {
        SM_TRACE("The Vulkan API supported on the system uses an unexpected variant: %d.", VK_API_VERSION_VARIANT(applicationInfo.apiVersion));
        return false;
    }

    // Create the vulkan instance
    const vk::InstanceCreateInfo info = vk::InstanceCreateInfo()
        .setEnabledLayerCount(static_cast<uint32_t>(layerVec.size()))
        .setPpEnabledLayerNames(layerVec.data())
        .setEnabledExtensionCount(static_cast<uint32_t>(instanceExtVec.size()))
        .setPpEnabledExtensionNames(instanceExtVec.data())
        .setPApplicationInfo(&applicationInfo);

    res = vk::createInstance(&info, nullptr, &m_VulkanInstance);
    if (res != vk::Result::eSuccess)
    {
        SM_TRACE("Failed to create a Vulkan instance, error code = %s", nvrhi::vulkan::resultToString(static_cast<VkResult>(res)));
        return false;
    }

    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_VulkanInstance);

    return true;
}

void RendererBackendVulkan::Shutdown(uint32_t timeoutMs) {
    m_RhiSwapChainBuffers.clear();

    DestroyDeviceAndSwapChain();
}

RendererAPI RendererBackendVulkan::GetAPI() const {
    return RendererAPI::Vulkan;
}

nvrhi::DeviceHandle RendererBackendVulkan::CreateDevice() {
#if defined(_DEBUG)
    InstallDebugCallback();
#endif

    if (m_Settings.swapChainFormat == nvrhi::Format::SRGBA8_UNORM)
        m_Settings.swapChainFormat = nvrhi::Format::SBGRA8_UNORM;
    else if (m_Settings.swapChainFormat == nvrhi::Format::RGBA8_UNORM)
        m_Settings.swapChainFormat = nvrhi::Format::BGRA8_UNORM;

    if (!CreateWindowSurface()) {
        SM_ASSERT(false, "Failed to create Vulkan window surface");
        return nullptr;
    }
    if (!PickPhysicalDevice()) {
        SM_ASSERT(false, "Failed to pick Vulkan physical device");
        return nullptr;
    }
    if (!FindQueueFamilies(m_VulkanPhysicalDevice)) {
        SM_ASSERT(false, "Failed to find Vulkan queue families");
        return nullptr;
    }
    if (!CreateVkDevice()) {
        SM_ASSERT(false, "Failed to create Vulkan logical device");
        return nullptr;
    }

    auto vecInstanceExt = stringSetToVector(enabledExtensions.instance);
    auto vecLayers = stringSetToVector(enabledExtensions.layers);
    auto vecDeviceExt = stringSetToVector(enabledExtensions.device);

    m_DeviceDesc.errorCB = &DefaultMessageCallback::GetInstance();
    m_DeviceDesc.instance = m_VulkanInstance;
    m_DeviceDesc.physicalDevice = m_VulkanPhysicalDevice;
    m_DeviceDesc.device = m_VulkanDevice;
    m_DeviceDesc.graphicsQueue = m_GraphicsQueue;
    m_DeviceDesc.graphicsQueueIndex = m_GraphicsQueueFamily;
    m_DeviceDesc.computeQueue = m_ComputeQueue;
    m_DeviceDesc.computeQueueIndex = m_ComputeQueueFamily;
    m_DeviceDesc.transferQueue = m_TransferQueue;
    m_DeviceDesc.transferQueueIndex = m_TransferQueueFamily;
    m_DeviceDesc.instanceExtensions = vecInstanceExt.data();
    m_DeviceDesc.numInstanceExtensions = vecInstanceExt.size();
    m_DeviceDesc.deviceExtensions = vecDeviceExt.data();
    m_DeviceDesc.numDeviceExtensions = vecDeviceExt.size();
    m_DeviceDesc.bufferDeviceAddressSupported = m_BufferDeviceAddressSupported;
    //m_DeviceDesc.vulkanLibraryName = "";
    m_DeviceDesc.logBufferLifetime = false;

    m_Device = nvrhi::vulkan::createDevice(m_DeviceDesc);

#if defined(_DEBUG)
   return nvrhi::validation::createValidationLayer(m_Device);
#endif

   return m_Device;
}

void RendererBackendVulkan::CreateSwapChain(uint32_t width, uint32_t height) {
    m_Settings.backBufferWidth = width;
    m_Settings.backBufferHeight = height;

    CreateSwapChain();

    size_t const numPresentSemaphores = m_SwapChainImages.size();
    m_PresentSemaphores.reserve(numPresentSemaphores);
    for (uint32_t i = 0; i < numPresentSemaphores; ++i)
    {
        m_PresentSemaphores.push_back(m_VulkanDevice.createSemaphore(vk::SemaphoreCreateInfo()));
    }

    size_t const numAcquireSemaphores = std::max(size_t(m_Settings.maxFramesInFlight),
        m_SwapChainImages.size());
    m_AcquireSemaphores.reserve(numAcquireSemaphores);
    for (uint32_t i = 0; i < numAcquireSemaphores; ++i)
    {
        m_AcquireSemaphores.push_back(m_VulkanDevice.createSemaphore(vk::SemaphoreCreateInfo()));
    }
}

nvrhi::CommandListHandle RendererBackendVulkan::CreateCommandList() {
    if (!m_CommandList) {
        m_CommandList = m_Device->createCommandList();
    }

    return m_CommandList;
}

void RendererBackendVulkan::ResizeSwapChain(uint32_t width, uint32_t height) {
    m_Settings.backBufferWidth = width;
    m_Settings.backBufferHeight = height;
    m_ResizeRequested = true;
}

bool RendererBackendVulkan::BeginFrame() {
    const auto& semaphore = m_AcquireSemaphores[m_AcquireSemaphoreIndex];

    vk::Result res;

    int const maxAttempts = 3;
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        res = m_VulkanDevice.acquireNextImageKHR(
            m_SwapChain,
            std::numeric_limits<uint64_t>::max(), // timeout
            semaphore,
            vk::Fence(),
            &m_SwapChainIndex);

        if ((res == vk::Result::eErrorOutOfDateKHR || res == vk::Result::eSuboptimalKHR || m_ResizeRequested) && attempt < maxAttempts)
        {
            BackBufferResizing();
            auto surfaceCaps = m_VulkanPhysicalDevice.getSurfaceCapabilitiesKHR(m_WindowSurface);

            m_Settings.backBufferWidth = surfaceCaps.currentExtent.width;
            m_Settings.backBufferHeight = surfaceCaps.currentExtent.height;

            ResizeSwapChain();
            BackBufferResized();
        }
        else
            break;
    }

    m_AcquireSemaphoreIndex = (m_AcquireSemaphoreIndex + 1) % m_AcquireSemaphores.size();

    if (res == vk::Result::eSuccess || res == vk::Result::eSuboptimalKHR) // Suboptimal is considered a success
    {
        // Schedule the wait. The actual wait operation will be submitted when the app executes any command list.
        m_Device->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, semaphore, 0);
        return true;
    }

    return false;
}

bool RendererBackendVulkan::Present() {
    const auto& semaphore = m_PresentSemaphores[m_SwapChainIndex];

    m_Device->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, semaphore, 0);

    // NVRHI buffers the semaphores and signals them when something is submitted to a queue.
    // Call 'executeCommandLists' with no command lists to actually signal the semaphore.
    m_Device->executeCommandLists(nullptr, 0);

    vk::PresentInfoKHR info = vk::PresentInfoKHR()
                                .setWaitSemaphoreCount(1)
                                .setPWaitSemaphores(&semaphore)
                                .setSwapchainCount(1)
                                .setPSwapchains(&m_SwapChain)
                                .setPImageIndices(&m_SwapChainIndex);

    const vk::Result res = m_PresentQueue.presentKHR(&info);
    if (!(res == vk::Result::eSuccess || res == vk::Result::eErrorOutOfDateKHR || res == vk::Result::eSuboptimalKHR))
    {
        return false;
    }

#ifndef _WIN32
    if (m_DeviceParams.vsyncEnabled || m_DeviceParams.enableDebugRuntime)
    {
        // according to vulkan-tutorial.com, "the validation layer implementation expects
        // the application to explicitly synchronize with the GPU"
        m_PresentQueue.waitIdle();
    }
#endif

    while (m_FramesInFlight.size() >= m_Settings.maxFramesInFlight)
    {
        auto query = m_FramesInFlight.front();
        m_FramesInFlight.pop();

        m_Device->waitEventQuery(query);

        m_QueryPool.push_back(query);
    }

    nvrhi::EventQueryHandle query;
    if (!m_QueryPool.empty())
    {
        query = m_QueryPool.back();
        m_QueryPool.pop_back();
    }
    else
    {
        query = m_Device->createEventQuery();
    }

    m_Device->resetEventQuery(query);
    m_Device->setEventQuery(query, nvrhi::CommandQueue::Graphics);
    m_FramesInFlight.push(query);
    return true;
}

void RendererBackendVulkan::DestroyDeviceAndSwapChain() {
    DestroySwapChain();

    for (auto& semaphore : m_PresentSemaphores)
    {
        if (semaphore)
        {
            m_VulkanDevice.destroySemaphore(semaphore);
            semaphore = vk::Semaphore();
        }
    }

    for (auto& semaphore : m_AcquireSemaphores)
    {
        if (semaphore)
        {
            m_VulkanDevice.destroySemaphore(semaphore);
            semaphore = vk::Semaphore();
        }
    }

    m_Device = nullptr;
    m_RendererString.clear();

    if (m_VulkanDevice)
    {
        m_VulkanDevice.destroy();
        m_VulkanDevice = nullptr;
    }

    if (m_WindowSurface)
    {
        SM_ASSERT(m_VulkanInstance, "Vulkan instance is null but window surface is not");
        m_VulkanInstance.destroySurfaceKHR(m_WindowSurface);
        m_WindowSurface = nullptr;
    }

    if (m_DebugReportCallback)
    {
        m_VulkanInstance.destroyDebugReportCallbackEXT(m_DebugReportCallback);
    }

    if (m_VulkanInstance)
    {
        m_VulkanInstance.destroy();
        m_VulkanInstance = nullptr;
    }
}

// Private impls
void RendererBackendVulkan::InstallDebugCallback() {
    auto info = vk::DebugReportCallbackCreateInfoEXT()
                    .setFlags(vk::DebugReportFlagBitsEXT::eError |
                              vk::DebugReportFlagBitsEXT::eWarning |
                            //   vk::DebugReportFlagBitsEXT::eInformation |
                              vk::DebugReportFlagBitsEXT::ePerformanceWarning)
                    .setPfnCallback(vulkanDebugCallback)
                    .setPUserData(this);

    vk::Result res = m_VulkanInstance.createDebugReportCallbackEXT(&info, nullptr, &m_DebugReportCallback);
    SM_ASSERT(res == vk::Result::eSuccess, "Failed to create Vulkan debug callback");
}

bool RendererBackendVulkan::CreateWindowSurface() {
    const VkResult res = glfwCreateWindowSurface(m_VulkanInstance, m_Window, nullptr, reinterpret_cast<VkSurfaceKHR *>(&m_WindowSurface));
    if (res != VK_SUCCESS)
    {
        SM_ERROR("Failed to create a GLFW window surface, error code = %s", nvrhi::vulkan::resultToString(res));
        return false;
    }

    return true;
}

bool RendererBackendVulkan::PickPhysicalDevice() {
    VkFormat requestedFormat = nvrhi::vulkan::convertFormat(m_Settings.swapChainFormat);
    vk::Extent2D requestedExtent(m_Settings.backBufferWidth, m_Settings.backBufferHeight);

    auto devices = m_VulkanInstance.enumeratePhysicalDevices();

    int adapterIndex = -1;

    int firstDevice = 0;
    int lastDevice = int(devices.size()) - 1;
    if (adapterIndex >= 0)
    {
        if (adapterIndex > lastDevice)
        {
            SM_ERROR("The specified Vulkan physical device %d does not exist.", adapterIndex);
            return false;
        }
        firstDevice = adapterIndex;
        lastDevice = adapterIndex;
    }

    // Start building an error message in case we cannot find a device.
    std::stringstream errorStream;
    errorStream << "Cannot find a Vulkan device that supports all the required extensions and properties.";

    // build a list of GPUs
    std::vector<vk::PhysicalDevice> discreteGPUs;
    std::vector<vk::PhysicalDevice> otherGPUs;
    for (int deviceIndex = firstDevice; deviceIndex <= lastDevice; ++deviceIndex)
    {
        vk::PhysicalDevice const& dev = devices[deviceIndex];
        vk::PhysicalDeviceProperties prop = dev.getProperties();

        errorStream << std::endl << prop.deviceName.data() << ":";

        // check that all required device extensions are present
        std::unordered_set<std::string> requiredExtensions = enabledExtensions.device;
        auto deviceExtensions = dev.enumerateDeviceExtensionProperties();
        for(const auto& ext : deviceExtensions)
        {
            requiredExtensions.erase(std::string(ext.extensionName.data()));
        }

        bool deviceIsGood = true;

        if (!requiredExtensions.empty())
        {
            // device is missing one or more required extensions
            for (const auto& ext : requiredExtensions)
            {
                errorStream << std::endl << "  - missing " << ext;
            }
            deviceIsGood = false;
        }

        vk::PhysicalDeviceFeatures2 deviceFeatures2{};
        vk::PhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures{};
        deviceFeatures2.pNext = &dynamicRenderingFeatures;

        dev.getFeatures2(&deviceFeatures2);
        if (!deviceFeatures2.features.samplerAnisotropy)
        {
            // device is a toaster oven
            errorStream << std::endl << "  - does not support samplerAnisotropy";
            deviceIsGood = false;
        }
        if (!deviceFeatures2.features.textureCompressionBC)
        {
            errorStream << std::endl << "  - does not support textureCompressionBC";
            deviceIsGood = false;
        }
        if (!dynamicRenderingFeatures.dynamicRendering)
        {
            errorStream << std::endl << "  - does not support dynamicRendering";
            deviceIsGood = false;
        }

        if (!FindQueueFamilies(dev))
        {
            // device doesn't have all the queue families we need
            errorStream << std::endl << "  - does not support the necessary queue types";
            deviceIsGood = false;
        }

        if (deviceIsGood && m_WindowSurface)
        {
            bool surfaceSupported = dev.getSurfaceSupportKHR(m_PresentQueueFamily, m_WindowSurface);
            if (!surfaceSupported)
            {
                errorStream << std::endl << "  - does not support the window surface";
                deviceIsGood = false;
            }
            else
            {
                // check that this device supports our intended swap chain creation parameters
                auto surfaceCaps = dev.getSurfaceCapabilitiesKHR(m_WindowSurface);
                auto surfaceFmts = dev.getSurfaceFormatsKHR(m_WindowSurface);

                if (surfaceCaps.minImageCount > m_Settings.swapChainBufferCount ||
                    (surfaceCaps.maxImageCount < m_Settings.swapChainBufferCount && surfaceCaps.maxImageCount > 0))
                {
                    errorStream << std::endl << "  - cannot support the requested swap chain image count:";
                    errorStream << " requested " << m_Settings.swapChainBufferCount << ", available " << surfaceCaps.minImageCount << " - " << surfaceCaps.maxImageCount;
                    deviceIsGood = false;
                }

                if (surfaceCaps.minImageExtent.width > requestedExtent.width ||
                    surfaceCaps.minImageExtent.height > requestedExtent.height ||
                    surfaceCaps.maxImageExtent.width < requestedExtent.width ||
                    surfaceCaps.maxImageExtent.height < requestedExtent.height)
                {
                    errorStream << std::endl << "  - cannot support the requested swap chain size:";
                    errorStream << " requested " << requestedExtent.width << "x" << requestedExtent.height << ", ";
                    errorStream << " available " << surfaceCaps.minImageExtent.width << "x" << surfaceCaps.minImageExtent.height;
                    errorStream << " - " << surfaceCaps.maxImageExtent.width << "x" << surfaceCaps.maxImageExtent.height;
                    deviceIsGood = false;
                }

                bool surfaceFormatPresent = false;
                for (const vk::SurfaceFormatKHR& surfaceFmt : surfaceFmts)
                {
                    if (surfaceFmt.format == vk::Format(requestedFormat))
                    {
                        surfaceFormatPresent = true;
                        break;
                    }
                }

                if (!surfaceFormatPresent)
                {
                    // can't create a swap chain using the format requested
                    errorStream << std::endl << "  - does not support the requested swap chain format";
                    deviceIsGood = false;
                }

                // check that we can present from the graphics queue
                uint32_t canPresent = dev.getSurfaceSupportKHR(m_GraphicsQueueFamily, m_WindowSurface);
                if (!canPresent)
                {
                    errorStream << std::endl << "  - cannot present";
                    deviceIsGood = false;
                }
            }
        }

        if (!deviceIsGood)
            continue;

        if (prop.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
        {
            discreteGPUs.push_back(dev);
        }
        else
        {
            otherGPUs.push_back(dev);
        }
    }

    // pick the first discrete GPU if it exists, otherwise the first integrated GPU
    if (!discreteGPUs.empty())
    {
        uint32_t selectedIndex = 0;
#if DONUT_WITH_STREAMLINE
        // Auto select best adapter for streamline features
        if (adapterIndex < 0)
            selectedIndex = StreamlineIntegration::Get().FindBestAdapterVulkan(discreteGPUs);
#endif

        m_VulkanPhysicalDevice = discreteGPUs[selectedIndex];
        return true;
    }

    if (!otherGPUs.empty())
    {
        uint32_t selectedIndex = 0;
#if DONUT_WITH_STREAMLINE
        // Auto select best adapter for streamline features
        if (adapterIndex < 0)
            selectedIndex = StreamlineIntegration::Get().FindBestAdapterVulkan(otherGPUs);
#endif
        m_VulkanPhysicalDevice = otherGPUs[selectedIndex];
        return true;
    }

    SM_ERROR("%s", errorStream.str().c_str());

    return false;
}

bool RendererBackendVulkan::FindQueueFamilies(vk::PhysicalDevice physicalDevice) {
    auto props = physicalDevice.getQueueFamilyProperties();

    for(int i = 0; i < int(props.size()); i++)
    {
        const auto& queueFamily = props[i];

        if (m_GraphicsQueueFamily == -1)
        {
            if (queueFamily.queueCount > 0 &&
                (queueFamily.queueFlags & vk::QueueFlagBits::eGraphics))
            {
                m_GraphicsQueueFamily = i;
            }
        }

        if (m_ComputeQueueFamily == -1)
        {
            if (queueFamily.queueCount > 0 &&
                (queueFamily.queueFlags & vk::QueueFlagBits::eCompute) &&
                !(queueFamily.queueFlags & vk::QueueFlagBits::eGraphics))
            {
                m_ComputeQueueFamily = i;
            }
        }

        if (m_TransferQueueFamily == -1)
        {
            if (queueFamily.queueCount > 0 &&
                (queueFamily.queueFlags & vk::QueueFlagBits::eTransfer) &&
                !(queueFamily.queueFlags & vk::QueueFlagBits::eCompute) &&
                !(queueFamily.queueFlags & vk::QueueFlagBits::eGraphics))
            {
                m_TransferQueueFamily = i;
            }
        }

        if (m_PresentQueueFamily == -1)
        {
            if (queueFamily.queueCount > 0 &&
                glfwGetPhysicalDevicePresentationSupport(m_VulkanInstance, physicalDevice, i))
            {
                m_PresentQueueFamily = i;
            }
        }
    }

    if (m_GraphicsQueueFamily == -1 ||
        m_PresentQueueFamily == -1 ||
        m_ComputeQueueFamily == -1 ||
        m_TransferQueueFamily == -1)
    {
        return false;
    }

    return true;
}

bool RendererBackendVulkan::CreateVkDevice() {
    // figure out which optional extensions are supported
    auto deviceExtensions = m_VulkanPhysicalDevice.enumerateDeviceExtensionProperties();
    for(const auto& ext : deviceExtensions)
    {
        const std::string name = ext.extensionName;
        if (optionalExtensions.device.find(name) != optionalExtensions.device.end())
        {
            if (name == VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME && false)
                continue;

            enabledExtensions.device.insert(name);
        }
    }

    enabledExtensions.device.insert(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    const vk::PhysicalDeviceProperties physicalDeviceProperties = m_VulkanPhysicalDevice.getProperties();
    m_RendererString = std::string(physicalDeviceProperties.deviceName.data());

    bool accelStructSupported = false;
    bool rayPipelineSupported = false;
    bool rayQuerySupported = false;
    bool meshletsSupported = false;
    bool vrsSupported = false;
    bool interlockSupported = false;
    bool barycentricSupported = false;
    bool storage16BitSupported = false;
    bool synchronization2Supported = false;
    bool maintenance4Supported = false;
    bool aftermathSupported = false;
    bool clusterAccelerationStructureSupported = false;
    bool mutableDescriptorTypeSupported = false;

    SM_TRACE("Enabled Vulkan device extensions:");
    for (const auto& ext : enabledExtensions.device)
    {
        SM_TRACE("    %s", ext.c_str());

        if (ext == VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)
            accelStructSupported = true;
        else if (ext == VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)
            rayPipelineSupported = true;
        else if (ext == VK_KHR_RAY_QUERY_EXTENSION_NAME)
            rayQuerySupported = true;
        else if (ext == VK_NV_MESH_SHADER_EXTENSION_NAME)
            meshletsSupported = true;
        else if (ext == VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)
            vrsSupported = true;
        else if (ext == VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME)
            interlockSupported = true;
        else if (ext == VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME)
            barycentricSupported = true;
        else if (ext == VK_KHR_16BIT_STORAGE_EXTENSION_NAME)
            storage16BitSupported = true;
        else if (ext == VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)
            synchronization2Supported = true;
        else if (ext == VK_KHR_MAINTENANCE_4_EXTENSION_NAME)
            maintenance4Supported = true;
        else if (ext == VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME)
            m_SwapChainMutableFormatSupported = true;
        else if (ext == VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME)
            aftermathSupported = true;
        else if (ext == VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME)
            clusterAccelerationStructureSupported = true;
        else if (ext == VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME)
            mutableDescriptorTypeSupported = true;
    }

#define APPEND_EXTENSION(condition, desc) if (condition) { (desc).pNext = pNext; pNext = &(desc); }  // NOLINT(cppcoreguidelines-macro-usage)
    void* pNext = nullptr;

    vk::PhysicalDeviceFeatures2 physicalDeviceFeatures2;
    // Determine support for Buffer Device Address, the Vulkan 1.2 way
    auto bufferDeviceAddressFeatures = vk::PhysicalDeviceBufferDeviceAddressFeatures();
    // Determine support for maintenance4
    auto maintenance4Features = vk::PhysicalDeviceMaintenance4Features();
    // Determine support for aftermath
    auto aftermathPhysicalFeatures = vk::PhysicalDeviceDiagnosticsConfigFeaturesNV();

    // Put the user-provided extension structure at the end of the chain
    APPEND_EXTENSION(true, bufferDeviceAddressFeatures);
    APPEND_EXTENSION(maintenance4Supported, maintenance4Features);
    APPEND_EXTENSION(aftermathSupported, aftermathPhysicalFeatures);

    physicalDeviceFeatures2.pNext = pNext;
    m_VulkanPhysicalDevice.getFeatures2(&physicalDeviceFeatures2);

    std::unordered_set<int> uniqueQueueFamilies = { m_GraphicsQueueFamily };
    uniqueQueueFamilies.insert(m_PresentQueueFamily);
    uniqueQueueFamilies.insert(m_ComputeQueueFamily);
    uniqueQueueFamilies.insert(m_TransferQueueFamily);

    float priority = 1.f;
    std::vector<vk::DeviceQueueCreateInfo> queueDesc;
    queueDesc.reserve(uniqueQueueFamilies.size());
    for(int queueFamily : uniqueQueueFamilies)
    {
        queueDesc.push_back(vk::DeviceQueueCreateInfo()
                                .setQueueFamilyIndex(queueFamily)
                                .setQueueCount(1)
                                .setPQueuePriorities(&priority));
    }

    auto accelStructFeatures = vk::PhysicalDeviceAccelerationStructureFeaturesKHR()
        .setAccelerationStructure(true);
    auto rayPipelineFeatures = vk::PhysicalDeviceRayTracingPipelineFeaturesKHR()
        .setRayTracingPipeline(true)
        .setRayTraversalPrimitiveCulling(true);
    auto rayQueryFeatures = vk::PhysicalDeviceRayQueryFeaturesKHR()
        .setRayQuery(true);
    auto meshletFeatures = vk::PhysicalDeviceMeshShaderFeaturesNV()
        .setTaskShader(true)
        .setMeshShader(true);
    auto interlockFeatures = vk::PhysicalDeviceFragmentShaderInterlockFeaturesEXT()
        .setFragmentShaderPixelInterlock(true);
    auto barycentricFeatures = vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR()
        .setFragmentShaderBarycentric(true);
    auto vrsFeatures = vk::PhysicalDeviceFragmentShadingRateFeaturesKHR()
        .setPipelineFragmentShadingRate(true)
        .setPrimitiveFragmentShadingRate(true)
        .setAttachmentFragmentShadingRate(true);
    auto vulkan13features = vk::PhysicalDeviceVulkan13Features()
        .setDynamicRendering(true)
        .setSynchronization2(synchronization2Supported)
        .setMaintenance4(maintenance4Features.maintenance4);
    auto aftermathFeatures = vk::DeviceDiagnosticsConfigCreateInfoNV()
        .setFlags(vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableResourceTracking
            | vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableShaderDebugInfo
            | vk::DeviceDiagnosticsConfigFlagBitsNV::eEnableShaderErrorReporting);
    auto clusterAccelerationStructureFeatures = vk::PhysicalDeviceClusterAccelerationStructureFeaturesNV()
        .setClusterAccelerationStructure(true);
    auto mutableDescriptorTypeFeatures = vk::PhysicalDeviceMutableDescriptorTypeFeaturesEXT()
        .setMutableDescriptorType(true);
    auto dynamicRenderingFeatures = vk::PhysicalDeviceDynamicRenderingFeatures()
        .setDynamicRendering(true);

    pNext = nullptr;
    APPEND_EXTENSION(accelStructSupported, accelStructFeatures)
    APPEND_EXTENSION(rayPipelineSupported, rayPipelineFeatures)
    APPEND_EXTENSION(rayQuerySupported, rayQueryFeatures)
    APPEND_EXTENSION(meshletsSupported, meshletFeatures)
    APPEND_EXTENSION(vrsSupported, vrsFeatures)
    APPEND_EXTENSION(interlockSupported, interlockFeatures)
    APPEND_EXTENSION(barycentricSupported, barycentricFeatures)
    APPEND_EXTENSION(clusterAccelerationStructureSupported, clusterAccelerationStructureFeatures)
    APPEND_EXTENSION(mutableDescriptorTypeSupported, mutableDescriptorTypeFeatures)
    APPEND_EXTENSION(physicalDeviceProperties.apiVersion >= VK_API_VERSION_1_3, vulkan13features)
    APPEND_EXTENSION(physicalDeviceProperties.apiVersion < VK_API_VERSION_1_3 && maintenance4Supported, maintenance4Features)
    APPEND_EXTENSION(physicalDeviceProperties.apiVersion < VK_API_VERSION_1_3, dynamicRenderingFeatures)

#undef APPEND_EXTENSION

    auto deviceFeatures = vk::PhysicalDeviceFeatures()
        .setShaderImageGatherExtended(true)
        .setSamplerAnisotropy(true)
        .setTessellationShader(true)
        .setTextureCompressionBC(true)
        .setGeometryShader(true)
        .setImageCubeArray(true)
        .setShaderInt16(true)
        .setFillModeNonSolid(true)
        .setFragmentStoresAndAtomics(true)
        .setDualSrcBlend(true)
        .setVertexPipelineStoresAndAtomics(true)
        .setShaderInt64(true)
        .setShaderStorageImageWriteWithoutFormat(true)
        .setShaderStorageImageReadWithoutFormat(true);

    // Add a Vulkan 1.1 structure with default settings to make it easier for apps to modify them
    auto vulkan11features = vk::PhysicalDeviceVulkan11Features()
        .setStorageBuffer16BitAccess(true)
        .setPNext(pNext);

    auto vulkan12features = vk::PhysicalDeviceVulkan12Features()
        .setDescriptorIndexing(true)
        .setRuntimeDescriptorArray(true)
        .setDescriptorBindingPartiallyBound(true)
        .setDescriptorBindingVariableDescriptorCount(true)
        .setTimelineSemaphore(true)
        .setShaderSampledImageArrayNonUniformIndexing(true)
        .setBufferDeviceAddress(bufferDeviceAddressFeatures.bufferDeviceAddress)
        .setShaderSubgroupExtendedTypes(true)
        .setScalarBlockLayout(true)
        .setPNext(&vulkan11features);

    auto layerVec = stringSetToVector(enabledExtensions.layers);
    auto extVec = stringSetToVector(enabledExtensions.device);

    auto deviceDesc = vk::DeviceCreateInfo()
        .setPQueueCreateInfos(queueDesc.data())
        .setQueueCreateInfoCount(uint32_t(queueDesc.size()))
        .setPEnabledFeatures(&deviceFeatures)
        .setEnabledExtensionCount(uint32_t(extVec.size()))
        .setPpEnabledExtensionNames(extVec.data())
        //.setEnabledLayerCount(uint32_t(layerVec.size()))
        //.setPpEnabledLayerNames(layerVec.data())
        .setPNext(&vulkan12features);

    const vk::Result res = m_VulkanPhysicalDevice.createDevice(&deviceDesc, nullptr, &m_VulkanDevice);
    if (res != vk::Result::eSuccess)
    {
        SM_ERROR("Failed to create a Vulkan physical device, error code = %s", nvrhi::vulkan::resultToString(VkResult(res)));
        return false;
    }

    m_VulkanDevice.getQueue(m_GraphicsQueueFamily, kGraphicsQueueIndex, &m_GraphicsQueue);
    m_VulkanDevice.getQueue(m_ComputeQueueFamily, kComputeQueueIndex, &m_ComputeQueue);
    m_VulkanDevice.getQueue(m_TransferQueueFamily, kTransferQueueIndex, &m_TransferQueue);
    m_VulkanDevice.getQueue(m_PresentQueueFamily, kPresentQueueIndex, &m_PresentQueue);

    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_VulkanDevice);

    // remember the bufferDeviceAddress feature enablement
    m_BufferDeviceAddressSupported = vulkan12features.bufferDeviceAddress;

    SM_TRACE("Created Vulkan device: %s", m_RendererString.c_str());

    return true;
}

bool RendererBackendVulkan::CreateSwapChain() {
    DestroySwapChain();

    m_SwapChainFormat = {
        vk::Format(nvrhi::vulkan::convertFormat(m_Settings.swapChainFormat)),
        vk::ColorSpaceKHR::eSrgbNonlinear
    };

    vk::Extent2D extent = vk::Extent2D(m_Settings.backBufferWidth, m_Settings.backBufferHeight);

    std::unordered_set<uint32_t> uniqueQueues = {
        uint32_t(m_GraphicsQueueFamily),
        uint32_t(m_PresentQueueFamily) };

    std::vector<uint32_t> queues = setToVector(uniqueQueues);

    const bool enableSwapChainSharing = queues.size() > 1;

    auto desc = vk::SwapchainCreateInfoKHR()
                    .setSurface(m_WindowSurface)
                    .setMinImageCount(m_Settings.swapChainBufferCount)
                    .setImageFormat(m_SwapChainFormat.format)
                    .setImageColorSpace(m_SwapChainFormat.colorSpace)
                    .setImageExtent(extent)
                    .setImageArrayLayers(1)
                    .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled)
                    .setImageSharingMode(enableSwapChainSharing ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive)
                    .setFlags(m_SwapChainMutableFormatSupported ? vk::SwapchainCreateFlagBitsKHR::eMutableFormat : vk::SwapchainCreateFlagBitsKHR(0))
                    .setQueueFamilyIndexCount(enableSwapChainSharing ? uint32_t(queues.size()) : 0)
                    .setPQueueFamilyIndices(enableSwapChainSharing ? queues.data() : nullptr)
                    .setPreTransform(vk::SurfaceTransformFlagBitsKHR::eIdentity)
                    .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
                    .setPresentMode(m_Settings.vsyncEnabled ? vk::PresentModeKHR::eFifo : vk::PresentModeKHR::eImmediate)
                    .setClipped(true)
                    .setOldSwapchain(nullptr);

    std::vector<vk::Format> imageFormats = { m_SwapChainFormat.format };
    switch(m_SwapChainFormat.format)
    {
        case vk::Format::eR8G8B8A8Unorm:
            imageFormats.push_back(vk::Format::eR8G8B8A8Srgb);
            break;
        case vk::Format::eR8G8B8A8Srgb:
            imageFormats.push_back(vk::Format::eR8G8B8A8Unorm);
            break;
        case vk::Format::eB8G8R8A8Unorm:
            imageFormats.push_back(vk::Format::eB8G8R8A8Srgb);
            break;
        case vk::Format::eB8G8R8A8Srgb:
            imageFormats.push_back(vk::Format::eB8G8R8A8Unorm);
            break;
        default:
            break;
    }

    auto imageFormatListCreateInfo = vk::ImageFormatListCreateInfo()
        .setViewFormats(imageFormats);

    if (m_SwapChainMutableFormatSupported)
        desc.pNext = &imageFormatListCreateInfo;

    const vk::Result res = m_VulkanDevice.createSwapchainKHR(&desc, nullptr, &m_SwapChain);
    if (res != vk::Result::eSuccess)
    {
        SM_ERROR("Failed to create a Vulkan swap chain, error code = %s", nvrhi::vulkan::resultToString(VkResult(res)));
        return false;
    }

    // retrieve swap chain images
    auto images = m_VulkanDevice.getSwapchainImagesKHR(m_SwapChain);
    for(auto image : images)
    {
        SwapChainImage sci;
        sci.image = image;

        nvrhi::TextureDesc textureDesc;
        textureDesc.width = m_Settings.backBufferWidth;
        textureDesc.height = m_Settings.backBufferHeight;
        textureDesc.format = m_Settings.swapChainFormat;
        textureDesc.debugName = "Swap chain image";
        textureDesc.initialState = nvrhi::ResourceStates::Present;
        textureDesc.keepInitialState = true;
        textureDesc.isRenderTarget = true;

        sci.rhiHandle = m_Device->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, nvrhi::Object(sci.image), textureDesc);
        m_SwapChainImages.push_back(sci);
    }

    m_SwapChainIndex = 0;

    return true;
}

void RendererBackendVulkan::DestroySwapChain() {
    if (m_VulkanDevice)
    {
        m_VulkanDevice.waitIdle();
    }

    if (m_SwapChain)
    {
        m_VulkanDevice.destroySwapchainKHR(m_SwapChain);
        m_SwapChain = nullptr;
    }

    m_SwapChainImages.clear();
}

void RendererBackendVulkan::ResizeSwapChain() {
    if (m_VulkanDevice)
    {
        DestroySwapChain();
        CreateSwapChain();
    }
}

void RendererBackendVulkan::BackBufferResizing() {
    m_SwapChainFramebuffers.clear();
}

void RendererBackendVulkan::BackBufferResized() {
    uint32_t backBufferCount = GetBackBufferCount();
    m_SwapChainFramebuffers.resize(backBufferCount);
    for (uint32_t index = 0; index < backBufferCount; index++)
    {
        m_SwapChainFramebuffers[index] = m_Device->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(GetBackBuffer(index)));
    }
}

nvrhi::ITexture* RendererBackendVulkan::GetCurrentBackBuffer()
{
    return m_SwapChainImages[m_SwapChainIndex].rhiHandle;
}
nvrhi::ITexture* RendererBackendVulkan::GetBackBuffer(uint32_t index)
{
    if (index < m_SwapChainImages.size())
        return m_SwapChainImages[index].rhiHandle;
    return nullptr;
}
uint32_t RendererBackendVulkan::GetCurrentBackBufferIndex()
{
    return m_SwapChainIndex;
}
uint32_t RendererBackendVulkan::GetBackBufferCount()
{
    return uint32_t(m_SwapChainImages.size());
}

uint32_t * RendererBackendVulkan::GetFrameIndexPtr() {
    return &m_FrameIndex;
}

nvrhi::IFramebuffer * RendererBackendVulkan::GetFrameBuffer(int32_t index) {
    if (index < 0) {
        index = static_cast<int32_t>(GetCurrentBackBufferIndex());
    }
    return m_SwapChainFramebuffers[index];
}

// Static impls

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
    fprintf(stderr, "[Vulkan: location=0x%zx code=%d, layerPrefix='%s'] %s", location, code, layerPrefix, msg);
    // SM_WARN("[Vulkan: location=0x%zx code=%d, layerPrefix='%s'] %s", location, code, layerPrefix, msg);

    return VK_FALSE;
}

static std::vector<const char *> stringSetToVector(const std::unordered_set<std::string>& set)
{
    std::vector<const char *> ret;
    for(const auto& s : set)
    {
        ret.push_back(s.c_str());
    }

    return ret;
}
