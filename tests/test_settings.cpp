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

// AAMode persists as renderer.aaMode (int: 0=Off, 1=FXAA, 2=SMAA). Default is FXAA(1).
// (Migration of the legacy renderer.fxaa bool is covered by ResolveAaMode in test_aamode;
//  T03 below additionally exercises it through the SettingsManager::Load path.)

// Saving aaMode=Off(0) then loading into a fresh (default-FXAA) struct yields 0.
static void T00_aamode_off_roundtrip()
{
    ApplicationSettings in;
    in.aaMode = 0;   // Off
    EXPECT(SettingsManager::Save(kTmp, in));

    ApplicationSettings out; // default aaMode == 1 (FXAA)
    EXPECT(SettingsManager::Load(kTmp, &out));
    EXPECT(out.aaMode == 0);
    std::remove(kTmp);
}

// Saving aaMode=SMAA(2) round-trips to 2 (overwriting a forced-Off caller default).
static void T01_aamode_smaa_roundtrip()
{
    ApplicationSettings in;
    in.aaMode = 2;   // SMAA
    EXPECT(SettingsManager::Save(kTmp, in));

    ApplicationSettings out;
    out.aaMode = 0; // force Off, expect Load to set it to SMAA
    EXPECT(SettingsManager::Load(kTmp, &out));
    EXPECT(out.aaMode == 2);
    std::remove(kTmp);
}

// A settings file without renderer.aaMode (and no legacy fxaa) loads as default FXAA(1).
static void T02_missing_key_defaults_fxaa()
{
    {
        nlohmann::json j;
        j["version"] = 1;
        j["renderer"]["backend"] = "directx12";
        std::ofstream(kTmp) << j.dump(2);
    }
    ApplicationSettings out;
    out.aaMode = 0; // force Off, expect Load to apply the default FXAA(1)
    EXPECT(SettingsManager::Load(kTmp, &out));
    EXPECT(out.aaMode == 1);
    std::remove(kTmp);
}

// Legacy renderer.fxaa bool migrates through Load: false => Off(0), true => FXAA(1).
static void T03_legacy_fxaa_migration()
{
    {
        nlohmann::json j;
        j["version"] = 1;
        j["renderer"]["backend"] = "directx12";
        j["renderer"]["fxaa"]    = false;   // legacy key, no aaMode
        std::ofstream(kTmp) << j.dump(2);
    }
    ApplicationSettings out;
    out.aaMode = 2; // force SMAA, expect migration to flip it to Off(0)
    EXPECT(SettingsManager::Load(kTmp, &out));
    EXPECT(out.aaMode == 0);
    std::remove(kTmp);

    {
        nlohmann::json j;
        j["version"] = 1;
        j["renderer"]["backend"] = "directx12";
        j["renderer"]["fxaa"]    = true;
        std::ofstream(kTmp) << j.dump(2);
    }
    ApplicationSettings out2;
    out2.aaMode = 0; // force Off, expect migration to flip it to FXAA(1)
    EXPECT(SettingsManager::Load(kTmp, &out2));
    EXPECT(out2.aaMode == 1);
    std::remove(kTmp);
}

int main()
{
    T00_aamode_off_roundtrip();
    T01_aamode_smaa_roundtrip();
    T02_missing_key_defaults_fxaa();
    T03_legacy_fxaa_migration();
    if (g_Failures == 0) { std::printf("All settings tests passed.\n"); return 0; }
    std::printf("%d settings test(s) FAILED.\n", g_Failures);
    return 1;
}
