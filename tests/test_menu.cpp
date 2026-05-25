#include <cstdio>
#include "StateScope.h" // ScopeAllows + GameStateId
#include "MenuHitTest.h" // ToUiSpace

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

// mask == 0 => always-on (unscoped entity).
static void T00_unscoped_is_always_on() {
    EXPECT(ScopeAllows(0u, GameStateId::MainMenu));
    EXPECT(ScopeAllows(0u, GameStateId::InLevel));
}

// A single-state mask matches only that state.
static void T01_single_state() {
    const uint32_t menu = 1u << static_cast<uint32_t>(GameStateId::MainMenu);
    EXPECT(ScopeAllows(menu, GameStateId::MainMenu));
    EXPECT(!ScopeAllows(menu, GameStateId::InLevel));
}

// A multi-state mask matches any of its states.
static void T02_multi_state() {
    const uint32_t mask = (1u << static_cast<uint32_t>(GameStateId::MainMenu))
                        | (1u << static_cast<uint32_t>(GameStateId::Paused));
    EXPECT(ScopeAllows(mask, GameStateId::MainMenu));
    EXPECT(ScopeAllows(mask, GameStateId::Paused));
    EXPECT(!ScopeAllows(mask, GameStateId::InLevel));
}

// Window-space mouse -> UI/viewport space by subtracting the viewport origin.
static void T03_to_ui_space() {
    // Runtime: origin (0,0) -> identity.
    const glm::vec2 a = ToUiSpace(100.0, 50.0, 0u, 0u);
    EXPECT(a.x == 100.0f && a.y == 50.0f);
    // Editor: subtract the docked viewport's top-left.
    const glm::vec2 b = ToUiSpace(300.0, 220.0, 50u, 20u);
    EXPECT(b.x == 250.0f && b.y == 200.0f);
    // Mouse left/above the viewport origin -> negative UI coords (must NOT clamp).
    const glm::vec2 c = ToUiSpace(10.0, 5.0, 50u, 20u);
    EXPECT(c.x == -40.0f && c.y == -15.0f);
}

int main() {
    T00_unscoped_is_always_on();
    T01_single_state();
    T02_multi_state();
    T03_to_ui_space();
    if (g_Failures == 0) { std::printf("All menu tests passed.\n"); return 0; }
    std::printf("%d menu test(s) FAILED.\n", g_Failures);
    return 1;
}
