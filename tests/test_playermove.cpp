#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>

#include "PlayerMovement.h" // ComputePlanarMove + InputStateComponent/KEY_* via ECS.h

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

static void T00_cardinals()
{
    InputStateComponent w{}; w.KeysDown[KEY_W] = true;
    const glm::vec3 d = ComputePlanarMove(w, 5.0f, 2.0f); // speed*dt = 10
    EXPECT(near(d.x, 0.0f) && near(d.y, 0.0f) && near(d.z, -10.0f));

    InputStateComponent a{}; a.KeysDown[KEY_A] = true;
    EXPECT(near(ComputePlanarMove(a, 5.0f, 2.0f).x, -10.0f));
    InputStateComponent s{}; s.KeysDown[KEY_S] = true;
    EXPECT(near(ComputePlanarMove(s, 5.0f, 2.0f).z, 10.0f));
    InputStateComponent d2{}; d2.KeysDown[KEY_D] = true;
    EXPECT(near(ComputePlanarMove(d2, 5.0f, 2.0f).x, 10.0f));
}

static void T01_diagonal_normalized()
{
    InputStateComponent in{}; in.KeysDown[KEY_W] = true; in.KeysDown[KEY_D] = true;
    const glm::vec3 d = ComputePlanarMove(in, 5.0f, 2.0f); // length must be speed*dt = 10, not 10*sqrt(2)
    EXPECT(near(glm::length(d), 10.0f));
    EXPECT(d.x > 0.0f && d.z < 0.0f);
    EXPECT(near(d.y, 0.0f));
}

static void T02_opposing_and_none_zero()
{
    InputStateComponent ws{}; ws.KeysDown[KEY_W] = true; ws.KeysDown[KEY_S] = true;
    EXPECT(near(glm::length(ComputePlanarMove(ws, 5.0f, 2.0f)), 0.0f));

    InputStateComponent none{};
    EXPECT(near(glm::length(ComputePlanarMove(none, 5.0f, 2.0f)), 0.0f));
}

static void T03_scaling()
{
    InputStateComponent in{}; in.KeysDown[KEY_W] = true;
    const float l1 = glm::length(ComputePlanarMove(in, 5.0f, 1.0f));
    const float l2 = glm::length(ComputePlanarMove(in, 5.0f, 2.0f));
    const float l3 = glm::length(ComputePlanarMove(in, 10.0f, 1.0f));
    EXPECT(near(l2, 2.0f * l1));
    EXPECT(near(l3, 2.0f * l1));
}

int main()
{
    T00_cardinals();
    T01_diagonal_normalized();
    T02_opposing_and_none_zero();
    T03_scaling();
    if (g_Failures == 0) { std::printf("All player movement tests passed.\n"); return 0; }
    std::printf("%d player movement test(s) FAILED.\n", g_Failures);
    return 1;
}
