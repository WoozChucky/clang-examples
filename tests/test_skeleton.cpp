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
    Bone root;  root.name = "root";  root.parent = -1; root.localBind = glm::translate(glm::mat4(1.0f), glm::vec3(1,0,0));
    Bone child; child.name = "child"; child.parent = 0;  child.localBind = glm::translate(glm::mat4(1.0f), glm::vec3(0,2,0));
    sk.bones = { root, child };

    const auto g = ComputeBindPoseGlobals(sk);
    EXPECT(g.size() == 2);
    const glm::vec3 rp = glm::vec3(g[0] * glm::vec4(0,0,0,1));
    const glm::vec3 cp = glm::vec3(g[1] * glm::vec4(0,0,0,1));
    EXPECT(nearf(rp.x,1) && nearf(rp.y,0) && nearf(rp.z,0));     // root at (1,0,0)
    EXPECT(nearf(cp.x,1) && nearf(cp.y,2) && nearf(cp.z,0));     // child = root*local = (1,2,0)
    EXPECT(sk.bones[1].parent < 1);                              // topological invariant: parent index < child

    if (g_Failures == 0) std::printf("All skeleton tests passed.\n");
    return g_Failures ? 1 : 0;
}
