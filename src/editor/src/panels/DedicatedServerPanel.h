#pragma once

class ServerSupervisor;

// Dedicated Server panel: Start/Stop server.exe + show status. The supervisor is
// owned by ImGuiRenderer and passed in by reference.
void DrawDedicatedServerPanel(ServerSupervisor& supervisor, bool* open);
