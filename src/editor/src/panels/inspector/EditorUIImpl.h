#pragma once
#include "EditorUI.h"

// The editor's ImGui-backed implementation of the EditorUI bridge. Returns a process-wide
// static table game component editor hooks call through.
const EditorUI& EditorUIInstance();
