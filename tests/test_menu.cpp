#include <cstdio>
#include "StateScope.h" // ScopeAllows + GameStateId

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

int main() {
    T00_unscoped_is_always_on();
    T01_single_state();
    T02_multi_state();
    if (g_Failures == 0) { std::printf("All menu tests passed.\n"); return 0; }
    std::printf("%d menu test(s) FAILED.\n", g_Failures);
    return 1;
}
