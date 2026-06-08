#include "animation/AnimationStore.h"
#include "AssetKey.h"

AnimationStore& AnimationStore::Instance() { static AnimationStore s; return s; }

uint64_t AnimationStore::Add(const std::string& key, AnimationClip clip) {
    const uint64_t handle = AssetKeyHash(key);
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    if (it != m_ByHandle.end()) return handle;
    m_ByHandle.emplace(handle, Entry{ key, std::move(clip) });
    return handle;
}
const AnimationClip* AnimationStore::Get(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? nullptr : &it->second.clip;
}
std::string AnimationStore::KeyForHandle(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? std::string() : it->second.key;
}
std::vector<std::pair<uint64_t, std::string>> AnimationStore::GetAssetList() const {
    std::scoped_lock lk(m_Mutex);
    std::vector<std::pair<uint64_t, std::string>> out;
    out.reserve(m_ByHandle.size());
    for (const auto& [h, e] : m_ByHandle) out.emplace_back(h, e.key);
    return out;
}
