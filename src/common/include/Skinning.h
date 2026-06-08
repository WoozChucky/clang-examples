#pragma once
#include <vector>
#include <utility>
#include <algorithm>
#include <cstdint>
#include <glm/glm.hpp>
#include "Skeleton.h"

// GPU-skinning data + pure math, shared by the importer (weight extraction), the GameThread skinning
// step (palette), the renderer (vertex format), and tests. See
// docs/superpowers/specs/2026-06-08-anim-skinning-design.md.

// Per-vertex bone influences for GPU skinning: up to 4 (boneIndex, weight) pairs.
struct SkinnedVertex {
    glm::uvec4 BoneIndices{0u, 0u, 0u, 0u};
    glm::vec4  BoneWeights{0.0f, 0.0f, 0.0f, 0.0f};
};

// Reduce an arbitrary influence set for ONE vertex to the top-4 by weight, normalized to sum 1.
// No influences -> bone 0 with weights (1,0,0,0) so the vertex skins to bone 0's transform (the VS
// needs no zero-weight special case).
inline SkinnedVertex MakeSkinnedVertex(std::vector<std::pair<uint32_t,float>> influences) {
    SkinnedVertex out;
    if (influences.empty()) { out.BoneWeights.x = 1.0f; return out; }
    std::sort(influences.begin(), influences.end(),
              [](const auto& a, const auto& b){ return a.second > b.second; });
    const size_t n = std::min<size_t>(4, influences.size());
    float sum = 0.0f;
    for (size_t i = 0; i < n; ++i) sum += influences[i].second;
    if (sum <= 0.0f) { out.BoneWeights.x = 1.0f; return out; }
    for (size_t i = 0; i < n; ++i) {
        out.BoneIndices[(glm::length_t)i] = influences[i].first;
        out.BoneWeights[(glm::length_t)i] = influences[i].second / sum;
    }
    return out;
}

// Skinning palette: palette[b] = globals[b] * inverseBind[b]. `globals` are model-space bone
// transforms (bind pose for SP2 via ComputeBindPoseGlobals; clip-sampled in SP3). For a true bind
// pose this is identity per bone (globals = inverse(inverseBind)).
inline std::vector<glm::mat4> ComputeSkinningPalette(const Skeleton& sk, const std::vector<glm::mat4>& globals) {
    const size_t n = sk.bones.size();
    std::vector<glm::mat4> palette(n);
    // sk.rootTransform prepends the authored->engine (Y-up) correction; identity for Y-up models.
    for (size_t b = 0; b < n && b < globals.size(); ++b)
        palette[b] = sk.rootTransform * globals[b] * sk.bones[b].inverseBind;
    return palette;
}
