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

static void T01_add_get_basic()
{
    ECS world;
    const auto e = world.CreateEntity();
    world.AddComponent(e, TransformComponent{{1.0f, 2.0f, 3.0f}, {}, {1, 1, 1}});

    EXPECT(world.HasComponent<TransformComponent>(e));
    const auto* t = world.GetComponent<TransformComponent>(e);
    EXPECT(t != nullptr);
    EXPECT_EQ(t->Position.x, 1.0f);
}

static void T07_modify_no_clone_on_invalid_entity()
{
    ECS world;
    const auto e = world.CreateEntity();
    world.AddComponent(e, TransformComponent{{1.0f, 0, 0}, {}, {1, 1, 1}});

    const auto* preArr = world.GetArray<TransformComponent>();
    EXPECT(preArr != nullptr);

    // Modify on entity that doesn't have the component — must NOT clone.
    const auto otherEntity = world.CreateEntity();   // no TransformComponent
    world.Modify<TransformComponent>(otherEntity, [](auto& t) { t.Position.x = 999.0f; });

    const auto* postArr = world.GetArray<TransformComponent>();
    EXPECT_EQ(static_cast<const void*>(preArr), static_cast<const void*>(postArr));
}

static void T_modify_writes_through_lambda()
{
    ECS world;
    const auto e = world.CreateEntity();
    world.AddComponent(e, TransformComponent{{1.0f, 2.0f, 3.0f}, {}, {1, 1, 1}});

    world.Modify<TransformComponent>(e, [](auto& t) {
        t.Position.x = 42.0f;
    });

    EXPECT_EQ(world.GetComponent<TransformComponent>(e)->Position.x, 42.0f);
}

static void T_mutate_array_bulk_write()
{
    ECS world;
    const auto e1 = world.CreateEntity();
    const auto e2 = world.CreateEntity();
    world.AddComponent(e1, TransformComponent{{1, 0, 0}, {}, {1, 1, 1}});
    world.AddComponent(e2, TransformComponent{{2, 0, 0}, {}, {1, 1, 1}});

    auto& arr = world.MutateArray<TransformComponent>();
    for (auto& t : arr.GetComponents()) t.Position.x += 10.0f;

    EXPECT_EQ(world.GetComponent<TransformComponent>(e1)->Position.x, 11.0f);
    EXPECT_EQ(world.GetComponent<TransformComponent>(e2)->Position.x, 12.0f);
}

static void T03_destroy_entity_clears_all_components()
{
    ECS world;
    const auto e = world.CreateEntity();
    world.AddComponent(e, TransformComponent{{1, 0, 0}, {}, {1, 1, 1}});
    world.AddComponent(e, MeshComponent{42, true});

    world.DestroyEntity(e);

    EXPECT(!world.HasComponent<TransformComponent>(e));
    EXPECT(!world.HasComponent<MeshComponent>(e));
    EXPECT(!world.IsValidEntity(e));
}

static void T04_destroy_entity_only_clones_owning_arrays()
{
    ECS world;
    const auto e = world.CreateEntity();
    world.AddComponent(e, TransformComponent{{1, 0, 0}, {}, {1, 1, 1}});
    // Register a second component TYPE without giving it to `e`.
    const auto other = world.CreateEntity();
    world.AddComponent(other, MeshComponent{7, true});

    // Snapshot so any subsequent mutation must clone.
    auto snap = world.CreateSnapshot();

    const auto* preMesh = world.GetArray<MeshComponent>();
    world.DestroyEntity(e);  // touches Transform only (e doesn't own a Mesh)
    const auto* postMesh = world.GetArray<MeshComponent>();

    // Mesh array must NOT have been cloned because `e` didn't have one.
    EXPECT_EQ(static_cast<const void*>(preMesh), static_cast<const void*>(postMesh));
    // Snapshot still sees `e` and its Transform.
    EXPECT(snap->HasComponent<TransformComponent>(e));
    // Master no longer sees `e`.
    EXPECT(!world.HasComponent<TransformComponent>(e));
}

int main()
{
    T00_smoke();
    T01_clone_produces_independent_copy();
    T08_mutate_array_clones_once_per_tick();
    T_get_array_const_returns_nullptr_for_unregistered();
    T_copy_arrays_from_shares_storage();
    T01_add_get_basic();
    T07_modify_no_clone_on_invalid_entity();
    T_modify_writes_through_lambda();
    T_mutate_array_bulk_write();
    T03_destroy_entity_clears_all_components();
    T04_destroy_entity_only_clones_owning_arrays();

    if (g_Failures) {
        SM_ERROR("%d ECS test(s) failed", g_Failures);
        return 1;
    }
    SM_TRACE("All ECS tests passed.");
    return 0;
}
