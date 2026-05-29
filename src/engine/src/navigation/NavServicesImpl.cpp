#include "navigation/NavServicesImpl.h"

#include "navigation/NavMeshSystem.h"
#include "navigation/NavMesh.h"

namespace {

bool ForwardHasMeshForClass(uint8_t classId) {
    return NavMeshSystem::Instance().Current(classId) != nullptr;
}

void ForwardFindPathForClass(uint8_t classId, const glm::vec3& start, const glm::vec3& end,
                             float maxSearchRadius, std::vector<glm::vec3>* outPath) {
    if (!outPath) return;
    outPath->clear();
    auto nm = NavMeshSystem::Instance().Current(classId);
    if (!nm) return;
    const auto path = nm->FindPath(start, end, maxSearchRadius);
    outPath->reserve(path.size());
    for (const auto& pt : path) outPath->push_back(pt.Position);
}

glm::vec3 ForwardMoveAlongSurfaceForClass(uint8_t classId, const glm::vec3& start,
                                          const glm::vec3& desiredEnd) {
    auto nm = NavMeshSystem::Instance().Current(classId);
    if (!nm) return desiredEnd;
    return nm->ConstrainMove(start, desiredEnd);
}

bool ForwardHasMesh() {
    return ForwardHasMeshForClass(0);
}

void ForwardFindPath(const glm::vec3& start, const glm::vec3& end,
                     float maxSearchRadius, std::vector<glm::vec3>* outPath) {
    ForwardFindPathForClass(0, start, end, maxSearchRadius, outPath);
}

uint32_t ForwardNavVersion() {
    return NavMeshSystem::Instance().GetNavVersion();
}

uint32_t ForwardAddCylinderObstacle(const glm::vec3& pos, float radius, float height) {
    return NavMeshSystem::Instance().AddCylinderObstacle(pos, radius, height);
}

uint32_t ForwardAddBoxObstacle(const glm::vec3& bmin, const glm::vec3& bmax) {
    return NavMeshSystem::Instance().AddBoxObstacle(bmin, bmax);
}

void ForwardRemoveObstacle(uint32_t handle) {
    NavMeshSystem::Instance().RemoveObstacle(handle);
}

void ForwardTrackObstacleForEntity(EntityId e, uint32_t handle) {
    NavMeshSystem::Instance().TrackObstacleForEntity(e, handle);
}

uint32_t ForwardFindObstacleForEntity(EntityId e) {
    return NavMeshSystem::Instance().FindObstacleForEntity(e);
}

void ForwardUntrackEntity(EntityId e) {
    NavMeshSystem::Instance().UntrackEntity(e);
}

glm::vec3 ForwardMoveAlongSurface(const glm::vec3& start, const glm::vec3& desiredEnd) {
    return ForwardMoveAlongSurfaceForClass(0, start, desiredEnd);
}

} // namespace

namespace NavServicesImpl {

void Init(NavServices& out) {
    out.HasMesh                = &ForwardHasMesh;
    out.FindPath               = &ForwardFindPath;
    out.NavVersion             = &ForwardNavVersion;
    out.AddCylinderObstacle    = &ForwardAddCylinderObstacle;
    out.AddBoxObstacle         = &ForwardAddBoxObstacle;
    out.RemoveObstacle         = &ForwardRemoveObstacle;
    out.TrackObstacleForEntity = &ForwardTrackObstacleForEntity;
    out.FindObstacleForEntity  = &ForwardFindObstacleForEntity;
    out.UntrackEntity          = &ForwardUntrackEntity;
    out.MoveAlongSurface       = &ForwardMoveAlongSurface;
    out.HasMeshForClass          = &ForwardHasMeshForClass;
    out.FindPathForClass         = &ForwardFindPathForClass;
    out.MoveAlongSurfaceForClass = &ForwardMoveAlongSurfaceForClass;
}

} // namespace NavServicesImpl
