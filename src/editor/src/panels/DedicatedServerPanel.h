#pragma once

class ServerSupervisor;
struct ApplicationContext;

// Dedicated Server panel: Start/Stop server.exe + show status. The supervisor is
// owned by ImGuiRenderer and passed in by reference. `app` (may be null) receives
// the chosen port via NetServerPort so the in-editor client retargets on Start.
void DrawDedicatedServerPanel(ServerSupervisor& supervisor, ApplicationContext* app, bool* open);
