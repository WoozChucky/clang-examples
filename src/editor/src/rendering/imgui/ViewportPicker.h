#pragma once
#include "ECS.h" // EntityId
struct EditorContext;

// Ray-casts the click (screen coords) against every visible mesh entity's world AABB and returns
// the nearest hit, or INVALID_ENTITY if none. Reads camera/entities/bounds/viewport-rect from ctx.
EntityId PickEntity(const EditorContext& ctx, float mouseX, float mouseY);
