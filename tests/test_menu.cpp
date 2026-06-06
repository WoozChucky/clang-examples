#include <cstdio>
#include "StateScope.h" // ScopeAllows (uint32_t state index)
#include "MenuHitTest.h" // ToUiSpace

// Mirrors the game's GameStateId bit indices (game-owned enum; values are the
// implicit contract with StateMask bits). This test exercises the pure ScopeAllows
// masking logic in common, so it uses the raw indices directly.
namespace { constexpr uint32_t kMainMenu = 1, kInLevel = 2, kPaused = 4; }

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
    EXPECT(ScopeAllows(0u, kMainMenu));
    EXPECT(ScopeAllows(0u, kInLevel));
}

// A single-state mask matches only that state.
static void T01_single_state() {
    const uint32_t menu = 1u << kMainMenu;
    EXPECT(ScopeAllows(menu, kMainMenu));
    EXPECT(!ScopeAllows(menu, kInLevel));
}

// A multi-state mask matches any of its states.
static void T02_multi_state() {
    const uint32_t mask = (1u << kMainMenu) | (1u << kPaused);
    EXPECT(ScopeAllows(mask, kMainMenu));
    EXPECT(ScopeAllows(mask, kPaused));
    EXPECT(!ScopeAllows(mask, kInLevel));
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

// Half-open rect [pos, pos+size): inside, on each edge, outside.
static void T04_point_in_rect() {
    const glm::vec2 pos(100.0f, 50.0f), size(40.0f, 20.0f);
    EXPECT(PointInRect(glm::vec2(120.0f, 60.0f), pos, size));  // inside
    EXPECT(PointInRect(glm::vec2(100.0f, 50.0f), pos, size));  // top-left inclusive
    EXPECT(!PointInRect(glm::vec2(140.0f, 60.0f), pos, size)); // right edge exclusive (100+40)
    EXPECT(!PointInRect(glm::vec2(120.0f, 70.0f), pos, size)); // bottom edge exclusive (50+20)
    EXPECT(!PointInRect(glm::vec2(99.0f, 60.0f), pos, size));  // left of
    EXPECT(!PointInRect(glm::vec2(120.0f, 49.0f), pos, size)); // above
}

int main() {
    T00_unscoped_is_always_on();
    T01_single_state();
    T02_multi_state();
    T03_to_ui_space();
    T04_point_in_rect();
    if (g_Failures == 0) { std::printf("All menu tests passed.\n"); return 0; }
    std::printf("%d menu test(s) FAILED.\n", g_Failures);
    return 1;
}
