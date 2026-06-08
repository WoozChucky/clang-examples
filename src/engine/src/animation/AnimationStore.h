#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <mutex>
#include "Engine.h"          // ENGINE_API
#include "AnimationClip.h"

// Process-wide store of immutable AnimationClip assets, keyed by a stable hash handle
// (AssetKeyHash of "<modelKey>#anim/<name>"). GameThread-written (Add at load) + GameThread-read
// (sampling). Entries immutable once added; the container mutates across loads so a mutex guards
// access. Mirrors SkeletonStore.
class ENGINE_API AnimationStore {
public:
    static AnimationStore& Instance();
    uint64_t Add(const std::string& key, AnimationClip clip);   // de-dup by key
    const AnimationClip* Get(uint64_t handle) const;            // null if unknown
    std::string KeyForHandle(uint64_t handle) const;
    std::vector<std::pair<uint64_t, std::string>> GetAssetList() const;
private:
    AnimationStore() = default;
    struct Entry { std::string key; AnimationClip clip; };
    mutable std::mutex m_Mutex;
    std::unordered_map<uint64_t, Entry> m_ByHandle;
};
