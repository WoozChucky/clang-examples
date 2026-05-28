#include "DayNightPanel.h"
#include "EditorContext.h"

#include <imgui.h>

#include <cstring>              // std::strcmp
#include "ECS.h"
#include "ECSCommands.h"
#include "ApplicationContext.h" // ctx.App->ECSCommandRing
#include "lib.h"                // SM_WARN
#include "AtmospherePresets.h"  // presets + MatchPreset

namespace {
    // Push a singleton-component edit through the command ring (RenderThread ->
    // GameThread), matching the DayNightConfig flow. Logs on ring-full.
    template <typename T>
    void PushSingletonEdit(const EditorContext& ctx, const ECS* world, const T& value, const char* what)
    {
        ECSCommand cmd = ECSCommand::AddComponent(world->SingletonEntity(), value);
        if (!ctx.App->ECSCommandRing.Push(cmd)) {
            SM_WARN("AtmospherePanel: ECSCommandRing full, %s edit dropped", what);
        }
    }
}

void DrawDayNightPanel(const EditorContext& ctx, bool* open)
{
    if (open && !*open) return;
    if (!ImGui::Begin("Atmosphere", open)) { ImGui::End(); return; }

    const ECS* world = ctx.World;
    if (!world) { ImGui::TextDisabled("No world"); ImGui::End(); return; }

    const DayNightConfigComponent* dnCur = world->GetSingleton<DayNightConfigComponent>();
    const FogComponent*            fogCur = world->GetSingleton<FogComponent>();
    const SkyComponent*            skyCur = world->GetSingleton<SkyComponent>();
    if (!dnCur) { ImGui::TextDisabled("No DayNightConfig singleton"); ImGui::End(); return; }

    DayNightConfigComponent cfg = *dnCur;

    // --- Mode ---
    {
        int mode = static_cast<int>(cfg.Mode);
        const char* modeNames[] = { "Dynamic Cycle", "Static" };
        if (ImGui::Combo("Mode", &mode, modeNames, IM_ARRAYSIZE(modeNames))) {
            cfg.Mode = static_cast<SkyMode>(mode);
            PushSingletonEdit(ctx, world, cfg, "mode");
        }
    }

    // --- Preset ---
    if (fogCur && skyCur) {
        const char* active = MatchPreset(*fogCur, *skyCur, *dnCur); // nullptr -> Custom
        const char* label  = active ? active : "Custom";
        if (ImGui::BeginCombo("Preset", label)) {
            for (std::size_t i = 0; i < kAtmospherePresetCount; ++i) {
                const AtmospherePreset& p = kAtmospherePresets[i];
                const bool selected = active && std::strcmp(active, p.Name) == 0;
                if (ImGui::Selectable(p.Name, selected)) {
                    // Stamp palette into all three singletons. Preserve the user's current
                    // Mode + static-sun angle (presets carry only palette/cycle values).
                    DayNightConfigComponent dn = p.DayNight;
                    dn.Mode                = cfg.Mode;
                    dn.StaticSunElevDeg    = cfg.StaticSunElevDeg;
                    dn.StaticSunAzimuthDeg = cfg.StaticSunAzimuthDeg;
                    dn.ShowSunDisc         = cfg.ShowSunDisc;
                    PushSingletonEdit(ctx, world, p.Fog, "preset fog");
                    PushSingletonEdit(ctx, world, p.Sky, "preset sky");
                    PushSingletonEdit(ctx, world, dn,    "preset day/night");
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    // --- Mode-specific common controls ---
    bool dnChanged = false;
    if (cfg.Mode == SkyMode::DynamicCycle) {
        dnChanged |= ImGui::SliderFloat("Cycle seconds", &cfg.CycleSeconds, 2.0f, 300.0f, "%.1f");
        dnChanged |= ImGui::SliderFloat("Day brightness", &cfg.DayBrightness, 0.0f, 1.0f, "%.2f");
        dnChanged |= ImGui::SliderFloat("Moon intensity", &cfg.MoonIntensity, 0.0f, 1.0f, "%.3f");
    } else {
        dnChanged |= ImGui::SliderFloat("Sun elevation", &cfg.StaticSunElevDeg, 0.0f, 90.0f, "%.1f deg");
        dnChanged |= ImGui::SliderFloat("Sun azimuth",   &cfg.StaticSunAzimuthDeg, 0.0f, 360.0f, "%.1f deg");
        dnChanged |= ImGui::SliderFloat("Day brightness", &cfg.DayBrightness, 0.0f, 1.0f, "%.2f");
        dnChanged |= ImGui::Checkbox("Show sun disc", &cfg.ShowSunDisc);
    }

    // --- Advanced (collapsed) ---
    if (ImGui::CollapsingHeader("Advanced")) {
        ImGui::SeparatorText("Day / Night");
        dnChanged |= ImGui::SliderFloat("Twilight width", &cfg.TwilightWidth, 0.01f, 1.0f, "%.2f");
        dnChanged |= ImGui::SliderFloat("Day ambient",    &cfg.DayAmbient, 0.0f, 0.5f, "%.3f");
        dnChanged |= ImGui::ColorEdit3("Moon color (fill)", &cfg.MoonColor.x);

        if (skyCur) {
            ImGui::SeparatorText("Sky");
            SkyComponent sky = *skyCur;
            bool sChanged = false;
            sChanged |= ImGui::Checkbox("Sky enabled", &sky.Enabled);
            sChanged |= ImGui::ColorEdit3("Day zenith",    &sky.DayZenith.x);
            sChanged |= ImGui::ColorEdit3("Day horizon",   &sky.DayHorizon.x);
            sChanged |= ImGui::ColorEdit3("Night zenith",  &sky.NightZenith.x);
            sChanged |= ImGui::ColorEdit3("Night horizon", &sky.NightHorizon.x);
            sChanged |= ImGui::ColorEdit3("Sun color",     &sky.SunColor.x);
            sChanged |= ImGui::SliderFloat("Sun radius (deg)",  &sky.SunRadiusDeg, 0.5f, 15.0f, "%.1f");
            sChanged |= ImGui::SliderFloat("Sun glow",          &sky.SunGlow, 1.0f, 512.0f, "%.0f");
            sChanged |= ImGui::ColorEdit3("Moon color",    &sky.MoonColor.x);
            sChanged |= ImGui::SliderFloat("Moon radius (deg)", &sky.MoonRadiusDeg, 0.5f, 15.0f, "%.1f");
            sChanged |= ImGui::SliderFloat("Moon glow",         &sky.MoonGlow, 1.0f, 512.0f, "%.0f");
            if (sChanged) PushSingletonEdit(ctx, world, sky, "sky");
        }

        if (fogCur) {
            ImGui::SeparatorText("Fog");
            FogComponent fog = *fogCur;
            bool fChanged = false;
            fChanged |= ImGui::Checkbox("Fog enabled", &fog.Enabled);
            fChanged |= ImGui::SliderFloat("Day density",   &fog.DayDensity,   0.0f, 0.05f, "%.4f");
            fChanged |= ImGui::SliderFloat("Night density", &fog.NightDensity, 0.0f, 0.30f, "%.3f");
            fChanged |= ImGui::ColorEdit3("Day color",   &fog.DayColor.x);
            fChanged |= ImGui::ColorEdit3("Night color", &fog.NightColor.x);
            if (fChanged) PushSingletonEdit(ctx, world, fog, "fog");
        }
    }

    if (dnChanged) PushSingletonEdit(ctx, world, cfg, "day/night");

    ImGui::End();
}
