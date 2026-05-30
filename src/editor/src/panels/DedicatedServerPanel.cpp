#include "DedicatedServerPanel.h"
#include "ServerSupervisor.h"
#include "ServerControl.h"   // kDedicatedServerDefaultPort

#include <imgui.h>

void DrawDedicatedServerPanel(ServerSupervisor& supervisor, bool* open) {
    if (open && !*open) return;
    if (!ImGui::Begin("Dedicated Server", open)) { ImGui::End(); return; }

    static int s_Port = kDedicatedServerDefaultPort;
    ImGui::InputInt("Port", &s_Port);
    if (s_Port < 1)     s_Port = 1;
    if (s_Port > 65535) s_Port = 65535;

    const ServerStatus st = supervisor.Status();
    const bool running = (st == ServerStatus::Running);

    ImGui::BeginDisabled(running);
    if (ImGui::Button("Start", ImVec2(120, 0))) {
        supervisor.Start(static_cast<uint16_t>(s_Port));
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Stop", ImVec2(120, 0))) {
        supervisor.Stop();
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Status");
    switch (st) {
        case ServerStatus::NotStarted: ImGui::TextUnformatted("Not started"); break;
        case ServerStatus::Running:    ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "Running on port %u", (unsigned)supervisor.Port()); break;
        case ServerStatus::Stopped:    ImGui::TextUnformatted("Stopped"); break;
        case ServerStatus::Crashed:    ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "Crashed (exit 0x%08lX)", supervisor.LastExitCode()); break;
    }
    ImGui::TextDisabled("The in-editor client auto-connects to 127.0.0.1:%u (see log).",
                        (unsigned)kDedicatedServerDefaultPort);
    if (s_Port != kDedicatedServerDefaultPort) {
        // The game-side client is compiled against the fixed default port (variable-port
        // editor->client wiring is a documented Phase-3 follow-up), so a custom port binds
        // the server but the in-editor client won't reach it.
        ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1),
                           "Note: in-editor client only connects to the default port %u.",
                           (unsigned)kDedicatedServerDefaultPort);
    }

    ImGui::End();
}
