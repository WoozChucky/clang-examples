#pragma once
#include <cstdint>

#include "Engine.h"

// Per-frame mesh render counters, written by GBufferFillPass once at the end of each frame.
struct RenderStats {
    uint32_t MeshEntitiesTotal  = 0; // entities with Visible==true considered this frame
    uint32_t MeshEntitiesDrawn  = 0; // entries actually submitted (Total - Culled)
    uint32_t MeshEntitiesCulled = 0; // rejected by the frustum test
    uint32_t InstancesDrawn     = 0; // sum of per-batch instance counts emitted
    uint32_t BatchesDrawn       = 0; // draw batches (runs) issued
};

struct CullingSettings { bool Enabled = true; };

struct DebugDrawSettings {
    bool ShowLightGizmos   = false;
    bool ShowCameraFrustum = false;
    bool ShowSelectedAABB  = false;
    bool Wireframe         = false;
    bool ShowGrid          = false;
};

struct ShadowSettings {
    bool  Enabled = true;
    float Bias    = 0.0015f;
};

// Single instances DEFINED in RenderStats.cpp and exported from Engine.dll so the mesh pass
// (Engine.dll) and the editor panel (editor.exe) share ONE copy each. A header-inline
// function-local static would give every module its own copy (the staging-pool bug).
// Both are touched only on the RenderThread (mesh pass writes stats / reads the toggle;
// the ImGui overlay later in the same frame reads stats / writes the toggle) -> no locks.
ENGINE_API RenderStats&     GetRenderStats();
ENGINE_API CullingSettings& GetCullingSettings();
ENGINE_API DebugDrawSettings& GetDebugDrawSettings();
ENGINE_API ShadowSettings& GetShadowSettings();
