#include "DayNightPanel.h"
#include "EditorContext.h"

#include <imgui.h>

#include "ECS.h"
#include "ECSCommands.h"
#include "ApplicationContext.h" // ctx.App->ECSCommandRing
#include "lib.h"                // SM_WARN

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

    // --- Day / Night ---
    if (const DayNightConfigComponent* cur = world->GetSingleton<DayNightConfigComponent>()) {
        ImGui::SeparatorText("Day / Night");
        DayNightConfigComponent cfg = *cur;
        bool changed = false;
        changed |= ImGui::SliderFloat("Cycle seconds", &cfg.CycleSeconds, 2.0f, 300.0f, "%.1f");
        changed |= ImGui::SliderFloat("Day brightness", &cfg.DayBrightness, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Moon intensity", &cfg.MoonIntensity, 0.0f, 1.0f, "%.3f");
        changed |= ImGui::SliderFloat("Twilight width", &cfg.TwilightWidth, 0.01f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Day ambient", &cfg.DayAmbient, 0.0f, 0.5f, "%.3f");
        changed |= ImGui::ColorEdit3("Moon color (fill)", &cfg.MoonColor.x);
        if (changed) PushSingletonEdit(ctx, world, cfg, "day/night");
    } else {
        ImGui::TextDisabled("No DayNightConfig singleton");
    }

    // --- Fog ---
    if (const FogComponent* cur = world->GetSingleton<FogComponent>()) {
        ImGui::SeparatorText("Fog");
        FogComponent fog = *cur;
        bool changed = false;
        changed |= ImGui::Checkbox("Fog enabled", &fog.Enabled);
        changed |= ImGui::SliderFloat("Day density",   &fog.DayDensity,   0.0f, 0.05f, "%.4f");
        changed |= ImGui::SliderFloat("Night density", &fog.NightDensity, 0.0f, 0.30f, "%.3f");
        changed |= ImGui::ColorEdit3("Day color",   &fog.DayColor.x);
        changed |= ImGui::ColorEdit3("Night color", &fog.NightColor.x);
        if (changed) PushSingletonEdit(ctx, world, fog, "fog");
    } else {
        ImGui::TextDisabled("No Fog singleton");
    }

    // --- Sky ---
    if (const SkyComponent* cur = world->GetSingleton<SkyComponent>()) {
        ImGui::SeparatorText("Sky");
        SkyComponent sky = *cur;
        bool changed = false;
        changed |= ImGui::Checkbox("Sky enabled", &sky.Enabled);
        changed |= ImGui::ColorEdit3("Day zenith",    &sky.DayZenith.x);
        changed |= ImGui::ColorEdit3("Day horizon",   &sky.DayHorizon.x);
        changed |= ImGui::ColorEdit3("Night zenith",  &sky.NightZenith.x);
        changed |= ImGui::ColorEdit3("Night horizon", &sky.NightHorizon.x);
        changed |= ImGui::ColorEdit3("Sun color",     &sky.SunColor.x);
        changed |= ImGui::SliderFloat("Sun radius (deg)",  &sky.SunRadiusDeg, 0.5f, 15.0f, "%.1f");
        changed |= ImGui::SliderFloat("Sun glow",          &sky.SunGlow, 1.0f, 512.0f, "%.0f");
        changed |= ImGui::ColorEdit3("Moon color",    &sky.MoonColor.x);
        changed |= ImGui::SliderFloat("Moon radius (deg)", &sky.MoonRadiusDeg, 0.5f, 15.0f, "%.1f");
        changed |= ImGui::SliderFloat("Moon glow",         &sky.MoonGlow, 1.0f, 512.0f, "%.0f");
        if (changed) PushSingletonEdit(ctx, world, sky, "sky");
    } else {
        ImGui::TextDisabled("No Sky singleton");
    }

    ImGui::End();
}
