#include "DayNightPanel.h"
#include "EditorContext.h"

#include <imgui.h>

#include "ECS.h"
#include "ECSCommands.h"
#include "ApplicationContext.h" // ctx.App->ECSCommandRing
#include "lib.h"                // SM_WARN

void DrawDayNightPanel(const EditorContext& ctx, bool* open)
{
    if (open && !*open) return;
    if (!ImGui::Begin("Day / Night", open)) { ImGui::End(); return; }

    const ECS* world = ctx.World;
    const DayNightConfigComponent* cur = world ? world->GetSingleton<DayNightConfigComponent>() : nullptr;
    if (!cur) { ImGui::TextDisabled("No DayNightConfig singleton"); ImGui::End(); return; }

    DayNightConfigComponent cfg = *cur; // edit a local copy
    bool changed = false;
    changed |= ImGui::SliderFloat("Cycle seconds", &cfg.CycleSeconds, 2.0f, 300.0f, "%.1f");
    changed |= ImGui::SliderFloat("Day brightness", &cfg.DayBrightness, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Moon intensity", &cfg.MoonIntensity, 0.0f, 1.0f, "%.3f");
    changed |= ImGui::SliderFloat("Twilight width", &cfg.TwilightWidth, 0.01f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Day ambient", &cfg.DayAmbient, 0.0f, 0.5f, "%.3f");
    changed |= ImGui::ColorEdit3("Moon color", &cfg.MoonColor.x);

    if (changed) {
        ECSCommand cmd = ECSCommand::AddComponent(world->SingletonEntity(), cfg);
        if (!ctx.App->ECSCommandRing.Push(cmd)) {
            SM_WARN("DayNightPanel: ECSCommandRing full, edit dropped");
        }
    }
    ImGui::End();
}
