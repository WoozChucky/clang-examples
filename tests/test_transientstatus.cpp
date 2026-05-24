#include <cstdio>

#include "TransientStatus.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static void T00_default_not_visible()
{
    TransientStatus s;
    EXPECT(!s.Visible(0.0));
    EXPECT(!s.Visible(100.0));
}

static void T01_visible_within_window()
{
    TransientStatus s;
    s.Set("Saved world.json", false, 10.0); // default 3 s
    EXPECT(s.Visible(10.0));
    EXPECT(s.Visible(12.9));
    EXPECT(!s.Visible(13.1));   // expired
    EXPECT(s.Text() == "Saved world.json");
    EXPECT(s.IsError() == false);
}

static void T02_error_flag_and_custom_duration()
{
    TransientStatus s;
    s.Set("Save failed", true, 5.0, 1.0); // 1 s
    EXPECT(s.Visible(5.0));
    EXPECT(!s.Visible(6.1));
    EXPECT(s.IsError() == true);
    EXPECT(s.Text() == "Save failed");
}

static void T03_reset_extends_expiry()
{
    TransientStatus s;
    s.Set("a", false, 10.0);   // expires ~13
    s.Set("b", false, 20.0);   // expires ~23
    EXPECT(s.Visible(13.5));    // "b" is active (expiry 23.0)
    EXPECT(s.Visible(22.0));    // ...second Set is active
    EXPECT(s.Text() == "b");
}

int main()
{
    T00_default_not_visible();
    T01_visible_within_window();
    T02_error_flag_and_custom_duration();
    T03_reset_extends_expiry();

    if (g_Failures == 0) { std::printf("All transient status tests passed.\n"); return 0; }
    std::printf("%d transient status test(s) FAILED.\n", g_Failures);
    return 1;
}
