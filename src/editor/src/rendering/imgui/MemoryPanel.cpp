#include "MemoryPanel.h"

#include <imgui.h>

#include <memory/AllocatorRegistry.h>
#include <memory/IAllocator.h>
#include <memory/MemoryCategory.h>

#include <ECS.h>

#include "StagingBufferPool.h"

#include <string>
#include <cstdio>

// Humanize a byte count, keeping the exact value in parens (>= 1 KB).
static std::string FormatBytes(size_t bytes)
{
    char buf[64];
    if (bytes < 1024)
        std::snprintf(buf, sizeof(buf), "%zu B", bytes);
    else if (bytes < 1024ull * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f KB (%zu)", bytes / 1024.0, bytes);
    else if (bytes < 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f MB (%zu)", bytes / (1024.0 * 1024.0), bytes);
    else
        std::snprintf(buf, sizeof(buf), "%.2f GB (%zu)", bytes / (1024.0 * 1024.0 * 1024.0), bytes);
    return buf;
}

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
                ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatBytes(s.Used).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatBytes(s.Peak).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatBytes(s.Capacity).c_str());
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
                ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatBytes(s.Used).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatBytes(s.Peak).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatBytes(s.Capacity).c_str());
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

    if (ImGui::CollapsingHeader("Staging Pool", ImGuiTreeNodeFlags_DefaultOpen)) {
        const StagingPoolStats s = GetStagingPoolStats();
        ImGui::Text("Free:       %zu", s.Free);
        ImGui::Text("In use:     %zu", s.InUse);
        ImGui::Text("Created:    %zu", s.Created);
        ImGui::Text("Reuses:     %llu", (unsigned long long)s.Reuses);
        ImGui::Text("Reserved:   %s", FormatBytes(s.ReservedBytes).c_str());
        ImGui::Text("Free bytes: %s", FormatBytes(s.FreeBytes).c_str());
    }

    if (ImGui::CollapsingHeader("ComponentArray Pool", ImGuiTreeNodeFlags_DefaultOpen)) {
        const ComponentArrayPoolStats s = GetComponentArrayPoolStats();
        ImGui::Text("Free:    %zu", s.Free);
        ImGui::Text("In use:  %zu", s.InUse);
        ImGui::Text("Created: %zu", s.Created);
        ImGui::Text("Reuses:  %llu", (unsigned long long)s.Reuses);
    }

    if (world && ImGui::CollapsingHeader("ECS Memory", ImGuiTreeNodeFlags_DefaultOpen)) {
        const EcsMemoryStats s = world->MemoryStats();
        ImGui::Text("Component used:     %s", FormatBytes(s.ComponentUsed).c_str());
        ImGui::Text("Component reserved: %s", FormatBytes(s.ComponentReserved).c_str());
        ImGui::Text("Entity used:        %s", FormatBytes(s.EntityUsed).c_str());
        ImGui::Text("Entity reserved:    %s", FormatBytes(s.EntityReserved).c_str());
        ImGui::Text("Total reserved:     %s", FormatBytes(s.ComponentReserved + s.EntityReserved).c_str());
        ImGui::Text("Arrays: %zu   Entities: %zu", s.ArrayCount, s.EntityCount);
        ImGui::TextDisabled("(buffers only; excludes map/control-block overhead)");
    }

    ImGui::End();
}
