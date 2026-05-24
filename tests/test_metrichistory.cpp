#include <cstdio>
#include <cmath>

#include "MetricHistory.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)
#define EXPECT_NEAR(a, b) EXPECT(std::fabs((a) - (b)) < 1e-4f)

static void T00_empty_is_safe()
{
    MetricHistory<4> h;
    EXPECT(h.Count() == 0);
    EXPECT(h.Capacity() == 4);
    EXPECT(h.Offset() == 0);
    EXPECT(h.Min() == 0.0f);
    EXPECT(h.Max() == 0.0f);
    EXPECT(h.Avg() == 0.0f);
    EXPECT(h.Last() == 0.0f);
}

static void T01_partial_fill()
{
    MetricHistory<4> h;
    h.Push(1.0f); h.Push(2.0f); h.Push(3.0f);
    EXPECT(h.Count() == 3);
    EXPECT(h.Offset() == 0);     // not wrapped yet
    EXPECT(h.Last() == 3.0f);
    EXPECT(h.Min() == 1.0f);
    EXPECT(h.Max() == 3.0f);
    EXPECT_NEAR(h.Avg(), 2.0f);
}

static void T02_exactly_full()
{
    MetricHistory<4> h;
    h.Push(1.0f); h.Push(2.0f); h.Push(3.0f); h.Push(4.0f);
    EXPECT(h.Count() == 4);
    EXPECT(h.Last() == 4.0f);
    EXPECT(h.Min() == 1.0f);
    EXPECT(h.Max() == 4.0f);
    EXPECT_NEAR(h.Avg(), 2.5f);
}

static void T03_wraps_drops_oldest()
{
    MetricHistory<4> h;
    for (float v : {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f}) h.Push(v); // retains 3,4,5,6
    EXPECT(h.Count() == 4);
    EXPECT(h.Last() == 6.0f);
    EXPECT(h.Min() == 3.0f);      // 1,2 dropped
    EXPECT(h.Max() == 6.0f);
    EXPECT_NEAR(h.Avg(), 4.5f);
    EXPECT(h.Offset() == 2);      // write cursor = oldest slot after wrap
}

int main()
{
    T00_empty_is_safe();
    T01_partial_fill();
    T02_exactly_full();
    T03_wraps_drops_oldest();

    if (g_Failures == 0) { std::printf("All metric history tests passed.\n"); return 0; }
    std::printf("%d metric history test(s) FAILED.\n", g_Failures);
    return 1;
}
