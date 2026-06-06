#include <cstdio>
#include <string>
#include "LoginTextEdit.h"

static int g_Failures = 0;
#define EXPECT(c) do { if(!(c)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++g_Failures; } } while(0)

static InputEvent Text(uint32_t cp) { InputEvent e(InputEventType::TextInput); e.TextEvent.Key = cp; return e; }
static InputEvent Key(KeyCode k)    { InputEvent e(InputEventType::Key); e.KeyEvent.Key = k; e.KeyEvent.Action = PRESS; return e; }

static void T01_appends_printable()
{
    std::string f;
    ApplyTextEdit(f, Text('a'));
    ApplyTextEdit(f, Text('B'));
    ApplyTextEdit(f, Text('3'));
    EXPECT(f == "aB3");
}

static void T02_ignores_non_text_events()
{
    std::string f = "x";
    ApplyTextEdit(f, Key(KEY_ENTER));   // not a TextInput -> ignored
    ApplyTextEdit(f, Text(31));         // non-printable codepoint (<32) -> ignored
    EXPECT(f == "x");
}

int main()
{
    T01_appends_printable();
    T02_ignores_non_text_events();
    if (g_Failures == 0) { std::printf("All login-edit tests passed.\n"); return 0; }
    std::printf("%d login-edit test(s) FAILED.\n", g_Failures);
    return 1;
}
