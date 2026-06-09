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
    bool ShowColliders     = false;
    bool ShowNavMesh       = false;
    bool ShowObstacles     = false;
    bool ShowNavPaths      = false;
    bool ShowSkeleton      = false;
    int  NavMeshClass      = 0;   // which class mesh ShowNavMesh draws (index into NavMeshConfig classes)
};

struct ShadowSettings {
    bool  Enabled        = true;
    float ShadowDistance = 60.0f;   // frustum-fit far cap (world units); smaller = sharper + shorter range
    float NearExtend     = 50.0f;   // light-space near-plane pull-back toward the sun
    float NormalOffset   = 1.0f;    // normal-offset bias (shadow texels)
    float PcfRadius      = 1.5f;    // Poisson PCF penumbra radius (shadow texels)
};

enum class AoMode : int { Off = 0, SSAO = 1, HBAO = 2, GTAO = 3 };

struct SsaoSettings {
    AoMode Mode     = AoMode::SSAO;
    float Radius    = 0.5f;    // world units
    float Intensity = 1.0f;    // occlusion strength
    float Power     = 2.0f;    // contrast (pow on AO)
    float Bias      = 0.025f;  // view-depth bias to avoid self-occlusion; SSAO-only (HBAO/GTAO ignore)
};

enum class AAMode : int { Off = 0, FXAA = 1, SMAA = 2 };
struct AntiAliasingSettings {
    AAMode Mode = AAMode::FXAA;
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
ENGINE_API SsaoSettings& GetSsaoSettings();
ENGINE_API AntiAliasingSettings& GetAntiAliasingSettings();
