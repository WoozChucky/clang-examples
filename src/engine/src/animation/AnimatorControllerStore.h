#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <memory>
#include <mutex>
#include "Engine.h"          // ENGINE_API
#include "AnimatorController.h"

// Process-wide store of AnimatorController assets, keyed by a stable hash handle (AssetKeyHash of
// "<modelKey>#animctrl"). Entries are shared_ptr<const AnimatorController>: the GameThread evaluator
// loads the shared_ptr once per tick (its own ref keeps the graph alive for the tick), so the editor
// can atomically Reload (swap) an entry without tearing the read. Mutex-guarded. Mirrors the engine's
// shared_ptr<const ECS> snapshot pattern.
class ENGINE_API AnimatorControllerStore {
public:
    static AnimatorControllerStore& Instance();
    // First load: de-dup by key (no-op if already present). `sourcePath` = the .animctrl.json the
    // controller was loaded from (so the editor's Save writes back to the same file).
    uint64_t Add(const std::string& key, AnimatorController controller, const std::string& sourcePath = "");
    // Editor write-back: ALWAYS replaces the entry's controller (atomic shared_ptr swap under mutex).
    void     Reload(const std::string& key, AnimatorController controller);
    std::shared_ptr<const AnimatorController> Get(uint64_t handle) const;   // null if unknown
    std::string KeyForHandle(uint64_t handle) const;
    std::string SourcePathForHandle(uint64_t handle) const;                 // "" if unknown/unset
    std::vector<std::pair<uint64_t, std::string>> GetAssetList() const;
private:
    AnimatorControllerStore() = default;
    struct Entry { std::string key; std::shared_ptr<const AnimatorController> controller; std::string sourcePath; };
    mutable std::mutex m_Mutex;
    std::unordered_map<uint64_t, Entry> m_ByHandle;
};
