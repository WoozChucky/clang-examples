#include "NavigationPanel.h"
#include "EditorContext.h"

#include <imgui.h>

#include <chrono>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "ECS.h"
#include "ECSCommands.h"
#include "ApplicationContext.h" // ctx.App->ECSCommandRing
#include "lib.h"                // SM_WARN

#include "navigation/NavMeshSystem.h"
#include "navigation/NavMesh.h"

#include "WorldManager.h"  // DEFAULT_WORLD_SNAPSHOT_PATH (path hardcode source)

namespace {
    // Push a singleton-component edit through the command ring (RenderThread ->
    // GameThread). AddComponent upserts on the singleton entity. Matches the
    // DayNightPanel flow.
    template <typename T>
    void PushSingletonEdit(const EditorContext& ctx, const ECS* world, const T& value, const char* what)
    {
        ECSCommand cmd = ECSCommand::AddComponent(world->SingletonEntity(), value);
        if (!ctx.App->ECSCommandRing.Push(cmd)) {
            SM_WARN("NavigationPanel: ECSCommandRing full, %s edit dropped", what);
        }
    }

    // Format a uint64 epoch-seconds as "YYYY-MM-DD HH:MM:SS UTC" string. Returns
    // "(invalid)" on epoch == 0 or any time-conversion error.
    std::string FormatEpochUtc(uint64_t epochSeconds) {
        if (epochSeconds == 0) return "(invalid)";
        std::time_t t = static_cast<std::time_t>(epochSeconds);
        std::tm tm{};
#ifdef _WIN32
        if (gmtime_s(&tm, &t) != 0) return "(invalid)";
#else
        if (!gmtime_r(&t, &tm)) return "(invalid)";
#endif
        char buf[64];
        if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm) == 0) {
            return "(invalid)";
        }
        return std::string(buf);
    }

    // Read just the bake-file header to extract BakeEpoch + WorldMtimeAtBakeTime.
    // Returns false on missing file, bad magic, or short read. 24-byte read.
    struct BakeHeaderInfo {
        bool     Exists = false;
        bool     ValidMagic = false;
        uint32_t Version = 0;
        uint64_t BakeEpoch = 0;
        uint64_t WorldMtimeAtBake = 0;
    };
    BakeHeaderInfo PeekBakeHeader(const std::string& bakePath) {
        BakeHeaderInfo out;
        if (!std::filesystem::exists(bakePath)) return out;
        out.Exists = true;
        std::ifstream ifs(bakePath, std::ios::binary);
        if (!ifs) return out;
        uint32_t magic = 0;
        ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (!ifs.good() || magic != 0x484D534Eu) return out;
        out.ValidMagic = true;
        ifs.read(reinterpret_cast<char*>(&out.Version), sizeof(out.Version));
        ifs.read(reinterpret_cast<char*>(&out.BakeEpoch), sizeof(out.BakeEpoch));
        ifs.read(reinterpret_cast<char*>(&out.WorldMtimeAtBake), sizeof(out.WorldMtimeAtBake));
        return out;
    }

    // Raw file_time tick count — MUST match NavMeshSystem's GetFileMtimeEpoch so the
    // freshness comparison (info.WorldMtimeAtBake == currentWorldMtime) is meaningful.
    // The engine stores this same raw-ticks value at bake time.
    uint64_t PanelFileMtimeEpoch(const std::string& path) {
        std::error_code ec;
        auto ftime = std::filesystem::last_write_time(path, ec);
        if (ec) return 0;
        return static_cast<uint64_t>(ftime.time_since_epoch().count());
    }

    // Convert a raw file_time tick count (as produced by PanelFileMtimeEpoch /
    // stored in the bake header's WorldMtimeAtBake) to epoch-seconds for display.
    uint64_t FileTimeTicksToEpochSeconds(uint64_t ticks) {
        if (ticks == 0) return 0;
        const std::filesystem::file_time_type ft{
            std::filesystem::file_time_type::duration{
                static_cast<std::filesystem::file_time_type::rep>(ticks)}};
        const auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(ft);
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                sysTime.time_since_epoch()).count());
    }
}

