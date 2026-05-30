#include "ConsolePanel.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace {
    ImVec4 LevelColor(LogLevel l) {
        switch (l) {
            case LogLevel::Warn:  return ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
            case LogLevel::Error: return ImVec4(1.0f, 0.4f,  0.4f, 1.0f);
            default:              return ImVec4(0.7f, 0.85f, 0.7f, 1.0f); // Trace
        }
    }
    bool ContainsCI(const char* hay, const char* needle) {
        if (!needle || !needle[0]) return true;
        std::string h(hay), n(needle);
        auto lower = [](std::string& s){ for (char& c : s) c = (char)std::tolower((unsigned char)c); };
        lower(h); lower(n);
        return h.find(n) != std::string::npos;
    }
}

void ConsolePanel::DrainNew() {
    LogRecord buf[256];
    size_t n;
    do {
        n = LogBus::Instance().Drain(buf, 256);
        for (size_t i = 0; i < n; ++i) {
            if (m_Lines.size() >= kScrollback) m_Lines.pop_front();
            m_Lines.push_back(buf[i]);
        }
    } while (n == 256);
}

void ConsolePanel::Draw(bool* open) {
    DrainNew();   // drain every frame even when hidden, so the ring never overflows from neglect

    if (open && !*open) return;
    if (!ImGui::Begin("Console", open)) { ImGui::End(); return; }

    ImGui::Checkbox("Trace", &m_ShowTrace); ImGui::SameLine();
    ImGui::Checkbox("Warn",  &m_ShowWarn);  ImGui::SameLine();
    ImGui::Checkbox("Error", &m_ShowError); ImGui::SameLine();
    ImGui::Checkbox("Autoscroll", &m_AutoScroll); ImGui::SameLine();
    if (ImGui::Button("Clear")) m_Lines.clear(); ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##filter", "filter...", m_Filter, sizeof(m_Filter));

    const uint64_t dropped = LogBus::Instance().DroppedCount();
    if (dropped > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1,0.5f,0.2f,1), "(%llu dropped)", (unsigned long long)dropped);
    }

    ImGui::Separator();
    ImGui::BeginChild("##log", ImVec2(0,0), false, ImGuiWindowFlags_HorizontalScrollbar);

    auto visible = [&](const LogRecord& r) {
        if (r.level == LogLevel::Trace && !m_ShowTrace) return false;
        if (r.level == LogLevel::Warn  && !m_ShowWarn)  return false;
        if (r.level == LogLevel::Error && !m_ShowError) return false;
        return ContainsCI(r.text, m_Filter);
    };

    for (const LogRecord& r : m_Lines) {
        if (!visible(r)) continue;
        ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(r.level));
        ImGui::TextUnformatted(r.text);
        ImGui::PopStyleColor();
    }

    if (m_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
}
