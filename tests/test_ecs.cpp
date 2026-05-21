#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "lib.h"
#include "ECS.h"
#include "Systems.h"

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

static void T05_snapshot_isolates_reads_from_writes()
{
    ECS world;
    const auto e = world.CreateEntity();
    world.AddComponent(e, TransformComponent{{1.0f, 2.0f, 3.0f}, {}, {1, 1, 1}});

    auto snap = world.CreateSnapshot();
    EXPECT_EQ(snap->GetComponent<TransformComponent>(e)->Position.x, 1.0f);

    world.Modify<TransformComponent>(e, [](auto& t) { t.Position.x = 999.0f; });

    // Snapshot still sees the original value.
    EXPECT_EQ(snap->GetComponent<TransformComponent>(e)->Position.x, 1.0f);
    // Master sees the new value.
    EXPECT_EQ(world.GetComponent<TransformComponent>(e)->Position.x, 999.0f);
}

static void T06_snapshot_unchanged_arrays_share_storage()
{
    ECS world;
    const auto e = world.CreateEntity();
    world.AddComponent(e, TransformComponent{{1, 0, 0}, {}, {1, 1, 1}});
    world.AddComponent(e, MeshComponent{1, true});

    auto snap = world.CreateSnapshot();

    const auto* snapMesh   = snap->GetArray<MeshComponent>();
    const auto* worldMesh  = world.GetArray<MeshComponent>();
    EXPECT_EQ(static_cast<const void*>(snapMesh), static_cast<const void*>(worldMesh));

    // Touch Transform only.
    world.Modify<TransformComponent>(e, [](auto& t) { t.Position.x = 7.0f; });

    // Mesh array still shared (no clone).
    const auto* worldMeshAfter = world.GetArray<MeshComponent>();
    EXPECT_EQ(static_cast<const void*>(snapMesh), static_cast<const void*>(worldMeshAfter));

    // Transform array now DIFFERENT (it was cloned).
    const auto* snapTransform  = snap->GetArray<TransformComponent>();
    const auto* worldTransform = world.GetArray<TransformComponent>();
    EXPECT_NE(static_cast<const void*>(snapTransform), static_cast<const void*>(worldTransform));
}

static void T09_create_snapshot_clears_dirty()
{
    ECS world;
    const auto e = world.CreateEntity();
    world.AddComponent(e, TransformComponent{{1, 0, 0}, {}, {1, 1, 1}});

    auto snap1 = world.CreateSnapshot();
    const auto* snap1Arr = snap1->GetArray<TransformComponent>();

    // After snapshot, dirty cleared — next mutation must clone again.
    world.Modify<TransformComponent>(e, [](auto& t) { t.Position.x = 2.0f; });

    const auto* worldArrAfter = world.GetArray<TransformComponent>();
    EXPECT_NE(static_cast<const void*>(snap1Arr), static_cast<const void*>(worldArrAfter));
    EXPECT_EQ(snap1->GetComponent<TransformComponent>(e)->Position.x, 1.0f);
    EXPECT_EQ(world.GetComponent<TransformComponent>(e)->Position.x, 2.0f);
}

static void T10_multi_snapshot_lifetime()
{
    ECS world;
    const auto e = world.CreateEntity();
    world.AddComponent(e, TransformComponent{{1, 0, 0}, {}, {1, 1, 1}});

    auto snap1 = world.CreateSnapshot();
    world.Modify<TransformComponent>(e, [](auto& t) { t.Position.x = 2.0f; });

    auto snap2 = world.CreateSnapshot();
    world.Modify<TransformComponent>(e, [](auto& t) { t.Position.x = 3.0f; });

    EXPECT_EQ(snap1->GetComponent<TransformComponent>(e)->Position.x, 1.0f);
    EXPECT_EQ(snap2->GetComponent<TransformComponent>(e)->Position.x, 2.0f);
    EXPECT_EQ(world.GetComponent<TransformComponent>(e)->Position.x, 3.0f);

    snap1.reset();  // drop the oldest

    // snap2 still readable, unchanged.
    EXPECT_EQ(snap2->GetComponent<TransformComponent>(e)->Position.x, 2.0f);
}

static void T_addcomponent_after_snapshot_clones()
{
    ECS world;
    const auto e = world.CreateEntity();
    world.AddComponent(e, TransformComponent{{1, 0, 0}, {}, {1, 1, 1}});

    auto snap = world.CreateSnapshot();
    const auto* preArr = world.GetArray<TransformComponent>();

    // AddComponent on a new entity: must NOT touch the snapshot's view.
    const auto e2 = world.CreateEntity();
    world.AddComponent(e2, TransformComponent{{9, 0, 0}, {}, {1, 1, 1}});

    EXPECT(!snap->HasComponent<TransformComponent>(e2));
    EXPECT(world.HasComponent<TransformComponent>(e2));

    const auto* postArr = world.GetArray<TransformComponent>();
    EXPECT_NE(static_cast<const void*>(preArr), static_cast<const void*>(postArr));
}