void DrawNavigationPanel(const EditorContext& ctx, bool* open)
{
    if (open && !*open) return;
    if (!ImGui::Begin("Navigation", open)) { ImGui::End(); return; }

    const ECS* world = ctx.World;
    if (!world) { ImGui::TextDisabled("No world"); ImGui::End(); return; }

    // --- Config ---
    NavMeshConfigComponent cfg{};
    if (const auto* cur = world->GetSingleton<NavMeshConfigComponent>()) cfg = *cur;
    NavMeshConfigComponent edited = cfg;

    ImGui::SeparatorText("Config");
    bool changed = false;
    changed |= ImGui::DragFloat("Cell Size",   &edited.CellSize,     0.01f, 0.05f, 2.0f,   "%.2f m");
    changed |= ImGui::DragFloat("Cell Height", &edited.CellHeight,   0.01f, 0.05f, 2.0f,   "%.2f m");

    // --- Classes (per-radius bakes) ---
    ImGui::SeparatorText("Classes");
    for (uint8_t i = 0; i < edited.ClassCount; ++i) {
        ImGui::PushID(i);
        ImGui::Text("Class %u", (unsigned)i);
        changed |= ImGui::DragFloat("Radius", &edited.Classes[i].AgentRadius,   0.05f, 0.05f, 5.0f, "%.2f m");
        changed |= ImGui::DragFloat("Height", &edited.Classes[i].AgentHeight,   0.05f, 0.10f, 5.0f, "%.2f m");
        changed |= ImGui::DragFloat("Climb",  &edited.Classes[i].AgentMaxClimb, 0.05f, 0.00f, 2.0f, "%.2f m");
        ImGui::PopID();
        ImGui::Separator();
    }
    if (edited.ClassCount < kMaxNavClasses && ImGui::Button("Add Class")) {
        edited.Classes[edited.ClassCount] = edited.Classes[edited.ClassCount - 1];  // seed from previous
        edited.ClassCount++;
        changed = true;
    }
    ImGui::SameLine();
    if (edited.ClassCount > 1 && ImGui::Button("Remove Last Class")) {   // invariant: keep >= 1
        edited.ClassCount--;
        changed = true;
    }

    ImGui::SeparatorText("Shared");
    changed |= ImGui::DragFloat("Max Slope",     &edited.AgentMaxSlope, 1.00f, 0.00f, 85.0f,  "%.0f deg");
    changed |= ImGui::DragFloat("Tile Size",     &edited.TileSize,      1.00f, 8.00f, 128.0f, "%.0f voxels");
    changed |= ImGui::DragInt  ("Max Obstacles", &edited.MaxObstacles,  1.0f,  0,     4096);

    if (changed) PushSingletonEdit(ctx, world, edited, "nav config");

    // --- Build / Bake ---
    ImGui::Spacing();
    if (ImGui::Button("Rebuild NavMesh", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - 4.0f, 0))) {
        ECSCommand cmd = ECSCommand::RebuildNavMesh();
        if (!ctx.App->ECSCommandRing.Push(cmd)) {
            SM_WARN("NavigationPanel: ECSCommandRing full, RebuildNavMesh dropped");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Bake to Disk", ImVec2(-1, 0))) {
        ECSCommand cmd = ECSCommand::BakeNavMesh();
        if (!ctx.App->ECSCommandRing.Push(cmd)) {
            SM_WARN("NavigationPanel: ECSCommandRing full, BakeNavMesh dropped");
        }
    }

    // --- Status ---
    ImGui::Spacing();
    ImGui::SeparatorText("Status");
    auto nm = NavMeshSystem::Instance().Current();
    if (!nm) {
        ImGui::TextUnformatted("(no navmesh built yet)");
    } else {
        const auto s = nm->GetStats();
        ImGui::Text("Tiles built: %d", s.TilesBuilt);
        ImGui::Text("Polys: %d",       s.PolyCount);
        ImGui::Text("Vert count: %d",  s.VertCount);
        ImGui::Text("Memory: %d KB",   s.MemoryKB);

        ImGui::Spacing();
        ImGui::SeparatorText("Disk bake");

        const std::string worldPath = WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH;
        const std::string bakePath = [&worldPath]() {
            if (worldPath.size() >= 5 && worldPath.compare(worldPath.size() - 5, 5, ".json") == 0) {
                return worldPath.substr(0, worldPath.size() - 5) + ".navmesh.bin";
            }
            return worldPath + ".navmesh.bin";
        }();

        const BakeHeaderInfo info = PeekBakeHeader(bakePath);
        const uint64_t currentWorldMtime = PanelFileMtimeEpoch(worldPath);

        ImGui::Text("Disk bake: %s", info.Exists ? "yes" : "no");
        if (info.Exists) {
            if (!info.ValidMagic) {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Header: invalid magic");
            } else {
                ImGui::Text("World.json mtime: %s", FormatEpochUtc(FileTimeTicksToEpochSeconds(currentWorldMtime)).c_str());
                ImGui::Text("Last baked: %s", FormatEpochUtc(info.BakeEpoch).c_str());
                const bool fresh = (info.WorldMtimeAtBake == currentWorldMtime);
                if (fresh) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Fresh? yes");
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                       "Fresh? no (stale — Rebuild will fire on next load)");
                }
            }
        }
    }

    ImGui::End();
}
