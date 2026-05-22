#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Frustum.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

// Camera at origin looking down -Z, RH; 60-deg vertical FOV, square aspect, near 0.1 far 100.
static glm::mat4 TestVP()
{
    const glm::mat4 V = glm::lookAtRH(glm::vec3(0, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    const glm::mat4 P = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    return P * V;
}

// Small AABB centered at c with half-size h.
static void Box(glm::vec3 c, float h, glm::vec3& mn, glm::vec3& mx)
{
    mn = c - glm::vec3(h);
    mx = c + glm::vec3(h);
}

static void T00_smoke() { EXPECT(1 + 1 == 2); }

static void T01_point_ahead_visible()
{
    const Frustum f = ExtractFrustum(TestVP());
    glm::vec3 mn, mx; Box({0, 0, -10}, 0.1f, mn, mx);
    EXPECT(IsAABBVisible(f, mn, mx) == true);
}

static void T02_behind_camera_culled()
{
    const Frustum f = ExtractFrustum(TestVP());
    glm::vec3 mn, mx; Box({0, 0, 10}, 0.1f, mn, mx); // +Z is behind the camera
    EXPECT(IsAABBVisible(f, mn, mx) == false);
}

static void T03_beyond_far_culled()
{
    const Frustum f = ExtractFrustum(TestVP());
    glm::vec3 mn, mx; Box({0, 0, -200}, 0.1f, mn, mx); // far plane is 100
    EXPECT(IsAABBVisible(f, mn, mx) == false);
}

static void T04_left_right_planes()
{
    const Frustum f = ExtractFrustum(TestVP());
    glm::vec3 mn, mx;
    // At z=-10, half-width = 10*tan(30deg) ~= 5.77. x=5 inside, x=8 outside.
    Box({5, 0, -10}, 0.1f, mn, mx);  EXPECT(IsAABBVisible(f, mn, mx) == true);
    Box({8, 0, -10}, 0.1f, mn, mx);  EXPECT(IsAABBVisible(f, mn, mx) == false);
    Box({-8, 0, -10}, 0.1f, mn, mx); EXPECT(IsAABBVisible(f, mn, mx) == false);
}

static void T05_top_bottom_planes()
{
    const Frustum f = ExtractFrustum(TestVP());
    glm::vec3 mn, mx;
    Box({0, 8, -10}, 0.1f, mn, mx);  EXPECT(IsAABBVisible(f, mn, mx) == false);
    Box({0, -8, -10}, 0.1f, mn, mx); EXPECT(IsAABBVisible(f, mn, mx) == false);
}

static void T06_straddling_visible()
{
    const Frustum f = ExtractFrustum(TestVP());
    glm::vec3 mn, mx; Box({0, 0, -10}, 4.0f, mn, mx); // big box overlapping the view
    EXPECT(IsAABBVisible(f, mn, mx) == true);
}

static void T10_transform_identity()
{
    glm::vec3 mn, mx;
    TransformAABB(glm::mat4(1.0f), {-1, -2, -3}, {1, 2, 3}, mn, mx);
    EXPECT(std::abs(mn.x + 1) < 1e-5f && std::abs(mn.y + 2) < 1e-5f && std::abs(mn.z + 3) < 1e-5f);
    EXPECT(std::abs(mx.x - 1) < 1e-5f && std::abs(mx.y - 2) < 1e-5f && std::abs(mx.z - 3) < 1e-5f);
}

static void T11_transform_translate_into_view()
{
    const Frustum f = ExtractFrustum(TestVP());
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), {0, 0, -10});
    glm::vec3 mn, mx;
    TransformAABB(m, {-1, -1, -1}, {1, 1, 1}, mn, mx);
    EXPECT(IsAABBVisible(f, mn, mx) == true);
}

static void T12_transform_translate_behind()
{
    const Frustum f = ExtractFrustum(TestVP());
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), {0, 0, 50}); // behind camera
    glm::vec3 mn, mx;
    TransformAABB(m, {-1, -1, -1}, {1, 1, 1}, mn, mx);
    EXPECT(IsAABBVisible(f, mn, mx) == false);
}

static void T13_transform_rotate_scale_enlarges()
{
    // Unit cube, scale 2, rotate 45deg about Z. World extent per axis grows past the
    // local half-extent (1) -> abs-3x3 method produced an enlarged world AABB.
    glm::mat4 m = glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), {0, 0, 1});
    m = m * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    glm::vec3 mn, mx;
    TransformAABB(m, {-1, -1, -1}, {1, 1, 1}, mn, mx);
    EXPECT((mx.x - mn.x) > 4.0f); // 2*2*(cos45+sin45) ~= 5.66 > local-scaled 4
    EXPECT((mx.z - mn.z) > 3.9f); // z just scaled by 2 -> ~4
}

int main()
{
    T00_smoke();
    T01_point_ahead_visible();
    T02_behind_camera_culled();
    T03_beyond_far_culled();
    T04_left_right_planes();
    T05_top_bottom_planes();
    T06_straddling_visible();
    T10_transform_identity();
    T11_transform_translate_into_view();
    T12_transform_translate_behind();
    T13_transform_rotate_scale_enlarges();

    if (g_Failures == 0) { std::printf("All frustum tests passed.\n"); return 0; }
    std::printf("%d frustum test(s) FAILED.\n", g_Failures);
    return 1;
}
