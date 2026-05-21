#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>

#include "lib.h"
#include <memory/MemUtil.h>
#include <memory/MemoryCategory.h>
#include <memory/IAllocator.h>
#include <memory/AllocatorRegistry.h>
#include <memory/ArenaAllocator.h>
#include <memory/PoolAllocator.h>
#include <MeshBatching.h>

// Required by lib.h's SM_ASSERT. Test exe: print + abort, no MessageBox.
void platform_debug_break(const char* expr, const char* file, int line, const char* message)
{
    std::fprintf(stderr, "ASSERT FAIL %s:%d: %s (expr: %s)\n",
                 (file ? file : "<unknown>"), line,
                 (message ? message : "<no message>"),
                 (expr ? expr : "<none>"));
    std::abort();
}

static int g_Failures = 0;

#define EXPECT(cond)                                               \
    do {                                                           \
        if (!(cond)) {                                             \
            SM_ERROR("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                          \
        }                                                          \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))
#define EXPECT_NE(a, b) EXPECT((a) != (b))

static void T00_smoke()
{
    EXPECT_EQ(1 + 1, 2);
}

static void T01_alignup()
{
    EXPECT_EQ(Engine::AlignUp(0, 16), (size_t)0);
    EXPECT_EQ(Engine::AlignUp(1, 16), (size_t)16);
    EXPECT_EQ(Engine::AlignUp(16, 16), (size_t)16);
    EXPECT_EQ(Engine::AlignUp(17, 16), (size_t)32);
    EXPECT_EQ(Engine::AlignUp(13, 8), (size_t)16);
}

static void T02_category_tostring()
{
    EXPECT_EQ(std::string(Engine::ToString(Engine::MemCategory::ECS)), std::string("ECS"));
    EXPECT_EQ(std::string(Engine::ToString(Engine::MemCategory::FrameTransient)),
              std::string("FrameTransient"));
}

// Minimal IAllocator for registry tests — does not actually allocate.
class FakeAllocator final : public Engine::IAllocator {
public:
    FakeAllocator(Engine::MemCategory cat, const char* name, size_t used, size_t cap,
                  uint64_t allocCount = 0, uint64_t freeCount = 0)
        : m_Category(cat), m_Name(name) {
        m_Stats.Used = used; m_Stats.Peak = used; m_Stats.Capacity = cap;
        m_Stats.AllocCount = allocCount; m_Stats.FreeCount = freeCount;
    }
    void* Allocate(size_t, size_t) override { return nullptr; }
    void  Deallocate(void*, size_t) override {}
    const Engine::AllocatorStats& Stats() const override { return m_Stats; }
    Engine::MemCategory Category() const override { return m_Category; }
    const char* Name() const override { return m_Name; }
private:
    Engine::MemCategory   m_Category;
    const char*           m_Name;
    Engine::AllocatorStats m_Stats;
};

static void T10_registry_register_unregister()
{
    auto& reg = Engine::Registry();
    const size_t before = reg.Count();
    FakeAllocator a(Engine::MemCategory::Mesh, "A", 100, 1000);
    reg.Register(&a);
    EXPECT_EQ(reg.Count(), before + 1);
    reg.Unregister(&a);
    EXPECT_EQ(reg.Count(), before);
}

static void T11_registry_foreach_and_sum()
{
    auto& reg = Engine::Registry();
    // baseline captured before registering (registry is a process-wide singleton)
    Engine::AllocatorStats meshBase = reg.SumByCategory(Engine::MemCategory::Mesh);
    FakeAllocator a(Engine::MemCategory::Mesh, "A", 100, 1000, 5, 2);
    FakeAllocator b(Engine::MemCategory::Mesh, "B", 250, 2000, 7, 3);
    FakeAllocator c(Engine::MemCategory::Game, "C", 999, 9999, 11, 4);
    reg.Register(&a); reg.Register(&b); reg.Register(&c);

    int seen = 0;
    reg.ForEach([&](Engine::IAllocator*){ ++seen; });
    EXPECT(seen >= 3);

    Engine::AllocatorStats mesh = reg.SumByCategory(Engine::MemCategory::Mesh);
    EXPECT_EQ(mesh.Used - meshBase.Used, (size_t)350);
    EXPECT_EQ(mesh.Capacity - meshBase.Capacity, (size_t)3000);
    EXPECT_EQ(mesh.Peak - meshBase.Peak, (size_t)350);
    EXPECT_EQ(mesh.AllocCount - meshBase.AllocCount, (uint64_t)12);
    EXPECT_EQ(mesh.FreeCount - meshBase.FreeCount, (uint64_t)5);

    reg.Unregister(&a); reg.Unregister(&b); reg.Unregister(&c);
}

static void T20_arena_bump_and_align()
{
    Engine::ArenaAllocator a(1024, Engine::MemCategory::FrameTransient, "T20");
    void* p0 = a.Allocate(10, 16);
    void* p1 = a.Allocate(10, 16);
    EXPECT_NE(p0, nullptr);
    EXPECT_NE(p1, nullptr);
    EXPECT_EQ((uintptr_t)p0 % 16, (uintptr_t)0);
    EXPECT_EQ((uintptr_t)p1 % 16, (uintptr_t)0);
    EXPECT(p1 != p0);
    EXPECT(a.Stats().Used >= 20);
}

static void T21_arena_overflow_returns_null()
{
    Engine::ArenaAllocator a(64, Engine::MemCategory::FrameTransient, "T21");
    void* ok = a.Allocate(32, 8);
    EXPECT_NE(ok, nullptr);
    void* fail = a.Allocate(1024, 8); // exceeds capacity
    EXPECT_EQ(fail, nullptr);
}

static void T22_arena_reset_keeps_peak()
{
    Engine::ArenaAllocator a(1024, Engine::MemCategory::FrameTransient, "T22");
    a.Allocate(500, 8);
    const size_t peak = a.Stats().Peak;
    EXPECT(peak >= 500);
    a.Reset();
    EXPECT_EQ(a.Stats().Used, (size_t)0);
    EXPECT_EQ(a.Stats().Peak, peak); // peak retained
}

static void T23_arena_marker_rewind()
{
    Engine::ArenaAllocator a(1024, Engine::MemCategory::FrameTransient, "T23");
    a.Allocate(100, 8);
    Engine::ArenaAllocator::Marker m = a.GetMarker();
    a.Allocate(200, 8);
    EXPECT(a.Stats().Used > (size_t)m);
    a.RewindTo(m);
    EXPECT_EQ(a.Stats().Used, (size_t)m);
}

static void T24_arena_allocate_array_and_typed()
{
    Engine::ArenaAllocator a(1024, Engine::MemCategory::FrameTransient, "T24");
    int* arr = a.AllocateArray<int>(8);
    EXPECT_NE(arr, nullptr);
    EXPECT_EQ((uintptr_t)arr % alignof(int), (uintptr_t)0);
    double* d = a.Allocate<double>();
    EXPECT_NE(d, nullptr);
    EXPECT_EQ((uintptr_t)d % alignof(double), (uintptr_t)0);
}

static void T25_arena_external_buffer()
{
    alignas(16) static unsigned char buf[256];
    Engine::ArenaAllocator a(buf, sizeof(buf), Engine::MemCategory::General, "T25");
    void* p = a.Allocate(16, 16);
    EXPECT_NE(p, nullptr);
    EXPECT(p >= (void*)buf);
    EXPECT(p < (void*)(buf + sizeof(buf)));
    EXPECT_EQ(a.Stats().Capacity, (size_t)256);
}

static void T26_arena_compat_getters_and_reuse()
{
    Engine::ArenaAllocator a(1024, Engine::MemCategory::FrameTransient, "T26");

    // AllocateArray<T>(0) returns null and does not consume space.
    int* none = a.AllocateArray<int>(0);
    EXPECT_EQ(none, nullptr);
    EXPECT_EQ(a.GetUsedBytes(), (size_t)0);

    void* p0 = a.Allocate(100, 8);
    EXPECT_NE(p0, nullptr);

    // Compat getters mirror Stats().
    EXPECT_EQ(a.GetUsedBytes(), a.Stats().Used);
    EXPECT_EQ(a.GetCapacity(), a.Stats().Capacity);
    EXPECT_EQ(a.GetPeakUsage(), a.Stats().Peak);
    EXPECT_EQ(a.GetCapacity(), (size_t)1024);

    // Reset hands the space back: next alloc returns the same base pointer.
    a.Reset();
    void* p1 = a.Allocate(100, 8);
    EXPECT_EQ(p0, p1);

    // RewindTo to a marker also reuses the region beyond the marker.
    Engine::ArenaAllocator::Marker m = a.GetMarker();
    void* q0 = a.Allocate(64, 8);
    EXPECT_NE(q0, nullptr);
    a.RewindTo(m);
    void* q1 = a.Allocate(64, 8);
    EXPECT_EQ(q0, q1);
}

static void T30_pool_alloc_free_reuse()
{
    Engine::PoolAllocator p(32, 4, 16, Engine::MemCategory::Mesh, "T30");
    void* a = p.Allocate(32, 16);
    EXPECT_NE(a, nullptr);
    EXPECT_EQ((uintptr_t)a % 16, (uintptr_t)0);
    p.Deallocate(a);
    void* b = p.Allocate(32, 16);
    EXPECT_EQ(a, b); // freed block is reused
}

static void T31_pool_grows_and_keeps_pointers_valid()
{
    Engine::PoolAllocator p(sizeof(uint64_t), 2, alignof(uint64_t),
                            Engine::MemCategory::Mesh, "T31");
    // Fill the first slab.
    auto* x0 = static_cast<uint64_t*>(p.Allocate(sizeof(uint64_t), alignof(uint64_t)));
    auto* x1 = static_cast<uint64_t*>(p.Allocate(sizeof(uint64_t), alignof(uint64_t)));
    EXPECT_NE(x0, nullptr);
    EXPECT_NE(x1, nullptr);
    *x0 = 0xAAAA; *x1 = 0xBBBB;
    const size_t capBefore = p.Stats().Capacity;

    // This forces a new slab (growth).
    auto* x2 = static_cast<uint64_t*>(p.Allocate(sizeof(uint64_t), alignof(uint64_t)));
    EXPECT_NE(x2, nullptr);
    *x2 = 0xCCCC;

    EXPECT(p.Stats().Capacity > capBefore);   // grew
    EXPECT_EQ(*x0, (uint64_t)0xAAAA);          // earlier-slab pointers still valid
    EXPECT_EQ(*x1, (uint64_t)0xBBBB);
    EXPECT_EQ(*x2, (uint64_t)0xCCCC);
}

static void T32_pool_reset_frees_all()
{
    Engine::PoolAllocator p(32, 2, 16, Engine::MemCategory::Mesh, "T32");
    p.Allocate(32, 16);
    p.Allocate(32, 16);
    EXPECT(p.Stats().Used > 0);
    p.Reset();
    EXPECT_EQ(p.Stats().Used, (size_t)0);
    void* a = p.Allocate(32, 16); // allocatable again
    EXPECT_NE(a, nullptr);
}

static void T33_pool_dealloc_updates_stats()
{
    Engine::PoolAllocator p(32, 4, 16, Engine::MemCategory::Mesh, "T33");
    void* a = p.Allocate(32, 16);
    EXPECT_NE(a, nullptr);
    const size_t usedAfterAlloc = p.Stats().Used;
    EXPECT(usedAfterAlloc > 0);
    p.Deallocate(a);
    EXPECT_EQ(p.Stats().Used, (size_t)0);
    EXPECT_EQ(p.Stats().FreeCount, (uint64_t)1);
    EXPECT_EQ(p.Stats().AllocCount, (uint64_t)1);
}

static void T34_pool_reset_rebuilds_full_freelist_across_slabs()
{
    // blockCount 2; allocate 3 to force a second slab (total capacity 4 blocks).
    Engine::PoolAllocator p(32, 2, 16, Engine::MemCategory::Mesh, "T34");
    void* a = p.Allocate(32, 16);
    void* b = p.Allocate(32, 16);
    void* c = p.Allocate(32, 16); // forces growth -> 2 slabs, 4 blocks total
    EXPECT_NE(a, nullptr); EXPECT_NE(b, nullptr); EXPECT_NE(c, nullptr);
    const size_t totalBlocks = p.Stats().Capacity / 32; // stride==32 here
    EXPECT_EQ(totalBlocks, (size_t)4);

    p.Reset();
    EXPECT_EQ(p.Stats().Used, (size_t)0);

    // Every block across both slabs must be allocatable again, none null.
    int handed = 0;
    for (size_t i = 0; i < totalBlocks; ++i) {
        void* blk = p.Allocate(32, 16);
        if (blk) ++handed;
    }
    EXPECT_EQ((size_t)handed, totalBlocks);
}

static void T40_batchruns_empty()
{
    BatchRun runs[1];
    EXPECT_EQ(BuildBatchRuns(nullptr, 0, runs, 1), (uint32_t)0);
    BatchEntry e[1] = {};
    EXPECT_EQ(BuildBatchRuns(e, 0, runs, 1), (uint32_t)0);
}

static void T41_batchruns_single()
{
    BatchEntry e[1] = { {5, 7, 100} };
    BatchRun runs[1];
    EXPECT_EQ(BuildBatchRuns(e, 1, runs, 1), (uint32_t)1);
    EXPECT_EQ(runs[0].begin, (uint32_t)0);
    EXPECT_EQ(runs[0].count, (uint32_t)1);
}

static void T42_batchruns_all_same_key()
{
    BatchEntry e[3] = { {1,2,10}, {1,2,11}, {1,2,12} };
    BatchRun runs[3];
    EXPECT_EQ(BuildBatchRuns(e, 3, runs, 3), (uint32_t)1);
    EXPECT_EQ(runs[0].begin, (uint32_t)0);
    EXPECT_EQ(runs[0].count, (uint32_t)3);
}

static void T43_batchruns_all_distinct()
{
    BatchEntry e[3] = { {1,0,10}, {2,0,11}, {3,0,12} };
    BatchRun runs[3];
    EXPECT_EQ(BuildBatchRuns(e, 3, runs, 3), (uint32_t)3);
    EXPECT_EQ(runs[0].count, (uint32_t)1);
    EXPECT_EQ(runs[1].count, (uint32_t)1);
    EXPECT_EQ(runs[2].count, (uint32_t)1);
}

static void T44_batchruns_mixed_unsorted()
{
    // keys before sort: (2,0)(1,0)(2,0)(1,1)(1,0)
    // after sort:       (1,0)(1,0)(1,1)(2,0)(2,0) -> runs [0,2],[2,1],[3,2]
    BatchEntry e[5] = { {2,0,10}, {1,0,11}, {2,0,12}, {1,1,13}, {1,0,14} };
    BatchRun runs[5];
    uint32_t n = BuildBatchRuns(e, 5, runs, 5);
    EXPECT_EQ(n, (uint32_t)3);

    // Runs are contiguous and cover all entries.
    EXPECT_EQ(runs[0].begin, (uint32_t)0);
    EXPECT_EQ(runs[1].begin, runs[0].begin + runs[0].count);
    EXPECT_EQ(runs[2].begin, runs[1].begin + runs[1].count);
    uint32_t covered = 0;
    for (uint32_t r = 0; r < n; ++r) {
        covered += runs[r].count;
        const uint32_t b = runs[r].begin;
        for (uint32_t i = 1; i < runs[r].count; ++i) {
            EXPECT(e[b + i].meshId == e[b].meshId);
            EXPECT(e[b + i].materialId == e[b].materialId);
        }
    }
    EXPECT_EQ(covered, (uint32_t)5);
}

static void T45_batchruns_maxruns_cap()
{
    BatchEntry e[3] = { {1,0,10}, {2,0,11}, {3,0,12} };
    BatchRun runs[2];
    EXPECT_EQ(BuildBatchRuns(e, 3, runs, 2), (uint32_t)2); // stops at maxRuns
}

int main()
{
    T00_smoke();
    T01_alignup();
    T02_category_tostring();
    T10_registry_register_unregister();
    T11_registry_foreach_and_sum();
    T20_arena_bump_and_align();
    T21_arena_overflow_returns_null();
    T22_arena_reset_keeps_peak();
    T23_arena_marker_rewind();
    T24_arena_allocate_array_and_typed();
    T25_arena_external_buffer();
    T26_arena_compat_getters_and_reuse();
    T30_pool_alloc_free_reuse();
    T31_pool_grows_and_keeps_pointers_valid();
    T32_pool_reset_frees_all();
    T33_pool_dealloc_updates_stats();
    T34_pool_reset_rebuilds_full_freelist_across_slabs();
    T40_batchruns_empty();
    T41_batchruns_single();
    T42_batchruns_all_same_key();
    T43_batchruns_all_distinct();
    T44_batchruns_mixed_unsorted();
    T45_batchruns_maxruns_cap();

    if (g_Failures == 0) {
        std::printf("All allocator tests passed.\n");
        return 0;
    }
    std::printf("%d allocator test(s) FAILED.\n", g_Failures);
    return 1;
}
