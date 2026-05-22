#include "ViewportPicker.h"
#include "EditorContext.h"

#include <limits>
#include "ECS.h"
#include "Picking.h"
#include "TransformMath.h"
#include "Frustum.h"        // TransformAABB
#include "MeshSystem.h"     // GetMeshBounds

EntityId PickEntity(const EditorContext& ctx, float mouseX, float mouseY)
{
    // Pick against the SAME ECS that was rendered (ctx.World) for both the camera and the entities,
    // so the ray hits exactly what's on screen (ctx.World and ctx.WorldSnapshot can differ by a frame).
    if (!ctx.World || !ctx.MeshSys || ctx.ViewportW == 0 || ctx.ViewportH == 0)
        return INVALID_ENTITY;

    const auto* cam = ctx.World->GetSingleton<WorldCameraComponent>();
    if (!cam)
        return INVALID_ENTITY;

    const Ray ray = ScreenPointToRay(mouseX, mouseY,
                                     ctx.ViewportMinX, ctx.ViewportMinY,
                                     static_cast<float>(ctx.ViewportW), static_cast<float>(ctx.ViewportH),
                                     cam->View, cam->Projection);

    EntityId best = INVALID_ENTITY;
    float bestT = std::numeric_limits<float>::max();

    ctx.World->Each<TransformComponent, MeshComponent>(
        [&](EntityId e, const TransformComponent& t, const MeshComponent& m)
    {
        if (!m.Visible)
            return;
        const auto bounds = ctx.MeshSys->GetMeshBounds(m.MeshId);
        if (!bounds.valid)
            return;

        glm::vec3 wMin, wMax;
        TransformAABB(ModelMatrix(t), bounds.min, bounds.max, wMin, wMax);

        float tHit = 0.0f;
        if (RayIntersectsAABB(ray, wMin, wMax, tHit) && tHit < bestT)
        {
            bestT = tHit;
            best = e;
        }
    });

    return best;
}
