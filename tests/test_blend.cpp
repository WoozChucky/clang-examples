#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "AnimationClip.h"

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)
static bool nearf(float a, float b) { return std::fabs(a - b) < 1e-3f; }
static bool veq(const glm::vec3& a, const glm::vec3& b){ return nearf(a.x,b.x)&&nearf(a.y,b.y)&&nearf(a.z,b.z); }

static void T_decompose_roundtrip() {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(2,3,4))
                * glm::mat4_cast(glm::angleAxis(glm::radians(35.0f), glm::normalize(glm::vec3(0.2f,1,0.4f))))
                * glm::scale(glm::mat4(1.0f), glm::vec3(1.5f,2.0f,0.5f));
    BonePose p = DecomposeTRS(m);
    glm::mat4 r = ComposeTRS(p);
    glm::vec3 tp = glm::vec3(m * glm::vec4(1,1,1,1));
    glm::vec3 rp = glm::vec3(r * glm::vec4(1,1,1,1));
    EXPECT(veq(tp, rp));
}

static void T_blend_endpoints_and_mid() {
    std::vector<BonePose> a(1), b(1);
    a[0].T = glm::vec3(0); a[0].R = glm::angleAxis(glm::radians(0.0f),  glm::vec3(0,0,1)); a[0].S = glm::vec3(1);
    b[0].T = glm::vec3(0,10,0); b[0].R = glm::angleAxis(glm::radians(90.0f), glm::vec3(0,0,1)); b[0].S = glm::vec3(3);
    EXPECT(veq(BlendPoses(a,b,0.0f)[0].T, a[0].T) && veq(BlendPoses(a,b,0.0f)[0].S, a[0].S));
    EXPECT(veq(BlendPoses(a,b,1.0f)[0].T, b[0].T) && veq(BlendPoses(a,b,1.0f)[0].S, b[0].S));
    BonePose mid = BlendPoses(a,b,0.5f)[0];
    EXPECT(veq(mid.T, glm::vec3(0,5,0)) && veq(mid.S, glm::vec3(2)));
    glm::vec3 rotated = mid.R * glm::vec3(1,0,0);
    EXPECT(nearf(rotated.x, 0.70710678f) && nearf(rotated.y, 0.70710678f));
}

static void T_sample_equivalence() {
    Skeleton sk; Bone root; root.name="root"; root.parent=-1; Bone child; child.name="child"; child.parent=0; sk.bones={root,child};
    AnimationClip clip; clip.name="spin"; clip.duration=1.0f;
    AnimChannel ch; ch.boneIndex=1;
    ch.posKeys={{0.0f,glm::vec3(0)},{1.0f,glm::vec3(0)}};
    ch.scaleKeys={{0.0f,glm::vec3(1)},{1.0f,glm::vec3(1)}};
    ch.rotKeys={{0.0f,glm::angleAxis(glm::radians(0.0f),glm::vec3(0,0,1))},{1.0f,glm::angleAxis(glm::radians(90.0f),glm::vec3(0,0,1))}};
    clip.channels={ch};
    auto g = PoseToGlobals(sk, SampleClipPose(sk, clip, 1.0f));
    glm::vec3 p = glm::vec3(g[1] * glm::vec4(1,0,0,1));
    EXPECT(nearf(p.x,0) && nearf(p.y,1));
    auto g2 = SampleAnimation(sk, clip, 0.5f);
    glm::vec3 p2 = glm::vec3(g2[1] * glm::vec4(1,0,0,1));
    EXPECT(nearf(p2.x,0.70710678f) && nearf(p2.y,0.70710678f));
}

int main() {
    T_decompose_roundtrip();
    T_blend_endpoints_and_mid();
    T_sample_equivalence();
    if (g_Failures == 0) std::printf("All blend tests passed.\n");
    return g_Failures ? 1 : 0;
}
