#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "EditorCamera.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static void T00_default_forward_is_minus_z()
{
    EditorCamera c;
    const glm::vec3 f = c.Forward();
    EXPECT(std::abs(f.x) < 1e-4f);
    EXPECT(std::abs(f.y) < 1e-4f);
    EXPECT(f.z < -0.99f);            // looks down -Z by default
    const glm::vec3 r = c.Right();
    EXPECT(r.x > 0.99f);             // right is +X
}

static void T01_look_yaw_rotates_forward()
{
    EditorCamera c;
    EditorCameraInput in{};
    in.Look = true; in.MouseDX = 100.0f; in.DeltaSeconds = 0.016f;
    c.Update(in);
    EXPECT(std::abs(c.GetYaw()) > 1e-3f);
    EXPECT(std::abs(c.Forward().x) > 1e-3f);
}

static void T02_pitch_clamps_at_89_deg()
{
    EditorCamera c;
    EditorCameraInput in{};
    in.Look = true; in.MouseDY = -100000.0f; in.DeltaSeconds = 0.016f; // slam look up
    c.Update(in);
    EXPECT(c.GetPitch() <= glm::radians(89.0f) + 1e-3f);
    EXPECT(c.GetPitch() >= glm::radians(89.0f) - 1e-1f); // hit the clamp
}

static void T03_wasd_moves_along_basis()
{
    EditorCamera c;
    const glm::vec3 p0 = c.GetPosition();
    EditorCameraInput in{};
    in.Look = true; in.W = true; in.DeltaSeconds = 1.0f; // forward 1 sec
    c.Update(in);
    const glm::vec3 d = c.GetPosition() - p0;
    EXPECT(d.z < -1.0f);
    EXPECT(std::abs(glm::length(d) - c.GetFlySpeed()) < 1e-3f);
}

static void T04_wasd_ignored_without_look()
{
    EditorCamera c;
    const glm::vec3 p0 = c.GetPosition();
    EditorCameraInput in{};
    in.W = true; in.DeltaSeconds = 1.0f; // no Look held
    c.Update(in);
    EXPECT(glm::length(c.GetPosition() - p0) < 1e-5f); // unchanged
}

static void T05_wheel_dollies_when_not_looking()
{
    EditorCamera c;
    const glm::vec3 p0 = c.GetPosition();
    EditorCameraInput in{};
    in.Wheel = 1.0f; // scroll, no RMB
    c.Update(in);
    const glm::vec3 d = c.GetPosition() - p0;
    EXPECT(d.z < 0.0f);              // dollied forward (-Z)
    EXPECT(glm::length(d) > 1e-3f);
}

static void T06_orbit_preserves_distance()
{
    EditorCamera c;
    const glm::vec3 pivot{0.0f, 0.0f, 0.0f};
    const float d0 = glm::length(c.GetPosition() - pivot);
    c.OrbitAround(pivot, glm::radians(30.0f), glm::radians(10.0f));
    const float d1 = glm::length(c.GetPosition() - pivot);
    EXPECT(std::abs(d0 - d1) < 1e-3f);     // distance to pivot preserved
}

static void T07_frame_centers_pivot()
{
    EditorCamera c;
    const glm::vec3 center{3.0f, 1.0f, -2.0f};
    c.FrameSelection(center, 2.0f);
    const CameraView cv = c.ToCameraView(1.0f);
    glm::vec4 clip = cv.Projection * cv.View * glm::vec4(center, 1.0f);
    EXPECT(clip.w > 0.0f);                          // in front
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    EXPECT(std::abs(ndc.x) < 1e-2f);
    EXPECT(std::abs(ndc.y) < 1e-2f);
    EXPECT(ndc.z > 0.0f && ndc.z < 1.0f);           // within depth [0,1]
}

static void T08_tocameraview_view_looks_at_target()
{
    EditorCamera c; // at (0,5,10) looking -Z
    const CameraView cv = c.ToCameraView(1.6f);
    glm::vec4 vp = cv.View * glm::vec4(0.0f, 5.0f, 0.0f, 1.0f); // 10 units ahead along -Z
    EXPECT(vp.z < 0.0f);
    EXPECT(std::abs(vp.x) < 1e-3f);
    EXPECT(std::abs(vp.y) < 1e-3f);
}

int main()
{
    T00_default_forward_is_minus_z();
    T01_look_yaw_rotates_forward();
    T02_pitch_clamps_at_89_deg();
    T03_wasd_moves_along_basis();
    T04_wasd_ignored_without_look();
    T05_wheel_dollies_when_not_looking();
    T06_orbit_preserves_distance();
    T07_frame_centers_pivot();
    T08_tocameraview_view_looks_at_target();

    if (g_Failures == 0) { std::printf("All editor camera tests passed.\n"); return 0; }
    std::printf("%d editor camera test(s) FAILED.\n", g_Failures);
    return 1;
}
