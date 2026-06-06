#include <cstdio>
#include <cmath>
#include <nlohmann/json.hpp>
#include "ComponentSerializerRegistry.h"

// Test exe stub for SM_ASSERT's break hook (matches sibling tests like test_ecs.cpp).
void platform_debug_break(const char* expr, const char* file, int line, const char* message) {
    std::fprintf(stderr, "ASSERT FAIL %s:%d: %s (expr: %s)\n",
                 file ? file : "<unknown>", line, message ? message : "<none>", expr ? expr : "<none>");
    std::abort();
}

static int g_Failures = 0;
#define EXPECT(c) do { if(!(c)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++g_Failures; } } while(0)

// A synthetic game-style component defined OUTSIDE ecs.dll (in this TU) and NOT in the
// ECS X-macro — exactly the case Piece 4 must support. Serializers via nlohmann ADL.
struct PersistProbe { int A = 0; float B = 0.0f; };
inline void to_json(nlohmann::json& j, const PersistProbe& p)   { j = nlohmann::json{ {"A", p.A}, {"B", p.B} }; }
inline void from_json(const nlohmann::json& j, PersistProbe& p) { p.A = j.at("A").get<int>(); p.B = j.at("B").get<float>(); }

static void T01_builtins_registered()
{
    EXPECT(SerializerRegistry().Find("TransformComponent") != nullptr);
    EXPECT(SerializerRegistry().Find("StateScopeComponent") != nullptr);
    EXPECT(SerializerRegistry().Find("NavClassComponent")   != nullptr);
    EXPECT(SerializerRegistry().Find("NopeNotReal")         == nullptr);
}

static void T02_register_and_roundtrip_game_component()
{
    SerializerRegistry().Register<PersistProbe>("PersistProbe");

    ECS w;
    const EntityId e = w.CreateEntity();
    w.AddComponent<PersistProbe>(e, PersistProbe{ 42, 1.5f });

    nlohmann::json jEntity;
    jEntity["EntityId"] = e;
    SaveEntityComponents(w, e, jEntity);
    EXPECT(jEntity.contains("PersistProbe"));
    EXPECT(jEntity["PersistProbe"]["A"].get<int>() == 42);

    ECS w2;
    const EntityId e2 = w2.CreateEntity();
    LoadEntityComponents(w2, e2, jEntity); // must skip "EntityId", load "PersistProbe"
    EXPECT(w2.HasComponent<PersistProbe>(e2));
    const PersistProbe* got = w2.GetComponent<PersistProbe>(e2);
    EXPECT(got != nullptr);
    EXPECT(got && got->A == 42);
    EXPECT(got && std::fabs(got->B - 1.5f) < 1e-6f);
}

static void T03_register_upserts_no_duplicate()
{
    SerializerRegistry().Register<PersistProbe>("PersistProbe");
    const size_t before = SerializerRegistry().Entries().size();
    SerializerRegistry().Register<PersistProbe>("PersistProbe"); // same name again
    EXPECT(SerializerRegistry().Entries().size() == before); // upsert, not append
}

int main()
{
    T01_builtins_registered();
    T02_register_and_roundtrip_game_component();
    T03_register_upserts_no_duplicate();
    if (g_Failures == 0) { std::printf("All component-serializer tests passed.\n"); return 0; }
    std::printf("%d component-serializer test(s) FAILED.\n", g_Failures);
    return 1;
}
