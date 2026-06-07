#include "animation/SkeletonStore.h"
#include "AssetKey.h"

SkeletonStore& SkeletonStore::Instance() {
    static SkeletonStore s;
    return s;
}

uint64_t SkeletonStore::Add(const std::string& key, Skeleton skeleton) {
    const uint64_t handle = AssetKeyHash(key);
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    if (it != m_ByHandle.end()) return handle; // de-dup
    m_ByHandle.emplace(handle, Entry{ key, std::move(skeleton) });
    return handle;
}

const Skeleton* SkeletonStore::Get(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? nullptr : &it->second.skeleton;
}

std::string SkeletonStore::KeyForHandle(uint64_t handle) const {
    std::scoped_lock lk(m_Mutex);
    auto it = m_ByHandle.find(handle);
    return it == m_ByHandle.end() ? std::string() : it->second.key;
}
