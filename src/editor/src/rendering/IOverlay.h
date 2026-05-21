#pragma once
#include <functional>
#include <memory>
#include <nvrhi/nvrhi.h>

struct ApplicationContext;
struct SimulationSnapshot;
class ECS;
class MeshSystem;
class MaterialSystem;
class Renderer;

// An optional, renderer-owned overlay drawn after the gameplay passes and before present.
// The editor supplies an ImGui-backed implementation; a stripped runtime supplies none.
class IOverlay {
public:
    virtual ~IOverlay() = default;
    virtual bool Init(nvrhi::IDevice* device, ApplicationContext* appCtx,
                      MeshSystem* meshSystem, MaterialSystem* materialSystem,
                      Renderer* renderer) = 0;
    virtual void Render(nvrhi::IFramebuffer* framebuffer, double deltaTime,
                        SimulationSnapshot& snapshot, const ECS* world,
                        float gpuFrameTimeMs) = 0;
    virtual void Shutdown() = 0;
    virtual void OnDeviceLost() = 0;                        // hot-swap: drop device-bound state
    virtual bool OnDeviceReset(nvrhi::IDevice* device) = 0; // hot-swap: rebuild against new device
};

// Factory the host supplies so the renderer can create its overlay on the RenderThread.
using OverlayFactory = std::function<std::unique_ptr<IOverlay>()>;
