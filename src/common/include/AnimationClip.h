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

// One bone's LOCAL transform as TRS (so blending can slerp the rotation). Blend in this space, then
// ComposeTRS + the hierarchy walk (PoseToGlobals).
struct BonePose {
    glm::vec3 T{0.0f};
    glm::quat R{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 S{1.0f};
};

inline glm::mat4 ComposeTRS(const BonePose& p) {
    return glm::translate(glm::mat4(1.0f), p.T) * glm::mat4_cast(p.R) * glm::scale(glm::mat4(1.0f), p.S);
}

// Affine decompose (bind/clip locals are T*R*S, no skew): T = col3; S = column lengths; R = quat of
// the scale-normalized rotation 3x3. Degenerate (zero-length) columns fall back to identity basis.
inline BonePose DecomposeTRS(const glm::mat4& m) {
    BonePose p;
    p.T = glm::vec3(m[3]);
    const glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    p.S = glm::vec3(glm::length(c0), glm::length(c1), glm::length(c2));
    const glm::vec3 r0 = p.S.x > 1e-8f ? c0 / p.S.x : glm::vec3(1,0,0);
    const glm::vec3 r1 = p.S.y > 1e-8f ? c1 / p.S.y : glm::vec3(0,1,0);
    const glm::vec3 r2 = p.S.z > 1e-8f ? c2 / p.S.z : glm::vec3(0,0,1);
    p.R = glm::normalize(glm::quat_cast(glm::mat3(r0, r1, r2)));
    return p;
}

// Per-bone LOCAL pose at `time`: each bone starts at rest (DecomposeTRS(localBind)); an animated bone
// overrides each track it has keys for (empty track keeps rest). R stays a quat (ready to slerp).
inline std::vector<BonePose> SampleClipPose(const Skeleton& sk, const AnimationClip& clip, float time) {
    std::vector<BonePose> pose(sk.bones.size());
    for (size_t b = 0; b < sk.bones.size(); ++b) pose[b] = DecomposeTRS(sk.bones[b].localBind);
    for (const auto& ch : clip.channels) {
        if (ch.boneIndex < 0 || static_cast<size_t>(ch.boneIndex) >= pose.size()) continue;
        BonePose& bp = pose[ch.boneIndex];
        if (!ch.posKeys.empty())   bp.T = anim_detail::SampleVec3(ch.posKeys,   time, bp.T);
        if (!ch.rotKeys.empty())   bp.R = anim_detail::SampleQuat(ch.rotKeys,   time);
        if (!ch.scaleKeys.empty()) bp.S = anim_detail::SampleVec3(ch.scaleKeys, time, bp.S);
    }
    return pose;
}

// Rest pose (every bone = DecomposeTRS(localBind)) — used when a state has no resolvable clip.
inline std::vector<BonePose> SampleClipPoseFromBind(const Skeleton& sk) {
    std::vector<BonePose> pose(sk.bones.size());
    for (size_t b = 0; b < sk.bones.size(); ++b) pose[b] = DecomposeTRS(sk.bones[b].localBind);
    return pose;
}

// Per-bone blend of two LOCAL poses: lerp T/S, slerp R (shortest-path). Clamped to the shorter size.
inline std::vector<BonePose> BlendPoses(const std::vector<BonePose>& a, const std::vector<BonePose>& b, float w) {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    std::vector<BonePose> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i].T = glm::mix(a[i].T, b[i].T, w);
        glm::quat qb = b[i].R;
        if (glm::dot(a[i].R, qb) < 0.0f) qb = -qb;
        out[i].R = glm::normalize(glm::slerp(a[i].R, qb, w));
        out[i].S = glm::mix(a[i].S, b[i].S, w);
    }
    return out;
}

// Compose each LOCAL TRS + hierarchy walk (topo order: parent index < b) -> model-space globals.
inline std::vector<glm::mat4> PoseToGlobals(const Skeleton& sk, const std::vector<BonePose>& localPoses) {
    std::vector<glm::mat4> global(sk.bones.size());
    for (size_t b = 0; b < sk.bones.size() && b < localPoses.size(); ++b) {
        const glm::mat4 local = ComposeTRS(localPoses[b]);
        const int parent = sk.bones[b].parent;
        global[b] = (parent < 0) ? local : global[parent] * local;
    }
    return global;
}

// Single-clip globals (SP3 convenience): sample the pose, then walk. Equivalent to the prior direct
// implementation for fully-keyed clips.
inline std::vector<glm::mat4> SampleAnimation(const Skeleton& sk, const AnimationClip& clip, float time) {
    return PoseToGlobals(sk, SampleClipPose(sk, clip, time));
}
