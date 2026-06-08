#include <cstdio>
#include <cmath>
#include <vector>
#include <utility>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "AnimationClip.h"

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)
static bool nearf(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static void T_sample_rotation() {
    // 2-bone skeleton, identity binds. Bone 1 channel rotates about +Z from 0deg (t=0) to 90deg (t=1).
    Skeleton sk;
    Bone root;  root.name="root";  root.parent=-1; // localBind identity
    Bone child; child.name="child"; child.parent=0; // localBind identity
    sk.bones = { root, child };

    AnimationClip clip; clip.name="spin"; clip.duration=1.0f;
    AnimChannel ch; ch.boneIndex = 1;
    ch.posKeys   = { {0.0f, glm::vec3(0)}, {1.0f, glm::vec3(0)} };
    ch.scaleKeys = { {0.0f, glm::vec3(1)}, {1.0f, glm::vec3(1)} };
    ch.rotKeys   = { {0.0f, glm::angleAxis(glm::radians(0.0f),  glm::vec3(0,0,1))},
                     {1.0f, glm::angleAxis(glm::radians(90.0f), glm::vec3(0,0,1))} };
    clip.channels = { ch };

    auto pt = [&](float t){ auto g = SampleAnimation(sk, clip, t); return glm::vec3(g[1] * glm::vec4(1,0,0,1)); };
    glm::vec3 p0 = pt(0.0f), ph = pt(0.5f), p1 = pt(1.0f);
    EXPECT(nearf(p0.x,1) && nearf(p0.y,0));                 // 0deg: (1,0,0)
    EXPECT(nearf(p1.x,0) && nearf(p1.y,1));                 // 90deg: (0,1,0)
    EXPECT(nearf(ph.x,0.70710678f) && nearf(ph.y,0.70710678f)); // 45deg
    glm::vec3 pAfter = pt(5.0f);
    EXPECT(nearf(pAfter.x,0) && nearf(pAfter.y,1));         // clamps to last key (90deg)
    auto g = SampleAnimation(sk, clip, 0.5f);
    glm::vec3 r = glm::vec3(g[0] * glm::vec4(1,0,0,1));
    EXPECT(nearf(r.x,1) && nearf(r.y,0) && nearf(r.z,0));   // unanimated bone 0 stays at rest
}

int main() {
    T_sample_rotation();
    if (g_Failures == 0) std::printf("All animation tests passed.\n");
    return g_Failures ? 1 : 0;
}
