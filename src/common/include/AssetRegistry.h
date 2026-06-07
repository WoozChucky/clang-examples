#pragma once
#include <cstdint>
#include <string>
#include "ECS.h"   // ECS_API

// Resolution seam between a stable logical asset key and the runtime handle. Today backed by
// MeshSystem/MaterialSystem; a future VFS re-backs THIS interface and nothing above it changes.
// Forward resolution (key->handle) is pure AssetKeyHash and does NOT need the registry; the
// registry provides the REVERSE map (handle->key) for serialization + editor display, which only
// the owning systems know. Fn-ptr struct, same idiom as NavServices/EditorUI.
struct AssetRegistry {
    std::string (*MeshKeyForHandle)(uint64_t handle);      // "" if unknown
    std::string (*MaterialKeyForHandle)(uint64_t handle);  // "" if unknown
};

// Process-wide pointer, set by Engine at init (RenderThread owns the systems). Null where no asset
// systems exist (unit tests) — callers fall back gracefully (empty key). Defined in ecs.dll so the
// common-header serializers can reach it.
ECS_API void SetAssetRegistry(const AssetRegistry* reg);
ECS_API const AssetRegistry* GetAssetRegistry();
