#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>

#include "ShadowMath.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static glm::vec3 proj_ndc(const glm::mat4& vp, const glm::vec3& p) {
    glm::vec4 c = vp * glm::vec4(p, 1.0f);
    return glm::vec3(c) / c.w;
}

static void T00_sun_up_gate()
{
    EXPECT(IsSunUp(glm::vec3(0, -1, 0)));
    EXPECT(IsSunUp(glm::normalize(glm::vec3(0, -0.5f, 0.5f))));
    EXPECT(!IsSunUp(glm::vec3(0, 1, 0)));
    EXPECT(!IsSunUp(glm::vec3(0, 0, 1)));
}

static void T01_center_projects_to_ndc_origin()
{
    const glm::vec3 center(3, 1, -2);
    const glm::mat4 vp = ComputeLightViewProj(center, 5.0f, glm::vec3(0, -1, 0));
    const glm::vec3 n = proj_ndc(vp, center);
    EXPECT(std::abs(n.x) < 1e-3f);
    EXPECT(std::abs(n.y) < 1e-3f);
    EXPECT(n.z > 0.0f && n.z < 1.0f);
}

static void T02_scene_fits_inside_light_frustum()
{
    const glm::vec3 center(0, 0, 0);
    const float r = 4.0f;
    const glm::mat4 vp = ComputeLightViewProj(center, r, glm::normalize(glm::vec3(0.3f, -1, 0.2f)));
    for (int i = 0; i < 8; ++i) {
        glm::vec3 corner(center.x + ((i&1)?r:-r), center.y + ((i&2)?r:-r), center.z + ((i&4)?r:-r));
        glm::vec3 n = proj_ndc(vp, corner);
        EXPECT(n.x >= -1.001f && n.x <= 1.001f);
        EXPECT(n.y >= -1.001f && n.y <= 1.001f);
        EXPECT(n.z >= -0.001f && n.z <= 1.001f);
    }
}

static void T03_straight_down_no_nan()
{
    const glm::mat4 vp = ComputeLightViewProj(glm::vec3(0), 2.0f, glm::vec3(0, -1, 0));
    const glm::vec3 n = proj_ndc(vp, glm::vec3(0));
    EXPECT(!std::isnan(n.x) && !std::isnan(n.y) && !std::isnan(n.z));
}

int main()
{
    T00_sun_up_gate();
    T01_center_projects_to_ndc_origin();
    T02_scene_fits_inside_light_frustum();
    T03_straight_down_no_nan();

    if (g_Failures == 0) { std::printf("All shadow math tests passed.\n"); return 0; }
    std::printf("%d shadow math test(s) FAILED.\n", g_Failures);
    return 1;
}
