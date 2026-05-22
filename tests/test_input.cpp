#include <cstdio>
#include "Input.h"
#include "InputRouting.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static void T00_smoke() { EXPECT(1 + 1 == 2); }

static void T01_cursor_locked_routes_all()
{
    EXPECT(RouteInputToGame(InputEventType::MouseMove,   true, false, false) == true);
    EXPECT(RouteInputToGame(InputEventType::MouseButton, true, false, false) == true);
    EXPECT(RouteInputToGame(InputEventType::MouseWheel,  true, false, false) == true);
    EXPECT(RouteInputToGame(InputEventType::Key,         true, false, false) == true);
    EXPECT(RouteInputToGame(InputEventType::TextInput,   true, false, false) == true);
}

static void T02_editor_mouse_follows_acceptsMouse()
{
    EXPECT(RouteInputToGame(InputEventType::MouseMove,   false, true,  false) == true);
    EXPECT(RouteInputToGame(InputEventType::MouseButton, false, true,  false) == true);
    EXPECT(RouteInputToGame(InputEventType::MouseWheel,  false, true,  false) == true);
    EXPECT(RouteInputToGame(InputEventType::MouseMove,   false, false, true)  == false);
    EXPECT(RouteInputToGame(InputEventType::MouseWheel,  false, false, true)  == false);
}

static void T03_editor_keyboard_follows_acceptsKeyboard()
{
    EXPECT(RouteInputToGame(InputEventType::Key,       false, false, true)  == true);
    EXPECT(RouteInputToGame(InputEventType::TextInput, false, false, true)  == true);
    EXPECT(RouteInputToGame(InputEventType::Key,       false, true,  false) == false);
    EXPECT(RouteInputToGame(InputEventType::TextInput, false, true,  false) == false);
}

int main()
{
    T00_smoke();
    T01_cursor_locked_routes_all();
    T02_editor_mouse_follows_acceptsMouse();
    T03_editor_keyboard_follows_acceptsKeyboard();

    if (g_Failures == 0) { std::printf("All input routing tests passed.\n"); return 0; }
    std::printf("%d input routing test(s) FAILED.\n", g_Failures);
    return 1;
}
