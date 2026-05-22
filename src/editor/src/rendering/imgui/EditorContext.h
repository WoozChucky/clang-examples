#pragma once
#include <cstdint>
#include <memory>

class ECS;
class MeshSystem;
class MaterialSystem;
class MeshPreviewRenderer;
struct ApplicationContext;
struct SimulationSnapshot;
struct ImDrawList;

// Per-frame dependencies shared by the editor panels. Built once by ImGuiRenderer::Render and
// passed by const& to each panel's Draw. The viewport gizmo fields are filled AFTER the Viewport
// window draws (so the inspector's gizmo can map to the panel) — see ImGuiRenderer::Render.
struct EditorContext {
    ApplicationContext*        App            = nullptr;
    MeshSystem*                MeshSys        = nullptr;
    MaterialSystem*            MatSys         = nullptr;
    MeshPreviewRenderer*       Preview        = nullptr;
    const ECS*                 World          = nullptr;
    std::shared_ptr<const ECS> WorldSnapshot;
    SimulationSnapshot*        Snapshot       = nullptr;
    float                      GpuFrameTimeMs = 0.0f;
    ImDrawList*                ViewportDrawList = nullptr;
    float                      ViewportMinX = 0.0f, ViewportMinY = 0.0f;
    std::uint32_t              ViewportW = 0, ViewportH = 0;
};
