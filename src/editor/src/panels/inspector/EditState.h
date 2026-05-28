// src/editor/src/panels/inspector/EditState.h
#pragma once
#include "ECS.h"
#include "EditorContext.h"
#include "ApplicationContext.h"   // ECSCommandRing
#include "ECSCommands.h"
#include "lib.h"                  // SM_WARN

// Per-component working copy + entity-switch + dirty tracking. Encapsulates the
// scaffold the monolithic inspector repeated 14 times. GUI-thread only.
template <class T>
struct EditState {
    T        edit{};
    EntityId last = INVALID_ENTITY;
    bool     modified = false;

    // Returns the live snapshot component (nullptr if absent). On entity switch,
    // copies snapshot -> edit and clears modified. While not editing, live-refreshes
    // edit from the snapshot each frame (so game-driven mutations show up). While
    // editing (modified == true), preserves the user's in-progress edit.
    const T* Begin(const EditorContext& ctx, EntityId e) {
        const T* c = ctx.WorldSnapshot->GetComponent<T>(e);
        if (!c) return nullptr;
        if (last != e) { edit = *c; last = e; modified = false; }
        else if (!modified) { edit = *c; }
        return c;
    }

    // Pushes a ModifyComponent command iff modified, then clears the flag.
    void Commit(const EditorContext& ctx, EntityId e) {
        if (!modified) return;
        modified = false;
        if (!ctx.App->ECSCommandRing.Push(ECSCommand::ModifyComponent(e, edit)))
            SM_WARN("ECS command queue full! Modify command dropped.");
    }
};
