#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "lib.h"
#include "ECS.h"

// Local platform_debug_break for the test exe: no MessageBox, just print + abort.
// SM_ASSERT delegates here when an assertion fails.
void platform_debug_break(const char* expr, const char* file, int line, const char* message)
{
    std::fprintf(stderr, "ASSERT FAIL %s:%d: %s (expr: %s)\n",
                 (file ? file : "<unknown>"),
                 line,
                 (message ? message : "<no message>"),
                 (expr ? expr : "<none>"));
    std::abort();
}

// Minimal harness — no Catch2 / doctest. Failures increment g_Failures.
static int g_Failures = 0;

#define EXPECT(cond)                                                    \
    do {                                                                \
        if (!(cond)) {                                                  \
            SM_ERROR("FAIL %s:%d: %s", __FILE__, __LINE__, #cond);      \
            ++g_Failures;                                               \
        }                                                               \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))
#define EXPECT_NE(a, b) EXPECT((a) != (b))

static void T00_smoke()
{
    EXPECT_EQ(1 + 1, 2);
}

static void T01_clone_produces_independent_copy()
{
    ComponentArray<TransformComponent> arr;
    arr.Add(1, TransformComponent{{1.0f, 2.0f, 3.0f}, {}, {1, 1, 1}});
    arr.Add(2, TransformComponent{{4.0f, 5.0f, 6.0f}, {}, {1, 1, 1}});

    std::shared_ptr<IComponentArray> clonedBase = arr.Clone();
    auto* cloned = static_cast<ComponentArray<TransformComponent>*>(clonedBase.get());

    EXPECT_EQ(cloned->Size(), arr.Size());
    EXPECT(cloned->Has(1));
    EXPECT(cloned->Has(2));
    EXPECT_EQ(cloned->Get(1)->Position.x, 1.0f);

    // Mutate the original; clone must not change.
    arr.Get(1)->Position.x = 999.0f;
    EXPECT_EQ(cloned->Get(1)->Position.x, 1.0f);
}

int main()
{
    T00_smoke();
    T01_clone_produces_independent_copy();

    if (g_Failures) {
        SM_ERROR("%d ECS test(s) failed", g_Failures);
        return 1;
    }
    SM_TRACE("All ECS tests passed.");
    return 0;
}
