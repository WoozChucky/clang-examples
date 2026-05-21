#include "MemoryPanel.h"

#include <imgui.h>

#include <memory/AllocatorRegistry.h>
#include <memory/IAllocator.h>
#include <memory/MemoryCategory.h>

#include <ECS.h>

void DrawMemoryPanel(bool* open, const ECS* world)
{
    if (open && !*open) return;
    if (!ImGui::Begin("Memory", open)) { ImGui::End(); return; }

    auto& reg = Engine::Registry();

    // Per-category rollups.
    if (ImGui::CollapsingHeader("By Category", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("mem_cat", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("Used");
            ImGui::TableSetupColumn("Peak");
            ImGui::TableSetupColumn("Capacity");
            ImGui::TableHeadersRow();
            for (uint8_t i = 0; i < (uint8_t)Engine::MemCategory::Count; ++i) {
                auto cat = (Engine::MemCategory)i;
                Engine::AllocatorStats s = reg.SumByCategory(cat);
                if (s.Capacity == 0 && s.Used == 0) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(Engine::ToString(cat));
                ImGui::TableNextColumn(); ImGui::Text("%zu", s.Used);
                ImGui::TableNextColumn(); ImGui::Text("%zu", s.Peak);
                ImGui::TableNextColumn(); ImGui::Text("%zu", s.Capacity);
            }
            ImGui::EndTable();
        }
    }

    // Per-allocator detail.
    if (ImGui::CollapsingHeader("Allocators", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("mem_alloc", 6,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("Used");
            ImGui::TableSetupColumn("Peak");
            ImGui::TableSetupColumn("Capacity");
            ImGui::TableSetupColumn("Alloc/Free");
            ImGui::TableHeadersRow();
            reg.ForEach([](Engine::IAllocator* a) {
                const Engine::AllocatorStats& s = a->Stats();
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(a->Name() ? a->Name() : "<unnamed>");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(Engine::ToString(a->Category()));
                ImGui::TableNextColumn(); ImGui::Text("%zu", s.Used);
                ImGui::TableNextColumn(); ImGui::Text("%zu", s.Peak);
                ImGui::TableNextColumn(); ImGui::Text("%zu", s.Capacity);
                ImGui::TableNextColumn(); ImGui::Text("%llu/%llu",
                    (unsigned long long)s.AllocCount, (unsigned long long)s.FreeCount);
            });
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Snapshot Pool", ImGuiTreeNodeFlags_DefaultOpen)) {
        const SnapshotPoolStats s = GetSnapshotPoolStats();
        ImGui::Text("Free:    %zu", s.Free);
        ImGui::Text("In use:  %zu", s.InUse);
        ImGui::Text("Created: %zu", s.Created);
        ImGui::Text("Reuses:  %llu", (unsigned long long)s.Reuses);
    }

    if (world && ImGui::CollapsingHeader("ECS Memory", ImGuiTreeNodeFlags_DefaultOpen)) {
        const EcsMemoryStats s = world->MemoryStats();
        ImGui::Text("Component used:     %zu", s.ComponentUsed);
        ImGui::Text("Component reserved: %zu", s.ComponentReserved);
        ImGui::Text("Entity used:        %zu", s.EntityUsed);
        ImGui::Text("Entity reserved:    %zu", s.EntityReserved);
        ImGui::Text("Total reserved:     %zu", s.ComponentReserved + s.EntityReserved);
        ImGui::Text("Arrays: %zu   Entities: %zu", s.ArrayCount, s.EntityCount);
        ImGui::TextDisabled("(buffers only; excludes map/control-block overhead)");
    }

    ImGui::End();
}
