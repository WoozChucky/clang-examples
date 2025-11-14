#pragma once

#include <nvrhi/nvrhi.h>
#include "ApplicationContext.h"

// Forward declarations
class Renderer;
class FrameAllocator;

// Abstract base class for render passes
class IRenderPass {
public:
    virtual ~IRenderPass() = default;

    // Initialize the render pass (create resources, shaders, pipelines, etc.)
    virtual bool Initialize(nvrhi::IDevice* device, Renderer* renderer) = 0;

    // Render the pass
    virtual void Render(
        nvrhi::ICommandList* commandList,
        nvrhi::IFramebuffer* frameBuffer,
        SimulationSnapshot& snapshot,
        double deltaTime,
        FrameAllocator* frameAllocator) = 0;

    // Cleanup resources
    virtual void Shutdown() = 0;

    // Handle window resize
    virtual void OnResize(uint32_t width, uint32_t height) = 0;
};
