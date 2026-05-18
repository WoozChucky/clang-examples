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

static void T08_mutate_array_clones_once_per_tick()
{
    ComponentStore store;
    auto* preStorage = static_cast<const void*>(store.GetArray<TransformComponent>());
    // Cold lookup: returns nullptr because nothing registered yet.
    EXPECT_EQ(preStorage, nullptr);

    auto& a1 = store.MutateArray<TransformComponent>();
    a1.Add(1, TransformComponent{{1, 2, 3}, {}, {1, 1, 1}});
    auto* addrAfterFirst = static_cast<const void*>(&a1);

    auto& a2 = store.MutateArray<TransformComponent>();
    // Same tick: returns the same already-cloned array. No additional clone.
    EXPECT_EQ(static_cast<const void*>(&a2), addrAfterFirst);

    // Reset dirty set (simulates CreateSnapshot publishing).
    store.ClearDirty();

    auto& a3 = store.MutateArray<TransformComponent>();
    // After ClearDirty, next mutation triggers a fresh clone — address may differ.
    // What we definitely require: a3 still contains entity 1 with the same value.
    EXPECT(a3.Has(1));
    EXPECT_EQ(a3.Get(1)->Position.x, 1.0f);
}

static void T_get_array_const_returns_nullptr_for_unregistered()
{
    ComponentStore store;
    const auto* arr = store.GetArray<MeshComponent>();
    EXPECT_EQ(arr, nullptr);
}

static void T_copy_arrays_from_shares_storage()
{
    ComponentStore master;
    master.MutateArray<TransformComponent>().Add(1, TransformComponent{{7,0,0}, {}, {1,1,1}});

    ComponentStore snap;
    snap.CopyArraysFrom(master);

    // Reads through snap see the same data.
    const auto* arr = snap.GetArray<TransformComponent>();
    EXPECT(arr != nullptr);
    EXPECT(arr->Has(1));
    EXPECT_EQ(arr->Get(1)->Position.x, 7.0f);
}

int main()
{
    T00_smoke();
    T01_clone_produces_independent_copy();
    T08_mutate_array_clones_once_per_tick();
    T_get_array_const_returns_nullptr_for_unregistered();
    T_copy_arrays_from_shares_storage();

    if (g_Failures) {
        SM_ERROR("%d ECS test(s) failed", g_Failures);
        return 1;
    }
    SM_TRACE("All ECS tests passed.");
    return 0;
}
