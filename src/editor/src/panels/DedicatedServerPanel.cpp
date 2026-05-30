#include "DedicatedServerPanel.h"
#include "ServerSupervisor.h"
#include "ServerControl.h"        // kDedicatedServerDefaultPort
#include "ApplicationContext.h"   // NetServerPort atomic (editor client target)

#include <imgui.h>

void DrawDedicatedServerPanel(ServerSupervisor& supervisor, ApplicationContext* app, bool* open) {
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
        const uint16_t port = static_cast<uint16_t>(s_Port);
        // Point the in-editor client at the same port BEFORE launching the server, so the
        // GameThread's NetDemoSystem retargets (a port change resets its connect state).
        if (app) app->NetServerPort.store(port, std::memory_order_relaxed);
        supervisor.Start(port);
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
    ImGui::TextDisabled("The in-editor client auto-connects to 127.0.0.1:%d (see log).", s_Port);

    ImGui::End();
}
