#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>

#include "CameraFollow.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

static void T00_eye_above_and_distance()
{
    const glm::vec3 targetPos(0.0f);
    const CameraView v = ComputeFollowCamera(targetPos, kPoE2Follow, 16.0f / 9.0f);
    const glm::vec3 lookTarget = targetPos + glm::vec3(0.0f, kPoE2Follow.TargetHeight, 0.0f);
    EXPECT(v.Position.y > lookTarget.y);                                        // eye above target
    EXPECT(near(glm::length(v.Position - lookTarget), kPoE2Follow.Distance));   // exact distance
}

static void T01_lookat_centers_target()
{
    const glm::vec3 targetPos(3.0f, 0.0f, -2.0f);
    const CameraView v = ComputeFollowCamera(targetPos, kPoE2Follow, 1.6f);
    const glm::vec3 lookTarget = targetPos + glm::vec3(0.0f, kPoE2Follow.TargetHeight, 0.0f);
    const glm::vec4 vs = v.View * glm::vec4(lookTarget, 1.0f); // target in view space
    EXPECT(near(vs.x, 0.0f) && near(vs.y, 0.0f));             // centered
    EXPECT(near(vs.z, -kPoE2Follow.Distance));               // in front, at Distance (RH: -Z forward)
}

static void T02_yaw_orientation()
{
    FollowCameraParams p0 = kPoE2Follow; p0.YawDeg = 0.0f;
    const CameraView v0 = ComputeFollowCamera(glm::vec3(0.0f), p0, 1.6f);
    EXPECT(v0.Position.z > 0.0f);          // yaw 0 -> eye on +Z side
    EXPECT(near(v0.Position.x, 0.0f));

    FollowCameraParams p90 = kPoE2Follow; p90.YawDeg = 90.0f;
    const CameraView v90 = ComputeFollowCamera(glm::vec3(0.0f), p90, 1.6f);
    EXPECT(v90.Position.x > 0.0f);         // yaw 90 -> eye on +X side
    EXPECT(near(v90.Position.z, 0.0f));
}

static void T03_aspect_changes_projection()
{
    const CameraView a = ComputeFollowCamera(glm::vec3(0.0f), kPoE2Follow, 1.0f);
    const CameraView b = ComputeFollowCamera(glm::vec3(0.0f), kPoE2Follow, 2.0f);
    EXPECT(!near(a.Projection[0][0], b.Projection[0][0])); // horizontal scale tracks aspect
}

static void T04_extreme_elevation_is_finite()
{
    FollowCameraParams p = kPoE2Follow; p.ElevationDeg = 90.0f; // would degenerate without the clamp
    const CameraView v = ComputeFollowCamera(glm::vec3(0.0f), p, 1.6f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            EXPECT(std::isfinite(v.View[c][r]));
    EXPECT(std::isfinite(v.Position.x) && std::isfinite(v.Position.y) && std::isfinite(v.Position.z));
}

int main()
{
    T00_eye_above_and_distance();
    T01_lookat_centers_target();
    T02_yaw_orientation();
    T03_aspect_changes_projection();
    T04_extreme_elevation_is_finite();
    if (g_Failures == 0) { std::printf("All follow camera tests passed.\n"); return 0; }
    std::printf("%d follow camera test(s) FAILED.\n", g_Failures);
    return 1;
}
