#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>

#include "lib.h"
#include <memory/MemUtil.h>
#include <memory/MemoryCategory.h>
#include <memory/IAllocator.h>
#include <memory/AllocatorRegistry.h>

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
    FakeAllocator(Engine::MemCategory cat, const char* name, size_t used, size_t cap)
        : m_Category(cat), m_Name(name) { m_Stats.Used = used; m_Stats.Peak = used; m_Stats.Capacity = cap; }
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
    FakeAllocator a(Engine::MemCategory::Mesh, "A", 100, 1000);
    FakeAllocator b(Engine::MemCategory::Mesh, "B", 250, 2000);
    FakeAllocator c(Engine::MemCategory::Game, "C", 999, 9999);
    reg.Register(&a); reg.Register(&b); reg.Register(&c);

    int seen = 0;
    reg.ForEach([&](Engine::IAllocator*){ ++seen; });
    EXPECT(seen >= 3);

    Engine::AllocatorStats mesh = reg.SumByCategory(Engine::MemCategory::Mesh);
    EXPECT_EQ(mesh.Used - meshBase.Used, (size_t)350);
    EXPECT_EQ(mesh.Capacity - meshBase.Capacity, (size_t)3000);

    reg.Unregister(&a); reg.Unregister(&b); reg.Unregister(&c);
}

int main()
{
    T00_smoke();
    T01_alignup();
    T02_category_tostring();
    T10_registry_register_unregister();
    T11_registry_foreach_and_sum();

    if (g_Failures == 0) {
        std::printf("All allocator tests passed.\n");
        return 0;
    }
    std::printf("%d allocator test(s) FAILED.\n", g_Failures);
    return 1;
}