static void T_removecomponent_after_snapshot_clones()
{
    ECS world;
    const auto e = world.CreateEntity();
    world.AddComponent(e, TransformComponent{{1, 0, 0}, {}, {1, 1, 1}});

    auto snap = world.CreateSnapshot();
    world.RemoveComponent<TransformComponent>(e);

    EXPECT(snap->HasComponent<TransformComponent>(e));
    EXPECT(!world.HasComponent<TransformComponent>(e));
}

static void T11_destroyed_entity_id_reuse()
{
    ECS world;
    const auto e1 = world.CreateEntity();
    world.AddComponent(e1, TransformComponent{{1, 0, 0}, {}, {1, 1, 1}});

    world.DestroyEntity(e1);

    const auto e2 = world.CreateEntity();
    EXPECT(!world.HasComponent<TransformComponent>(e2));

    auto snap = world.CreateSnapshot();
    EXPECT(!snap->HasComponent<TransformComponent>(e2));
}

static void T12_concurrent_smoke()
{
    ECS world;
    std::vector<EntityId> ids;
    ids.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        const auto e = world.CreateEntity();
        world.AddComponent(e, TransformComponent{{(float)i, 0, 0}, {}, {1, 1, 1}});
        ids.push_back(e);
    }

    auto snap = world.CreateSnapshot();

    std::atomic<bool> stop{false};
    std::thread reader([&]{
        while (!stop.load(std::memory_order_relaxed)) {
            for (const auto e : ids) {
                const auto* t = snap->GetComponent<TransformComponent>(e);
                if (!t) { ++g_Failures; break; }
            }
        }
    });

    for (int iter = 0; iter < 1000; ++iter) {
        for (const auto e : ids) {
            world.Modify<TransformComponent>(e, [&](auto& t) { t.Position.x += 0.1f; });
        }
    }

    stop.store(true, std::memory_order_relaxed);
    reader.join();

    // Snapshot positions must be the initial values, untouched by master writes.
    for (size_t i = 0; i < ids.size(); ++i) {
        const auto* t = snap->GetComponent<TransformComponent>(ids[i]);
        EXPECT(t != nullptr);
        if (t) EXPECT_EQ(t->Position.x, (float)i);
    }
}

// --- Singleton component tests ---

static void TSG01_set_get_roundtrip()
{
    ECS world;
    EXPECT_EQ(world.GetSingleton<DayNightConfigComponent>(), nullptr);   // unset → null
    world.SetSingleton(DayNightConfigComponent{ 42.0f });
    const auto* c = world.GetSingleton<DayNightConfigComponent>();
    EXPECT_NE(c, nullptr);
    if (c) EXPECT_EQ(c->CycleSeconds, 42.0f);
}

static void TSG02_modify_singleton()
{
    ECS world;
    world.SetSingleton(AppControlComponent{ false });
    world.ModifySingleton<AppControlComponent>([](AppControlComponent& a){ a.QuitRequested = true; });
    const auto* a = world.GetSingleton<AppControlComponent>();
    EXPECT_NE(a, nullptr);
    if (a) EXPECT(a->QuitRequested);
}

static void TSG03_singleton_entity_hidden()
{
    ECS world;
    world.SetSingleton(ViewportComponent{ 800, 600 });
    EXPECT_EQ(world.GetEntityCount(), 0u);                 // reserved entity not counted
    EXPECT_EQ(world.GetActiveEntities().size(), 0u);       // not iterated by gameplay
    const EntityId e = world.CreateEntity();               // first gameplay id
    EXPECT_NE(e, world.SingletonEntity());                 // distinct from reserved
    EXPECT_EQ(world.GetEntityCount(), 1u);                 // only the gameplay entity
}

static void TSG04_snapshot_preserves_singletons()
{
    ECS world;
    world.SetSingleton(DayNightConfigComponent{ 7.0f });
    std::shared_ptr<const ECS> snap = world.CreateSnapshot();
    const auto* c = snap->GetSingleton<DayNightConfigComponent>();
    EXPECT_NE(c, nullptr);
    if (c) EXPECT_EQ(c->CycleSeconds, 7.0f);
}

