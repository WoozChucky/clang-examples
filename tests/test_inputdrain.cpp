#include <cstdio>
#include <vector>
#include "InputDrain.h"

// SM_ASSERT (pulled in transitively via lib.h) references platform_debug_break, which lives
// in Engine. This is a host-free header test, so provide a stub like the sibling ECS tests.
void platform_debug_break(const char* expr, const char* file, int line, const char* message)
{
    std::fprintf(stderr, "ASSERT FAIL %s:%d: %s (expr: %s)\n",
                 (file ? file : "<unknown>"), line, (message ? message : ""), (expr ? expr : ""));
}

static int g_Failures = 0;
#define EXPECT(c) do { if(!(c)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++g_Failures; } } while(0)

static InputEvent MakeKey(KeyCode k, InputAction a)   { InputEvent e(InputEventType::Key);         e.KeyEvent.Key = k; e.KeyEvent.Action = a; return e; }
static InputEvent MakeMove(double x, double y)        { InputEvent e(InputEventType::MouseMove);   e.MouseMoveEvent.X = x; e.MouseMoveEvent.Y = y; return e; }
static InputEvent MakeButton(Button b, InputAction a) { InputEvent e(InputEventType::MouseButton); e.MouseButtonEvent.Button = b; e.MouseButtonEvent.Action = a; return e; }
static InputEvent MakeText(uint32_t cp)               { InputEvent e(InputEventType::TextInput);   e.TextEvent.Key = cp; return e; }

// Digests key/mouse into InputStateComponent AND preserves every event (incl TextInput) in order.
static void T01_digest_and_raw_incl_text()
{
    SpscRing<InputEvent, 16> ring;
    InputStateComponent s;
    s.MouseX = 5.0; s.MouseY = 7.0; // previous position, for delta computation

    EXPECT(ring.Push(MakeKey(KEY_A, PRESS)));
    EXPECT(ring.Push(MakeMove(20.0, 30.0)));
    EXPECT(ring.Push(MakeText('x')));
    EXPECT(ring.Push(MakeButton(MOUSE_BUTTON_LEFT, PRESS)));

    std::vector<InputEvent> frame;
    DrainInput(ring, s, frame);

    EXPECT(s.KeysDown[KEY_A]);
    EXPECT(s.Pressed[KEY_A]);
    EXPECT(s.MouseX == 20.0 && s.MouseY == 30.0);
    EXPECT(s.MouseDX == 15.0 && s.MouseDY == 23.0);
    EXPECT(s.MouseDown[MOUSE_BUTTON_LEFT]);
    EXPECT(s.MousePressed[MOUSE_BUTTON_LEFT]);

    EXPECT(frame.size() == 4);
    EXPECT(frame[0].Type == InputEventType::Key && frame[0].KeyEvent.Key == KEY_A);
    EXPECT(frame[1].Type == InputEventType::MouseMove);
    EXPECT(frame[2].Type == InputEventType::TextInput && frame[2].TextEvent.Key == 'x');
    EXPECT(frame[3].Type == InputEventType::MouseButton);
}

static void T02_clears_per_tick_and_empties_frame()
{
    SpscRing<InputEvent, 16> ring;
    InputStateComponent s;
    s.Pressed[KEY_A] = true;
    s.MousePressed[MOUSE_BUTTON_LEFT] = true;
    s.Wheel = 5;
    std::vector<InputEvent> frame;
    frame.push_back(MakeText('z')); // stale leftover from a prior tick

    DrainInput(ring, s, frame); // empty ring

    EXPECT(!s.Pressed[KEY_A]);
    EXPECT(!s.MousePressed[MOUSE_BUTTON_LEFT]);
    EXPECT(s.Wheel == 0);
    EXPECT(frame.empty());
}

int main()
{
    T01_digest_and_raw_incl_text();
    T02_clears_per_tick_and_empties_frame();
    if (g_Failures == 0) { std::printf("All input-drain tests passed.\n"); return 0; }
    std::printf("%d input-drain test(s) FAILED.\n", g_Failures);
    return 1;
}
