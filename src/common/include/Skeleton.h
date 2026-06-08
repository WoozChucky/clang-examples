#pragma once
#include <vector>
#include <string>
#include <glm/glm.hpp>

// Immutable skeletal-animation asset (CPU data). Bones are topologically ordered so a bone's parent
// index is always < its own index (parent computed before child in ComputeBindPoseGlobals).
// inverseBind (assimp mOffsetMatrix) is unused for SP1 bind-pose drawing — captured now for SP2
// skinning so the asset needs no re-extraction. See docs/superpowers/specs/2026-06-07-anim-skeleton-import-design.md.
struct Bone {
    std::string name;
    int         parent = -1;       // index into Skeleton::bones; -1 = root
    glm::mat4   localBind{1.0f};   // local bind transform (rest pose), relative to parent
    glm::mat4   inverseBind{1.0f}; // mesh-space -> bone-space at bind (SP2)
};

struct Skeleton {
    std::vector<Bone> bones;
    // Authored-content -> engine (Y-up) correction (the glTF scene-root / "Z_UP" node transform).
    // Prepended to skinning palettes (ComputeSkinningPalette). Identity for Y-up-authored models.
    glm::mat4 rootTransform{1.0f};
};

// Bind-pose global (model-space) transform per bone: global[b] = parent<0 ? localBind : global[parent]*localBind.
// Requires topological order (parent index < b), which the importer guarantees.
inline std::vector<glm::mat4> ComputeBindPoseGlobals(const Skeleton& sk) {
    std::vector<glm::mat4> g(sk.bones.size());
    for (size_t b = 0; b < sk.bones.size(); ++b) {
        const Bone& bone = sk.bones[b];
        g[b] = (bone.parent < 0) ? bone.localBind : g[bone.parent] * bone.localBind;
    }
    return g;
}