static void TSG05_clear_preserves_singletons_removes_gameplay()
{
    ECS world;
    world.SetSingleton(DayNightConfigComponent{ 3.0f });
    const EntityId e = world.CreateEntity();
    world.AddComponent(e, TransformComponent{{1,2,3},{},{1,1,1}});
    EXPECT_EQ(world.GetEntityCount(), 1u);

    world.Clear();

    EXPECT_EQ(world.GetEntityCount(), 0u);                       // gameplay gone
    const auto* c = world.GetSingleton<DayNightConfigComponent>();
    EXPECT_NE(c, nullptr);                                       // singleton survives
    if (c) EXPECT_EQ(c->CycleSeconds, 3.0f);
}

// --- Systems layer tests ---

// Records its name into a shared sink when run; carries a configurable phase.
struct RecordingSystem final : ISystem {
    RecordingSystem(const char* name, SystemPhase phase, std::vector<std::string>* sink)
        : m_Name(name), m_Phase(phase), m_Sink(sink) {}
    void Update(SystemContext&) override { m_Sink->push_back(m_Name); }
    const char* Name() const override { return m_Name; }
    SystemPhase Phase() const override { return m_Phase; }
    const char* m_Name;
    SystemPhase m_Phase;
    std::vector<std::string>* m_Sink;
};

