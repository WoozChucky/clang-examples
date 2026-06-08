#include <cstdio>
#include <cmath>
#include <vector>
#include <utility>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "Skinning.h"

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)
static bool nearf(float a, float b) { return std::fabs(a - b) < 1e-5f; }

static void T_make_skinned_vertex() {
    // >4 influences: keep top-4 by weight, renormalized to sum 1.
    std::vector<std::pair<uint32_t,float>> inf = {{0,0.05f},{1,0.40f},{2,0.30f},{3,0.20f},{4,0.05f}};
    const SkinnedVertex v = MakeSkinnedVertex(inf);
    float sum = v.BoneWeights.x + v.BoneWeights.y + v.BoneWeights.z + v.BoneWeights.w;
    EXPECT(nearf(sum, 1.0f));
    bool has1 = (v.BoneIndices.x==1u||v.BoneIndices.y==1u||v.BoneIndices.z==1u||v.BoneIndices.w==1u);
    EXPECT(has1);

    // Unweighted vertex -> bone 0, weights (1,0,0,0).
    const SkinnedVertex u = MakeSkinnedVertex({});
    EXPECT(u.BoneIndices.x==0u);
    EXPECT(nearf(u.BoneWeights.x,1.0f) && nearf(u.BoneWeights.y,0.0f) && nearf(u.BoneWeights.z,0.0f) && nearf(u.BoneWeights.w,0.0f));
}

static void T_bind_pose_palette_identity() {
    // Build a 2-bone skeleton; set inverseBind = inverse(globalBind) so palette = global*inverseBind = I.
    Skeleton sk;
    Bone root;  root.name="root";  root.parent=-1; root.localBind = glm::translate(glm::mat4(1.0f), glm::vec3(1,0,0));
    Bone child; child.name="child"; child.parent=0;  child.localBind = glm::rotate(glm::mat4(1.0f), glm::radians(30.0f), glm::vec3(0,0,1));
    sk.bones = { root, child };
    const auto globals = ComputeBindPoseGlobals(sk);
    sk.bones[0].inverseBind = glm::inverse(globals[0]);
    sk.bones[1].inverseBind = glm::inverse(globals[1]);

    const auto palette = ComputeSkinningPalette(sk, globals);
    EXPECT(palette.size() == 2);
    for (int b = 0; b < 2; ++b)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                EXPECT(nearf(palette[b][c][r], (c==r) ? 1.0f : 0.0f)); // ~identity
}

static bool veq(const glm::vec3& a, const glm::vec3& b) { return nearf(a.x,b.x) && nearf(a.y,b.y) && nearf(a.z,b.z); }

static void T_skin_vertex_cpu() {
    std::vector<glm::mat4> palette = { glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(10,0,0)) };
    glm::vec3 pos(1,2,3), nrm(0,1,0);
    glm::vec3 p0, n0; SkinVertexCPU(palette, glm::uvec4(0,0,0,0), glm::vec4(1,0,0,0), pos, nrm, p0, n0);
    EXPECT(veq(p0, pos) && veq(n0, nrm));                                    // full weight bone0 (identity) -> unchanged
    glm::vec3 p1, n1; SkinVertexCPU(palette, glm::uvec4(1,0,0,0), glm::vec4(1,0,0,0), pos, nrm, p1, n1);
    EXPECT(veq(p1, glm::vec3(11,2,3)) && veq(n1, nrm));                      // full weight bone1 (+10x) ; pure translate leaves normal
    glm::vec3 pm, nm; SkinVertexCPU(palette, glm::uvec4(0,1,0,0), glm::vec4(0.5f,0.5f,0,0), pos, nrm, pm, nm);
    EXPECT(veq(pm, glm::vec3(6,2,3)));                                       // 50/50 blend -> +5x
    glm::vec3 pe, ne; SkinVertexCPU({}, glm::uvec4(0,0,0,0), glm::vec4(1,0,0,0), pos, nrm, pe, ne);
    EXPECT(veq(pe, pos));                                                    // empty palette / oob index -> identity, no crash
}

int main() {
    T_make_skinned_vertex();
    T_bind_pose_palette_identity();
    T_skin_vertex_cpu();
    if (g_Failures == 0) std::printf("All skinning tests passed.\n");
    return g_Failures ? 1 : 0;
}
