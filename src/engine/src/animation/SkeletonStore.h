#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "Engine.h"      // ENGINE_API
#include "Skeleton.h"

// Process-wide store of immutable Skeleton assets, keyed by a stable hash handle
// (AssetKeyHash of the logical key, e.g. "models/x.gltf#skeleton"). GameThread-written (Add, at
// model load), render-read (Get, in DebugRenderPass). Entries are immutable once added, but the
// container mutates across loads while render reads — so a mutex guards both Add and Get. Mirrors
// the NavMeshSystem singleton pattern. Asset counts are tiny, so the lock is negligible.
class ENGINE_API SkeletonStore {
public:
    static SkeletonStore& Instance();

    // De-dups by key (re-adding a key returns the existing handle). Returns the stable handle.
    uint64_t Add(const std::string& key, Skeleton skeleton);
    // Pointer valid for the process lifetime (entries are never erased/mutated). Null if unknown.
    const Skeleton* Get(uint64_t handle) const;
    std::string KeyForHandle(uint64_t handle) const;

private:
    SkeletonStore() = default;
    struct Entry { std::string key; Skeleton skeleton; };
    mutable std::mutex m_Mutex;
    std::unordered_map<uint64_t, Entry> m_ByHandle;
};
