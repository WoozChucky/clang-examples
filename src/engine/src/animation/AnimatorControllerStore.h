#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <mutex>
#include "Engine.h"          // ENGINE_API
#include "AnimatorController.h"

// Process-wide store of immutable AnimatorController assets, keyed by a stable hash handle
// (AssetKeyHash of "<modelKey>#animctrl"). GameThread-written (Add at load) + GameThread-read
// (evaluator). Entries immutable once added; mutex-guarded. Mirrors AnimationStore.
class ENGINE_API AnimatorControllerStore {
public:
    static AnimatorControllerStore& Instance();
    uint64_t Add(const std::string& key, AnimatorController controller); // de-dup by key
    const AnimatorController* Get(uint64_t handle) const;                // null if unknown
    std::string KeyForHandle(uint64_t handle) const;
    std::vector<std::pair<uint64_t, std::string>> GetAssetList() const;
private:
    AnimatorControllerStore() = default;
    struct Entry { std::string key; AnimatorController controller; };
    mutable std::mutex m_Mutex;
    std::unordered_map<uint64_t, Entry> m_ByHandle;
};
