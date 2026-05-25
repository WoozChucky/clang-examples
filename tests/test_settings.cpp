#include <cstdio>   // std::printf, std::fprintf, std::remove
#include "ApplicationContext.h"          // ApplicationSettings
#include "utilities/SettingsManager.h"   // SettingsManager::Load/Save
#include <nlohmann/json.hpp>
#include <fstream>

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static const char* kTmp = "test_settings_tmp.json";

// Saving fxaaEnabled=false then loading into a fresh (default-true) struct yields false.
static void T00_fxaa_false_roundtrip()
{
    ApplicationSettings in;
    in.fxaaEnabled = false;
    EXPECT(SettingsManager::Save(kTmp, in));

    ApplicationSettings out; // default fxaaEnabled == true
    EXPECT(SettingsManager::Load(kTmp, &out));
    EXPECT(out.fxaaEnabled == false);
    std::remove(kTmp);
}

// Saving fxaaEnabled=true round-trips to true.
static void T01_fxaa_true_roundtrip()
{
    ApplicationSettings in;
    in.fxaaEnabled = true;
    EXPECT(SettingsManager::Save(kTmp, in));

    ApplicationSettings out;
    out.fxaaEnabled = false; // force off, expect Load to flip it back on
    EXPECT(SettingsManager::Load(kTmp, &out));
    EXPECT(out.fxaaEnabled == true);
    std::remove(kTmp);
}

// A settings file without renderer.fxaa leaves the caller default (true) untouched.
static void T02_missing_key_defaults_true()
{
    {
        nlohmann::json j;
        j["version"] = 1;
        j["renderer"]["backend"] = "directx12";
        std::ofstream(kTmp) << j.dump(2);
    }
    ApplicationSettings out; // default fxaaEnabled == true
    EXPECT(SettingsManager::Load(kTmp, &out));
    EXPECT(out.fxaaEnabled == true);
    std::remove(kTmp);
}

int main()
{
    T00_fxaa_false_roundtrip();
    T01_fxaa_true_roundtrip();
    T02_missing_key_defaults_true();
    if (g_Failures == 0) { std::printf("All settings tests passed.\n"); return 0; }
    std::printf("%d settings test(s) FAILED.\n", g_Failures);
    return 1;
}
