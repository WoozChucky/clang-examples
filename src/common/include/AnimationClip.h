#pragma once
#include <vector>
#include <string>
#include <utility>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "Skeleton.h"

// Immutable animation-clip asset (CPU). Per-bone keyframe tracks; channels are sparse (only animated
// bones). Times are in seconds. See docs/superpowers/specs/2026-06-08-anim-clip-playback-design.md.
struct AnimChannel {
    int boneIndex = -1;                                   // index into Skeleton::bones
    std::vector<std::pair<float, glm::vec3>> posKeys;     // (time, value), ascending time
    std::vector<std::pair<float, glm::quat>> rotKeys;
    std::vector<std::pair<float, glm::vec3>> scaleKeys;
};
struct AnimationClip {
    std::string name;
    float duration = 0.0f;                               // seconds
    std::vector<AnimChannel> channels;                   // sparse
};

namespace anim_detail {
    inline glm::vec3 SampleVec3(const std::vector<std::pair<float,glm::vec3>>& k, float t, glm::vec3 fallback) {
        if (k.empty()) return fallback;
        if (t <= k.front().first) return k.front().second;
        if (t >= k.back().first)  return k.back().second;
        for (size_t i = 1; i < k.size(); ++i) {
            if (t < k[i].first) {
                const float u = (t - k[i-1].first) / (k[i].first - k[i-1].first);
                return glm::mix(k[i-1].second, k[i].second, u);
            }
        }
        return k.back().second;
    }
    inline glm::quat SampleQuat(const std::vector<std::pair<float,glm::quat>>& k, float t) {
        if (k.empty()) return glm::quat(1,0,0,0);
        if (t <= k.front().first) return k.front().second;
        if (t >= k.back().first)  return k.back().second;
        for (size_t i = 1; i < k.size(); ++i) {
            if (t < k[i].first) {
                const float u = (t - k[i-1].first) / (k[i].first - k[i-1].first);
                glm::quat a = k[i-1].second, b = k[i].second;
                if (glm::dot(a, b) < 0.0f) b = -b;        // shortest path
                return glm::normalize(glm::slerp(a, b, u));
            }
        }
        return k.back().second;
    }
}

// Per-bone model-space globals at `time`: animated bones use their channel's sampled T*R*S (empty
// track => neutral component: pos 0 / rot identity / scale 1 — assimp clips populate all three);
// unanimated bones use localBind (rest). Hierarchy walk requires topo order (parent index < b).
inline std::vector<glm::mat4> SampleAnimation(const Skeleton& sk, const AnimationClip& clip, float time) {
    std::vector<glm::mat4> local(sk.bones.size());
    for (size_t b = 0; b < sk.bones.size(); ++b) local[b] = sk.bones[b].localBind;
    for (const auto& ch : clip.channels) {
        if (ch.boneIndex < 0 || static_cast<size_t>(ch.boneIndex) >= local.size()) continue;
        const glm::vec3 p = anim_detail::SampleVec3(ch.posKeys,   time, glm::vec3(0.0f));
        const glm::quat q = anim_detail::SampleQuat(ch.rotKeys,   time);
        const glm::vec3 s = anim_detail::SampleVec3(ch.scaleKeys, time, glm::vec3(1.0f));
        local[ch.boneIndex] = glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(q) * glm::scale(glm::mat4(1.0f), s);
    }
    std::vector<glm::mat4> global(sk.bones.size());
    for (size_t b = 0; b < sk.bones.size(); ++b) {
        const int parent = sk.bones[b].parent;
        global[b] = (parent < 0) ? local[b] : global[parent] * local[b];
    }
    return global;
}