// Increments a counter on destruction (proves Clear() runs dtors).
struct DtorCounterSystem final : ISystem {
    explicit DtorCounterSystem(int* counter) : m_Counter(counter) {}
    ~DtorCounterSystem() override { ++(*m_Counter); }
    void Update(SystemContext&) override {}
    const char* Name() const override { return "DtorCounter"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
    int* m_Counter;
};

// Adds a TransformComponent to entity 1 and bumps its X each Update.
struct MutateSystem final : ISystem {
    void Update(SystemContext& ctx) override {
        if (!ctx.world.HasComponent<TransformComponent>(1)) {
            ctx.world.AddComponent(1, TransformComponent{{0, 0, 0}, {}, {1, 1, 1}});
        }
        ctx.world.Modify<TransformComponent>(1, [](TransformComponent& t) {
            t.Position.x += 1.0f;
        });
    }
    const char* Name() const override { return "Mutate"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

static void TS01_register_and_count()
{
    SystemScheduler sched;
    EXPECT_EQ(sched.Count(), 0u);
    std::vector<std::string> sink;
    sched.Register(std::make_unique<RecordingSystem>("a", SystemPhase::Simulation, &sink));
    sched.Register(std::make_unique<RecordingSystem>("b", SystemPhase::Simulation, &sink));
    EXPECT_EQ(sched.Count(), 2u);
}

static void TS02_runs_in_phase_then_registration_order()
{
    SystemScheduler sched;
    std::vector<std::string> sink;
    sched.Register(std::make_unique<RecordingSystem>("sim1",   SystemPhase::Simulation,     &sink));
    sched.Register(std::make_unique<RecordingSystem>("input1", SystemPhase::Input,          &sink));
    sched.Register(std::make_unique<RecordingSystem>("sim2",   SystemPhase::Simulation,     &sink));
    sched.Register(std::make_unique<RecordingSystem>("pre1",   SystemPhase::PreRender,      &sink));
    sched.Register(std::make_unique<RecordingSystem>("post1",  SystemPhase::PostSimulation, &sink));

    ECS world;
    SystemContext ctx{ world, 0.016, 1.0 };
    sched.Run(ctx);

    EXPECT_EQ(sink.size(), 5u);
    EXPECT(sink[0] == "input1");
    EXPECT(sink[1] == "sim1");
    EXPECT(sink[2] == "sim2");
    EXPECT(sink[3] == "post1");
    EXPECT(sink[4] == "pre1");
}

static void TS03_clear_destroys_systems()
{
    int dtorCount = 0;
    {
        SystemScheduler sched;
        sched.Register(std::make_unique<DtorCounterSystem>(&dtorCount));
        sched.Register(std::make_unique<DtorCounterSystem>(&dtorCount));
        EXPECT_EQ(sched.Count(), 2u);
        sched.Clear();
        EXPECT_EQ(sched.Count(), 0u);
        EXPECT_EQ(dtorCount, 2);
    }
    EXPECT_EQ(dtorCount, 2);
}

static void TS04_empty_run_is_noop()
{
    SystemScheduler sched;
    ECS world;
    SystemContext ctx{ world, 0.016, 1.0 };
    sched.Run(ctx);
    EXPECT_EQ(sched.Count(), 0u);
}

static void TS05_system_mutates_ecs()
{
    SystemScheduler sched;
    sched.Register(std::make_unique<MutateSystem>());
    ECS world;
    world.CreateEntity();         // id 1
    SystemContext ctx{ world, 0.016, 1.0 };
    sched.Run(ctx);
    sched.Run(ctx);
    const auto* t = world.GetComponent<TransformComponent>(1);
    EXPECT_NE(t, nullptr);
    if (t) EXPECT_EQ(t->Position.x, 2.0f);
}

static void TE01_each_visits_matching_entities()
{
    ECS w;
    EntityId a = w.CreateEntity(); w.AddComponent(a, TransformComponent{});
    EntityId b = w.CreateEntity(); w.AddComponent(b, TransformComponent{}); w.AddComponent(b, MeshComponent{});
    EntityId c = w.CreateEntity(); w.AddComponent(c, MeshComponent{});
    (void)a; (void)c;

    int count = 0; EntityId seen = INVALID_ENTITY;
    w.Each<TransformComponent, MeshComponent>([&](EntityId e){ ++count; seen = e; });
    EXPECT_EQ(count, 1);
    EXPECT_EQ(seen, b);
}

static void TE02_each_components_form()
{
    ECS w;
    EntityId a = w.CreateEntity();
    w.AddComponent(a, TransformComponent{{1.0f, 2.0f, 3.0f}, {}, {1, 1, 1}});
    w.AddComponent(a, MeshComponent{ .MeshId = 42, .Visible = true });

    int count = 0; uint32_t meshId = 0; float px = 0.0f;
    w.Each<TransformComponent, MeshComponent>(
        [&](EntityId, const TransformComponent& t, const MeshComponent& m){
            ++count; meshId = m.MeshId; px = t.Position.x;
        });
    EXPECT_EQ(count, 1);
    EXPECT_EQ(meshId, 42u);
    EXPECT_EQ(px, 1.0f);
}

static void TE03_each_zero_matches()
{
    ECS w;
    EntityId a = w.CreateEntity(); w.AddComponent(a, TransformComponent{});
    (void)a;
    int count = 0;
    w.Each<MeshComponent>([&](EntityId){ ++count; });
    EXPECT_EQ(count, 0);
}

static void TE04_each_single_component()
{
    ECS w;
    w.AddComponent(w.CreateEntity(), TransformComponent{});
    w.AddComponent(w.CreateEntity(), TransformComponent{});
    w.CreateEntity(); // no components
    int count = 0;
    w.Each<TransformComponent>([&](EntityId){ ++count; });
    EXPECT_EQ(count, 2);
}

static void TE05_each_does_not_change_entity_count()
{
    ECS w;
    w.AddComponent(w.CreateEntity(), TransformComponent{});
    size_t before = w.GetEntityCount();
    w.Each<TransformComponent>([&](EntityId){});
    EXPECT_EQ(w.GetEntityCount(), before);
}

static void TE06_each_components_form_multi_entity_aliases_storage()
{
    ECS w;
    EntityId a = w.CreateEntity(); w.AddComponent(a, TransformComponent{}); w.AddComponent(a, MeshComponent{ .MeshId = 10, .Visible = true });
    EntityId b = w.CreateEntity(); w.AddComponent(b, TransformComponent{}); // Transform only -> must be skipped
    EntityId c = w.CreateEntity(); w.AddComponent(c, TransformComponent{}); w.AddComponent(c, MeshComponent{ .MeshId = 20, .Visible = true });
    (void)b;

    int count = 0; uint32_t meshIdSum = 0; bool aliases = true;
    w.Each<TransformComponent, MeshComponent>(
        [&](EntityId e, const TransformComponent&, const MeshComponent& m){
            ++count;
            meshIdSum += m.MeshId;
            // the ref must point at the live component storage, not a copy
            if (&m != w.GetComponent<MeshComponent>(e)) aliases = false;
        });
    EXPECT_EQ(count, 2);              // a and c, not b
    EXPECT_EQ(meshIdSum, (uint32_t)30);
    EXPECT(aliases);
}

static void TS01_componentarray_cross_page_ids()
{
    // ids 5 and 5000 will land in different pages once paged; behavior is
    // page-agnostic on the current impl too.
    ComponentArray<TransformComponent> arr;
    arr.Add(5,    TransformComponent{{5.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Add(5000, TransformComponent{{50.0f, 0, 0}, {}, {1, 1, 1}});

    EXPECT(arr.Has(5));
    EXPECT(arr.Has(5000));
    EXPECT(!arr.Has(6));
    EXPECT(!arr.Has(0));
    EXPECT_EQ(arr.Get(5)->Position.x, 5.0f);
    EXPECT_EQ(arr.Get(5000)->Position.x, 50.0f);
    EXPECT_EQ(arr.Size(), (size_t)2);
}

static void TS02_componentarray_high_id_only()
{
    ComponentArray<TransformComponent> arr;
    arr.Add(5000, TransformComponent{});
    EXPECT(arr.Has(5000));
    EXPECT(!arr.Has(0));
    EXPECT(!arr.Has(5));
    EXPECT(!arr.Has(4999));
    EXPECT(arr.Get(5) == nullptr);
    EXPECT(arr.Get(5000) != nullptr);
    EXPECT_EQ(arr.Size(), (size_t)1);
}

static void TS03_componentarray_swap_and_pop_preserves_others()
{
    ComponentArray<TransformComponent> arr;
    arr.Add(10, TransformComponent{{10.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Add(20, TransformComponent{{20.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Add(30, TransformComponent{{30.0f, 0, 0}, {}, {1, 1, 1}});

    arr.Remove(20); // middle removal -> last (30) swaps into 20's slot

    EXPECT(!arr.Has(20));
    EXPECT(arr.Has(10));
    EXPECT(arr.Has(30));
    EXPECT_EQ(arr.Get(10)->Position.x, 10.0f);
    EXPECT_EQ(arr.Get(30)->Position.x, 30.0f);
    EXPECT_EQ(arr.Size(), (size_t)2);
}

static void TS04_componentarray_remove_then_readd_same_id()
{
    ComponentArray<TransformComponent> arr;
    arr.Add(7, TransformComponent{{7.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Remove(7);
    EXPECT(!arr.Has(7));
    EXPECT(arr.Get(7) == nullptr);

    arr.Add(7, TransformComponent{{77.0f, 0, 0}, {}, {1, 1, 1}});
    EXPECT(arr.Has(7));
    EXPECT_EQ(arr.Get(7)->Position.x, 77.0f);
    EXPECT_EQ(arr.Size(), (size_t)1);
}

static void TS05_componentarray_update_existing()
{
    ComponentArray<TransformComponent> arr;
    arr.Add(3, TransformComponent{{1.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Add(3, TransformComponent{{2.0f, 0, 0}, {}, {1, 1, 1}}); // same id -> update
    EXPECT_EQ(arr.Size(), (size_t)1);
    EXPECT_EQ(arr.Get(3)->Position.x, 2.0f);
}

static void TS06_componentarray_clone_independent_after_swap_and_pop()
{
    ComponentArray<TransformComponent> arr;
    arr.Add(1, TransformComponent{{1.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Add(2, TransformComponent{{2.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Add(3, TransformComponent{{3.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Remove(2); // exercise swap-and-pop before cloning

    std::shared_ptr<IComponentArray> clonedBase = arr.Clone();
    auto* cloned = static_cast<ComponentArray<TransformComponent>*>(clonedBase.get());

    EXPECT_EQ(cloned->Size(), arr.Size());
    EXPECT(cloned->Has(1));
    EXPECT(cloned->Has(3));
    EXPECT(!cloned->Has(2));
    EXPECT_EQ(cloned->Get(3)->Position.x, 3.0f);

    arr.Get(1)->Position.x = 999.0f;
    arr.Remove(3);
    EXPECT_EQ(cloned->Get(1)->Position.x, 1.0f);
    EXPECT(cloned->Has(3));
    EXPECT_EQ(cloned->Get(3)->Position.x, 3.0f);
}

static void TS07_clone_and_assign_independent_across_pages()
{
    // ids 5 (page 0) and 5000 (page ~4) -> outer page vector has nullptr holes.
    // Exercises the copy ctor's multi-page deep-copy + nullptr-hole preservation,
    // plus copy-assignment independence (Rule-of-5 coverage).
    ComponentArray<TransformComponent> arr;
    arr.Add(5,    TransformComponent{{5.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Add(5000, TransformComponent{{50.0f, 0, 0}, {}, {1, 1, 1}});

    std::shared_ptr<IComponentArray> clonedBase = arr.Clone();
    auto* cloned = static_cast<ComponentArray<TransformComponent>*>(clonedBase.get());
    EXPECT_EQ(cloned->Size(), (size_t)2);
    EXPECT(cloned->Has(5));
    EXPECT(cloned->Has(5000));
    EXPECT_EQ(cloned->Get(5)->Position.x, 5.0f);
    EXPECT_EQ(cloned->Get(5000)->Position.x, 50.0f);

    // Mutate original across both pages; clone must be unaffected.
    arr.Get(5)->Position.x = 111.0f;
    arr.Remove(5000);
    EXPECT_EQ(cloned->Get(5)->Position.x, 5.0f);
    EXPECT(cloned->Has(5000));
    EXPECT_EQ(cloned->Get(5000)->Position.x, 50.0f);

    // Copy-assignment yields an independent array too.
    ComponentArray<TransformComponent> assigned;
    assigned = *cloned;
    EXPECT(assigned.Has(5));
    EXPECT(assigned.Has(5000));
    cloned->Get(5)->Position.x = 222.0f;
    EXPECT_EQ(assigned.Get(5)->Position.x, 5.0f);
}

static void TP01_snapshot_pool_reuses_object()
{
    ECS w;
    w.AddComponent(w.CreateEntity(), TransformComponent{});
    auto s1 = w.CreateSnapshot();
    const ECS* p = s1.get();
    s1.reset();                       // recycle p
    auto s2 = w.CreateSnapshot();     // LIFO -> reacquires p
    EXPECT_EQ(s2.get(), p);
}

static void TP02_snapshot_isolated_after_recycle()
{
    ECS w;
    EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{{1.0f, 0, 0}, {}, {1, 1, 1}});

    auto s1 = w.CreateSnapshot();
    s1.reset();                       // recycle the object s2 will reuse

    w.Modify<TransformComponent>(e, [](auto& t){ t.Position.x = 9.0f; });
    auto s2 = w.CreateSnapshot();     // reuses the recycled object

    const auto* t = s2->GetComponent<TransformComponent>(e);
    EXPECT(t != nullptr);
    EXPECT_EQ(t->Position.x, 9.0f);   // current master state, not stale s1 (1.0)

    w.Modify<TransformComponent>(e, [](auto& tt){ tt.Position.x = 5.0f; });
    EXPECT_EQ(s2->GetComponent<TransformComponent>(e)->Position.x, 9.0f); // COW isolation
}

static void TP03_snapshot_pool_stats_deltas()
{
    const SnapshotPoolStats before = GetSnapshotPoolStats();

    ECS w;
    w.AddComponent(w.CreateEntity(), TransformComponent{});
    auto s = w.CreateSnapshot();
    const SnapshotPoolStats held = GetSnapshotPoolStats();
    EXPECT_EQ(held.InUse, before.InUse + 1);

    s.reset();
    const SnapshotPoolStats dropped = GetSnapshotPoolStats();
    EXPECT_EQ(dropped.InUse, before.InUse);
    EXPECT(dropped.Free >= 1);

    auto s2 = w.CreateSnapshot();     // served from the free-list
    const SnapshotPoolStats reused = GetSnapshotPoolStats();
    EXPECT(reused.Reuses > before.Reuses);
    s2.reset();
}

static void TM01_componentarray_memorybytes_used_exact()
{
    ComponentArray<TransformComponent> arr;
    arr.Add(1, TransformComponent{});
    arr.Add(2, TransformComponent{});
    arr.Add(3, TransformComponent{});

    ArrayMemory m = arr.MemoryBytes();
    EXPECT_EQ(m.Used, (size_t)(3 * sizeof(TransformComponent) + 3 * sizeof(EntityId)));
    EXPECT(m.Reserved >= m.Used);
    EXPECT(m.Reserved >= (size_t)4096);   // >= one 4 KB sparse page allocated

    arr.Remove(2);                         // swap-and-pop -> 2 live
    ArrayMemory m2 = arr.MemoryBytes();
    EXPECT_EQ(m2.Used, (size_t)(2 * sizeof(TransformComponent) + 2 * sizeof(EntityId)));
}

static void TM02_componentarray_empty_memorybytes()
{
    ComponentArray<TransformComponent> arr;
    ArrayMemory m = arr.MemoryBytes();
    EXPECT_EQ(m.Used, (size_t)0);
}

static void TM03_ecs_memorystats_aggregates()
{
    ECS w;
    EntityId a = w.CreateEntity();
    EntityId b = w.CreateEntity();
    w.AddComponent(a, TransformComponent{});
    w.AddComponent(b, TransformComponent{});
    w.AddComponent(a, MeshComponent{});

    EcsMemoryStats s = w.MemoryStats();
    EXPECT_EQ(s.EntityCount, (size_t)2);    // singleton reserved entity excluded
    EXPECT_EQ(s.ArrayCount, (size_t)2);     // Transform + Mesh
    EXPECT(s.ComponentUsed >= (size_t)(2 * sizeof(TransformComponent) + 1 * sizeof(MeshComponent)));
    EXPECT(s.ComponentReserved >= s.ComponentUsed);
    EXPECT(s.EntityReserved >= s.EntityUsed);
}

static void TCF01_copyfrom_reproduces_source_over_stale_dest()
{
    // src: ids spanning >= 3 sparse pages (page size 1024)
    ComponentArray<TransformComponent> src;
    src.Add(1,    TransformComponent{{1.0f,  0, 0}, {}, {1,1,1}});
    src.Add(5,    TransformComponent{{5.0f,  0, 0}, {}, {1,1,1}});
    src.Add(1025, TransformComponent{{25.0f, 0, 0}, {}, {1,1,1}}); // page 1
    src.Add(2049, TransformComponent{{49.0f, 0, 0}, {}, {1,1,1}}); // page 2

    // dest: DIFFERENT prior layout — different ids and MORE pages than src
    ComponentArray<TransformComponent> dest;
    dest.Add(2,    TransformComponent{{2.0f,  0, 0}, {}, {1,1,1}});
    dest.Add(3000, TransformComponent{{30.0f, 0, 0}, {}, {1,1,1}}); // page 2 (different id)
    dest.Add(9000, TransformComponent{{90.0f, 0, 0}, {}, {1,1,1}}); // page 8 -> dest has more pages

    dest.CopyFrom(src);

    // dest now mirrors src exactly
    EXPECT_EQ(dest.Size(), src.Size());
    EXPECT(dest.Has(1));    EXPECT_EQ(dest.Get(1)->Position.x,    1.0f);
    EXPECT(dest.Has(5));    EXPECT_EQ(dest.Get(5)->Position.x,    5.0f);
    EXPECT(dest.Has(1025)); EXPECT_EQ(dest.Get(1025)->Position.x, 25.0f);
    EXPECT(dest.Has(2049)); EXPECT_EQ(dest.Get(2049)->Position.x, 49.0f);
    // stale entries from the larger old layout must be gone
    EXPECT(!dest.Has(2));
    EXPECT(!dest.Has(3000));
    EXPECT(!dest.Has(9000));

    // deep copy: mutating src must not affect dest
    src.Get(1)->Position.x = 999.0f;
    EXPECT_EQ(dest.Get(1)->Position.x, 1.0f);
}

static void TCF02_copyfrom_empty_src_clears_dest()
{
    ComponentArray<TransformComponent> src; // empty
    ComponentArray<TransformComponent> dest;
    dest.Add(1,    TransformComponent{{1.0f, 0, 0}, {}, {1,1,1}});
    dest.Add(1025, TransformComponent{{2.0f, 0, 0}, {}, {1,1,1}});

    dest.CopyFrom(src);
    EXPECT_EQ(dest.Size(), (size_t)0);
    EXPECT(!dest.Has(1));
    EXPECT(!dest.Has(1025));
}

static void TCF03_copyfrom_reuses_dense_capacity()
{
    ComponentArray<TransformComponent> dest;
    for (EntityId i = 1; i <= 64; ++i) dest.Add(i, TransformComponent{});
    const size_t capBefore = dest.GetComponents().capacity();

    ComponentArray<TransformComponent> src;
    src.Add(1, TransformComponent{{7.0f, 0, 0}, {}, {1,1,1}}); // far smaller than dest

    dest.CopyFrom(src);
    EXPECT_EQ(dest.Size(), (size_t)1);
    EXPECT_EQ(dest.Get(1)->Position.x, 7.0f);
    EXPECT_EQ(dest.GetComponents().capacity(), capBefore); // capacity reused, not shrunk/realloc'd
}

static void TCOW01_pool_reuses_recycled_array()
{
    ECS w;
    EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{{1.0f, 0, 0}, {}, {1,1,1}});

    auto s1 = w.CreateSnapshot();                                   // master slot = A; s1 shares A
    const void* pA = static_cast<const void*>(s1->GetArray<TransformComponent>());
    w.Modify<TransformComponent>(e, [](auto& t){ t.Position.x = 2.0f; }); // clone -> master = B
    const void* pB = static_cast<const void*>(w.GetArray<TransformComponent>());
    EXPECT_NE(pA, pB);                                              // B is a fresh array
    s1.reset();                                                    // A refcount 0 -> recycled

    auto s2 = w.CreateSnapshot();                                  // master = B; s2 shares B; dirty cleared
    w.Modify<TransformComponent>(e, [](auto& t){ t.Position.x = 3.0f; }); // clone -> Acquire pool -> reuse A
    const void* pReused = static_cast<const void*>(w.GetArray<TransformComponent>());
    EXPECT_EQ(pReused, pA);                                        // recycled A came back from the pool
    EXPECT_NE(pReused, pB);
    s2.reset();
}

static void TCOW02_pooled_clone_preserves_isolation_after_recycle()
{
    ECS w;
    EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{{1.0f, 0, 0}, {}, {1,1,1}});

    auto s1 = w.CreateSnapshot();
    w.Modify<TransformComponent>(e, [](auto& t){ t.Position.x = 2.0f; }); // clone B
    s1.reset();                                                    // recycle A

    auto s2 = w.CreateSnapshot();                                  // s2 shares B (x == 2)
    EXPECT_EQ(s2->GetComponent<TransformComponent>(e)->Position.x, 2.0f);
    w.Modify<TransformComponent>(e, [](auto& t){ t.Position.x = 3.0f; }); // reuse A from pool, CopyFrom B

    // the reuse-clone into recycled A must NOT corrupt the live snapshot B
    EXPECT_EQ(s2->GetComponent<TransformComponent>(e)->Position.x, 2.0f);
    EXPECT_EQ(w.GetComponent<TransformComponent>(e)->Position.x, 3.0f);
    s2.reset();
}

static void TCOW03_array_pool_stats_deltas()
{
    ECS w;
    EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{});
    auto s1 = w.CreateSnapshot();                                  // s1 shares A1
    w.Modify<TransformComponent>(e, [](auto& t){ t.Position.x = 1.0f; }); // clone A1 -> A2 (master)

    const ComponentArrayPoolStats held = GetComponentArrayPoolStats();
    s1.reset();                                                    // A1 recycled -> Free +1, InUse -1
    const ComponentArrayPoolStats afterDrop = GetComponentArrayPoolStats();
    EXPECT_EQ(afterDrop.Free, held.Free + 1);
    EXPECT_EQ(afterDrop.InUse, held.InUse - 1);

    auto s2 = w.CreateSnapshot();                                  // shares A2 (no array-pool op)
    w.Modify<TransformComponent>(e, [](auto& t){ t.Position.x = 2.0f; }); // clone -> reuse A1
    const ComponentArrayPoolStats reused = GetComponentArrayPoolStats();
    EXPECT(reused.Reuses > held.Reuses);                           // a free-list reuse happened
    EXPECT_EQ(reused.Free, afterDrop.Free - 1);                    // free-list consumed by the reuse
    s2.reset();
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
    T05_snapshot_isolates_reads_from_writes();
    T06_snapshot_unchanged_arrays_share_storage();
    T09_create_snapshot_clears_dirty();
    T10_multi_snapshot_lifetime();
    T_addcomponent_after_snapshot_clones();
    T_removecomponent_after_snapshot_clones();
    T11_destroyed_entity_id_reuse();
    T12_concurrent_smoke();
    TS01_register_and_count();
    TS02_runs_in_phase_then_registration_order();
    TS03_clear_destroys_systems();
    TS04_empty_run_is_noop();
    TS05_system_mutates_ecs();
    TSG01_set_get_roundtrip();
    TSG02_modify_singleton();
    TSG03_singleton_entity_hidden();
    TSG04_snapshot_preserves_singletons();
    TSG05_clear_preserves_singletons_removes_gameplay();
    TE01_each_visits_matching_entities();
    TE02_each_components_form();
    TE03_each_zero_matches();
    TE04_each_single_component();
    TE05_each_does_not_change_entity_count();
    TE06_each_components_form_multi_entity_aliases_storage();
    TS01_componentarray_cross_page_ids();
    TS02_componentarray_high_id_only();
    TS03_componentarray_swap_and_pop_preserves_others();
    TS04_componentarray_remove_then_readd_same_id();
    TS05_componentarray_update_existing();
    TS06_componentarray_clone_independent_after_swap_and_pop();
    TS07_clone_and_assign_independent_across_pages();
    TP01_snapshot_pool_reuses_object();
    TP02_snapshot_isolated_after_recycle();
    TP03_snapshot_pool_stats_deltas();
    TM01_componentarray_memorybytes_used_exact();
    TM02_componentarray_empty_memorybytes();
    TM03_ecs_memorystats_aggregates();
    TCF01_copyfrom_reproduces_source_over_stale_dest();
    TCF02_copyfrom_empty_src_clears_dest();
    TCF03_copyfrom_reuses_dense_capacity();
    TCOW01_pool_reuses_recycled_array();
    TCOW02_pooled_clone_preserves_isolation_after_recycle();
    TCOW03_array_pool_stats_deltas();

    if (g_Failures) {
        SM_ERROR("%d ECS test(s) failed", g_Failures);
        return 1;
    }
    SM_TRACE("All ECS tests passed.");
    return 0;
}
