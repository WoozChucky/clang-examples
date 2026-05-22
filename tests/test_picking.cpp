#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Picking.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static void T00_smoke() { EXPECT(1 + 1 == 2); }

static void T01_aabb_hit_ahead()
{
    Ray r; r.Origin = {0,0,0}; r.Dir = {0,0,-1};
    float t = -1.0f;
    EXPECT(RayIntersectsAABB(r, {-1,-1,-11}, {1,1,-9}, t) == true);
    EXPECT(std::abs(t - 9.0f) < 1e-3f);
}

static void T02_aabb_behind_miss()
{
    Ray r; r.Origin = {0,0,0}; r.Dir = {0,0,-1};
    float t = -1.0f;
    EXPECT(RayIntersectsAABB(r, {-1,-1,9}, {1,1,11}, t) == false);
}

static void T03_aabb_offaxis_miss()
{
    Ray r; r.Origin = {0,0,0}; r.Dir = {0,0,-1};
    float t = -1.0f;
    EXPECT(RayIntersectsAABB(r, {5,-1,-11}, {6,1,-9}, t) == false);
}

static void T04_aabb_inside_origin()
{
    Ray r; r.Origin = {0,0,0}; r.Dir = {0,0,-1};
    float t = -1.0f;
    EXPECT(RayIntersectsAABB(r, {-1,-1,-1}, {1,1,1}, t) == true);
    EXPECT(std::abs(t) < 1e-6f);
}

static void T05_nearest_ordering()
{
    Ray r; r.Origin = {0,0,0}; r.Dir = {0,0,-1};
    float tNear = 0.0f, tFar = 0.0f;
    EXPECT(RayIntersectsAABB(r, {-1,-1,-11}, {1,1,-9},  tNear) == true);
    EXPECT(RayIntersectsAABB(r, {-1,-1,-21}, {1,1,-19}, tFar)  == true);
    EXPECT(tNear < tFar);
}

static void T10_screen_center_ray()
{
    const glm::mat4 V = glm::lookAtRH(glm::vec3(0,0,0), glm::vec3(0,0,-1), glm::vec3(0,1,0));
    const glm::mat4 P = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    Ray r = ScreenPointToRay(500.0f, 500.0f, 0.0f, 0.0f, 1000.0f, 1000.0f, V, P);
    EXPECT(r.Dir.z < 0.0f);
    EXPECT(std::abs(r.Dir.x) < 1e-3f);
    EXPECT(std::abs(r.Dir.y) < 1e-3f);
    float t = -1.0f;
    EXPECT(RayIntersectsAABB(r, {-1,-1,-11}, {1,1,-9}, t) == true);
}

int main()
{
    T00_smoke();
    T01_aabb_hit_ahead();
    T02_aabb_behind_miss();
    T03_aabb_offaxis_miss();
    T04_aabb_inside_origin();
    T05_nearest_ordering();
    T10_screen_center_ray();

    if (g_Failures == 0) { std::printf("All picking tests passed.\n"); return 0; }
    std::printf("%d picking test(s) FAILED.\n", g_Failures);
    return 1;
}
