#pragma once

#include <memory>

#include "Engine.h"

class  ECS;
class  MeshSystem;
class  NavMesh;
struct NavMeshConfigComponent;

// Engine-side service holding the current navmesh. Build runs on GameThread; the
// resulting shared_ptr is atomic-published so any reader (RenderThread debug viz,
// game-side queries on the same GameThread) can grab a stable snapshot.
class ENGINE_API NavMeshSystem {
public:
    static NavMeshSystem& Instance();

    // Build navmesh from current ECS state. GameThread only.
    // meshSystem may be null — entities tagged Geometry=Mesh will be skipped with SM_WARN.
    void Rebuild(const ECS& world, const NavMeshConfigComponent& cfg, const MeshSystem* meshSystem);

    // Get current navmesh. Any thread. May be null before first build.
    std::shared_ptr<const NavMesh> Current() const;

private:
    NavMeshSystem() = default;
    std::shared_ptr<const NavMesh> m_Current;
};
