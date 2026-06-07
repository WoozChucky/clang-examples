#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "Skeleton.h"

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)
static bool nearf(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
    Skeleton sk;
    // Root is a pure +90deg rotation about Z; child is a +2 translation on Y. The rotation makes the
    // multiply ORDER observable: correct global[child] = root*childLocal rotates the child's +Y offset
    // into -X. A reversed (childLocal*root) impl would leave the child at (0,2,0) — so this guards the
    // ordering invariant, not just the result.
    Bone root;  root.name = "root";  root.parent = -1; root.localBind = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0,0,1));
    Bone child; child.name = "child"; child.parent = 0;  child.localBind = glm::translate(glm::mat4(1.0f), glm::vec3(0,2,0));
    sk.bones = { root, child };

    const auto g = ComputeBindPoseGlobals(sk);
    EXPECT(g.size() == 2);
    const glm::vec3 rp = glm::vec3(g[0] * glm::vec4(0,0,0,1));
    const glm::vec3 cp = glm::vec3(g[1] * glm::vec4(0,0,0,1));
    EXPECT(nearf(rp.x,0) && nearf(rp.y,0) && nearf(rp.z,0));     // root rotation leaves the origin fixed
    EXPECT(nearf(cp.x,-2) && nearf(cp.y,0) && nearf(cp.z,0));    // root(rotZ90) * child(+Y2) => (-2,0,0); reversed order would give (0,2,0)
    EXPECT(sk.bones[1].parent < 1);                              // topological invariant: parent index < child

    if (g_Failures == 0) std::printf("All skeleton tests passed.\n");
    return g_Failures ? 1 : 0;
}
