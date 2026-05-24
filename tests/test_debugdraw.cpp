#include <cstdio>
#include <cmath>
#include <vector>
#include <glm/glm.hpp>

#include "DebugDraw.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool hasVert(const std::vector<DebugVertex>& v, glm::vec3 p) {
    for (auto& x : v) if (std::abs(x.Position.x-p.x)<1e-4f && std::abs(x.Position.y-p.y)<1e-4f
                          && std::abs(x.Position.z-p.z)<1e-4f) return true;
    return false;
}

static void T00_line()
{
    std::vector<DebugVertex> v;
    DebugAppendLine(v, {0,0,0}, {1,2,3}, {1,1,1,1});
    EXPECT(v.size() == 2);
    EXPECT(v[0].Position == glm::vec3(0,0,0));
    EXPECT(v[1].Position == glm::vec3(1,2,3));
    EXPECT(v[0].Color == glm::vec4(1,1,1,1));
}

static void T01_box_24_verts_all_corners()
{
    std::vector<DebugVertex> v;
    DebugAppendBox(v, {-1,-1,-1}, {1,1,1}, {0,1,0,1});
    EXPECT(v.size() == 24);
    EXPECT(hasVert(v, {-1,-1,-1}));
    EXPECT(hasVert(v, { 1, 1, 1}));
    EXPECT(hasVert(v, { 1,-1, 1}));
    EXPECT(v[0].Color == glm::vec4(0,1,0,1));
}

static void T02_sphere_segment_count()
{
    std::vector<DebugVertex> v;
    DebugAppendSphere(v, {0,0,0}, 2.0f, {1,0,0,1}, 8);
    EXPECT(v.size() == static_cast<size_t>(3 * 8 * 2));
}

static void T03_frustum_identity_is_ndc_cube()
{
    std::vector<DebugVertex> v;
    DebugAppendFrustum(v, glm::mat4(1.0f), {1,1,0,1});
    EXPECT(v.size() == 24);
    EXPECT(hasVert(v, {-1,-1,0}));
    EXPECT(hasVert(v, { 1, 1,1}));
}

int main()
{
    T00_line();
    T01_box_24_verts_all_corners();
    T02_sphere_segment_count();
    T03_frustum_identity_is_ndc_cube();

    if (g_Failures == 0) { std::printf("All debug draw tests passed.\n"); return 0; }
    std::printf("%d debug draw test(s) FAILED.\n", g_Failures);
    return 1;
}
