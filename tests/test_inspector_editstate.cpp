// tests/test_inspector_editstate.cpp
#include <cstdio>
#include <cstdlib>

#include <glm/glm.hpp>

#include "ECS.h"
#include "EditorContext.h"
#include "EditState.h"

void platform_debug_break(const char* expr, const char* file, int line, const char* message) {
    std::fprintf(stderr, "ASSERT FAIL %s:%d: %s (expr: %s)\n",
                 (file ? file : "<unknown>"), line,
                 (message ? message : "<no message>"), (expr ? expr : "<none>"));
    std::abort();
}

static int g_Failures = 0;
#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                     \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_Failures;                                                  \
    } } while (0)

// Build a snapshot-backed EditorContext. App stays null — Begin never touches it.
static EditorContext CtxFor(std::shared_ptr<const ECS>& snap) {
    EditorContext ctx{};
    ctx.WorldSnapshot = snap;
    return ctx;
}

static void T01_entity_switch_resets() {
    ECS w;
    const EntityId a = w.CreateEntity();
    w.AddComponent(a, NavTargetComponent{ glm::vec3(1, 2, 3) });
    const EntityId b = w.CreateEntity();
    w.AddComponent(b, NavTargetComponent{ glm::vec3(7, 8, 9) });
    auto snap = w.CreateSnapshot();
    auto ctx = CtxFor(snap);

    EditState<NavTargetComponent> st;
    const auto* ca = st.Begin(ctx, a);
    EXPECT(ca != nullptr);
    EXPECT(st.last == a);
    EXPECT(st.edit.Destination == glm::vec3(1, 2, 3));

    // Pretend user edited, then switch entities -> reset to b's snapshot value, modified cleared.
    st.modified = true;
    st.edit.Destination = glm::vec3(99, 99, 99);
    const auto* cb = st.Begin(ctx, b);
    EXPECT(cb != nullptr);
    EXPECT(st.last == b);
    EXPECT(st.modified == false);
    EXPECT(st.edit.Destination == glm::vec3(7, 8, 9));
}

static void T02_live_refresh_while_not_modified() {
    ECS w;
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, NavTargetComponent{ glm::vec3(1, 0, 0) });
    auto snap1 = w.CreateSnapshot();
    auto ctx1 = CtxFor(snap1);

    EditState<NavTargetComponent> st;
    st.Begin(ctx1, e);
    EXPECT(st.edit.Destination == glm::vec3(1, 0, 0));

    // Game mutates the component; a fresh snapshot reflects it. Not modified -> refresh.
    w.Modify<NavTargetComponent>(e, [](NavTargetComponent& t){ t.Destination = glm::vec3(5, 0, 0); });
    auto snap2 = w.CreateSnapshot();
    auto ctx2 = CtxFor(snap2);
    st.Begin(ctx2, e);
    EXPECT(st.edit.Destination == glm::vec3(5, 0, 0));   // refreshed
}

static void T03_preserve_while_modified() {
    ECS w;
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, NavTargetComponent{ glm::vec3(1, 0, 0) });
    auto snap1 = w.CreateSnapshot();
    auto ctx1 = CtxFor(snap1);

    EditState<NavTargetComponent> st;
    st.Begin(ctx1, e);

    // User is editing.
    st.modified = true;
    st.edit.Destination = glm::vec3(42, 0, 0);

    // Snapshot changes underneath, but modified==true -> preserve user edit.
    w.Modify<NavTargetComponent>(e, [](NavTargetComponent& t){ t.Destination = glm::vec3(5, 0, 0); });
    auto snap2 = w.CreateSnapshot();
    auto ctx2 = CtxFor(snap2);
    st.Begin(ctx2, e);
    EXPECT(st.edit.Destination == glm::vec3(42, 0, 0));  // preserved, NOT overwritten
}

static void T04_absent_component_returns_null() {
    ECS w;
    const EntityId e = w.CreateEntity();  // no NavTargetComponent
    auto snap = w.CreateSnapshot();
    auto ctx = CtxFor(snap);

    EditState<NavTargetComponent> st;
    EXPECT(st.Begin(ctx, e) == nullptr);
}

int main() {
    T01_entity_switch_resets();
    T02_live_refresh_while_not_modified();
    T03_preserve_while_modified();
    T04_absent_component_returns_null();

    if (g_Failures == 0) { std::printf("All inspector EditState tests passed.\n"); return 0; }
    std::fprintf(stderr, "inspector EditState tests: %d failure(s)\n", g_Failures);
    return 1;
}
