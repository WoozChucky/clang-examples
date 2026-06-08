#include "animation/AnimatorControllerStore.h"
#include "AssetKey.h"

AnimatorControllerStore& AnimatorControllerStore::Instance() { static AnimatorControllerStore s; return s; }

uint64_t AnimatorControllerStore::Add(const std::string& key, AnimatorController controller, const std::string& sourcePath) {
    const uint64_t handle = AssetKeyHash(key);
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    if (it != m_ByHandle.end()) return handle;   // de-dup: first-load only
    m_ByHandle.emplace(handle, Entry{ key, std::make_shared<const AnimatorController>(std::move(controller)), sourcePath });
    return handle;
}
void AnimatorControllerStore::Reload(const std::string& key, AnimatorController controller) {
    const uint64_t handle = AssetKeyHash(key);
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    if (it == m_ByHandle.end()) {
        m_ByHandle.emplace(handle, Entry{ key, std::make_shared<const AnimatorController>(std::move(controller)), std::string() });
        return;
    }
    it->second.controller = std::make_shared<const AnimatorController>(std::move(controller)); // atomic swap; old refs survive
}
std::shared_ptr<const AnimatorController> AnimatorControllerStore::Get(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? nullptr : it->second.controller;
}
std::string AnimatorControllerStore::KeyForHandle(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? std::string() : it->second.key;
}
std::string AnimatorControllerStore::SourcePathForHandle(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? std::string() : it->second.sourcePath;
}
std::vector<std::pair<uint64_t, std::string>> AnimatorControllerStore::GetAssetList() const {
    std::scoped_lock lk(m_Mutex);
    std::vector<std::pair<uint64_t, std::string>> out;
    out.reserve(m_ByHandle.size());
    for (const auto& [h, e] : m_ByHandle) out.emplace_back(h, e.key);
    return out;
}
