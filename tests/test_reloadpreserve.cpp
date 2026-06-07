#include <cstdio>
#include <cmath>
#include <cstddef>
#include <string>
#include <nlohmann/json.hpp>
#include "ComponentSerializerRegistry.h"

// Test exe stub for SM_ASSERT's break hook (matches sibling tests).
void platform_debug_break(const char* expr, const char* file, int line, const char* message) {
    std::fprintf(stderr, "ASSERT FAIL %s:%d: %s (expr: %s)\n",
                 file ? file : "<unknown>", line, message ? message : "<none>", expr ? expr : "<none>");
    std::abort();
}

static int g_Failures = 0;
#define EXPECT(c) do { if(!(c)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++g_Failures; } } while(0)

// POD game component (no to_json) → byte path.
struct PodComp { int A = 0; float B = 0.0f; };
// Complex game component (heap member) → json path.
struct CplxComp { std::string S; int N = 0; };
inline void to_json(nlohmann::json& j, const CplxComp& p)   { j = nlohmann::json{ {"S", p.S}, {"N", p.N} }; }
inline void from_json(const nlohmann::json& j, CplxComp& p) { p.S = j.at("S").get<std::string>(); p.N = j.at("N").get<int>(); }
// Unpreservable: heap member, no to_json → neither path.
struct BadComp { std::string S; };

static void T01_byte_and_json_roundtrip_survive_clear()
{
    SerializerRegistry().Register<PodComp>("PodComp");
    SerializerRegistry().Register<CplxComp>("CplxComp");

    ECS w;
    const EntityId e1 = w.CreateEntity();
    const EntityId e2 = w.CreateEntity();
    w.AddComponent<PodComp>(e1, PodComp{ 7, 1.5f });
    w.AddComponent<CplxComp>(e2, CplxComp{ "hello", 9 });

    auto blob = PreserveNonBuiltinComponents(w);
    EXPECT(blob.size() == 2);

    w.RemoveNonBuiltinComponentArrays();
    EXPECT(!w.HasComponent<PodComp>(e1));
    EXPECT(!w.HasComponent<CplxComp>(e2));

    RestoreNonBuiltinComponents(w, blob);
    EXPECT(w.HasComponent<PodComp>(e1));
    EXPECT(w.HasComponent<CplxComp>(e2));
    const PodComp*  p = w.GetComponent<PodComp>(e1);
    const CplxComp* c = w.GetComponent<CplxComp>(e2);
    EXPECT(p && p->A == 7 && std::fabs(p->B - 1.5f) < 1e-6f);
    EXPECT(c && c->S == "hello" && c->N == 9);
}

static void T02_unpreservable_is_skipped()
{
    SerializerRegistry().Register<BadComp>("BadComp"); // neither path

    ECS w;
    const EntityId e = w.CreateEntity();
    w.AddComponent<BadComp>(e, BadComp{ "x" });

    auto blob = PreserveNonBuiltinComponents(w); // warns about BadComp
    for (const auto& pc : blob) EXPECT(pc.name != "BadComp"); // not preserved

    w.RemoveNonBuiltinComponentArrays();
    RestoreNonBuiltinComponents(w, blob);
    EXPECT(!w.HasComponent<BadComp>(e)); // dropped, as designed
}

static void T03_builtins_untouched_by_preserve()
{
    ECS w;
    const EntityId e = w.CreateEntity();
    w.AddComponent<TransformComponent>(e, TransformComponent{});
    auto blob = PreserveNonBuiltinComponents(w);
    for (const auto& pc : blob) EXPECT(pc.name != "TransformComponent"); // builtin excluded
}

int main()
{
    T01_byte_and_json_roundtrip_survive_clear();
    T02_unpreservable_is_skipped();
    T03_builtins_untouched_by_preserve();
    if (g_Failures == 0) { std::printf("All reload-preservation tests passed.\n"); return 0; }
    std::printf("%d reload-preservation test(s) FAILED.\n", g_Failures);
    return 1;
}
