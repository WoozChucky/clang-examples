#include "animation/AnimatorControllerStore.h"
#include "AssetKey.h"

AnimatorControllerStore& AnimatorControllerStore::Instance() { static AnimatorControllerStore s; return s; }

uint64_t AnimatorControllerStore::Add(const std::string& key, AnimatorController controller) {
    const uint64_t handle = AssetKeyHash(key);
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    if (it != m_ByHandle.end()) return handle;
    m_ByHandle.emplace(handle, Entry{ key, std::move(controller) });
    return handle;
}
const AnimatorController* AnimatorControllerStore::Get(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? nullptr : &it->second.controller;
}
std::string AnimatorControllerStore::KeyForHandle(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? std::string() : it->second.key;
}
std::vector<std::pair<uint64_t, std::string>> AnimatorControllerStore::GetAssetList() const {
    std::scoped_lock lk(m_Mutex);
    std::vector<std::pair<uint64_t, std::string>> out;
    out.reserve(m_ByHandle.size());
    for (const auto& [h, e] : m_ByHandle) out.emplace_back(h, e.key);
    return out;
}
