#pragma once
#include "IOverlay.h"
#include "imgui/ImGuiRenderer.h"

// Editor overlay: implements the engine's IOverlay by forwarding to ImGuiRenderer.
class ImGuiOverlay final : public IOverlay {
public:
    bool Init(nvrhi::IDevice* device, ApplicationContext* appCtx,
              MeshSystem* meshSystem, MaterialSystem* materialSystem,
              Renderer* renderer) override {
        return m_Impl.Init(device, appCtx, meshSystem, materialSystem, renderer);
    }
    void Render(nvrhi::IFramebuffer* framebuffer, double deltaTime,
                SimulationSnapshot& snapshot, const ECS* world,
                float gpuFrameTimeMs) override {
        m_Impl.Render(framebuffer, deltaTime, snapshot, world, gpuFrameTimeMs);
    }
    void Shutdown() override { m_Impl.Shutdown(); }
    void OnDeviceLost() override { m_Impl.ShutdownNvrhiOnly(); }
    bool OnDeviceReset(nvrhi::IDevice* device) override { return m_Impl.InitNvrhiForDevice(device); }
private:
    ImGuiRenderer m_Impl;
};
