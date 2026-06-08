# SP5 — Data-Driven Animator (Animation State Machine) Design

**Status:** Design approved, pre-implementation.
**Branch:** `feat/anim-statemachine`
**Depends on:** SP1–SP4 (skeleton import, GPU skinning, clip playback, two-clip pose blending) — all merged to `main`.

## Goal

A **generic, data-driven** animation state machine — Unity-Animator-mapped — so any entity (player, boss, prop) can have an arbitrary number of states and arbitrary transition rules, defined as **data**, not hardcoded C++. Gameplay drives it only by setting **parameters** (e.g. `speed`, bools, triggers); the engine evaluates the graph and produces the skinned pose. Fixes the SP4 walk↔run foot-slide via phase-sync.

## Why data-driven (not a C++ FSM)

A hardcoded idle/walk/run C++ FSM means every new creature's graph is a code change + rebuild. The requirement is the opposite: a boss with 4 states and a player with 8, with entirely different transition rules, authored independently. That is a data graph evaluated by a generic runtime. Gameplay still drives it — by setting parameter *values* — so it is both generic and gameplay-driven. The engine evaluator carries **zero** game-specific knowledge: states and parameters are just strings and floats.

## Architecture

Three layers, mapped to Unity's Animator:

- **Engine — generic mechanism (reusable, like the renderer):**
  - `AnimatorController` immutable asset + `AnimatorControllerStore` (mirrors `AnimationStore`/`SkeletonStore`).
  - `AnimatorComponent` engine builtin (controller handle + per-instance parameter values + runtime cursor).
  - A generic **evaluator** on the GameThread that advances time/phase, evaluates transitions against parameters, runs the chosen crossfade, samples + blends, and writes the palette.
- **Game — the gameplay-driven part:** ISystems set **parameter values** each tick (`speed = len(Velocity)`, bools, triggers) and own AI. Different creatures = different controller assets + different param-setters. **No per-graph C++.**
- **Authoring:** controllers authored as **JSON** next to the model; an inspector picker assigns a controller to an entity and shows a **live param/state debug readout**. (A visual node-graph editor is the **next** spec — SP5 ships no graph-editing UI.)

This supersedes the SP4-era assumption that the game would compute poses into a `PoseComponent`: because the evaluator is engine-side and generic, **the engine evaluator computes the pose** (the game only sets parameters). `PoseComponent` is therefore not introduced in SP5; it returns later as a procedural/IK override seam (SP6) if needed.

## Data model

All graph types live in a common header (`src/common/include/AnimatorController.h`) so the evaluator, JSON (de)serialization, and tests share them.

```cpp
enum class AnimParamType { Float, Bool, Trigger };
enum class AnimCondOp    { Greater, Less, GreaterEqual, LessEqual, Equal }; // Bool/Trigger: "is set" (value != 0)

struct AnimParam      { std::string name; AnimParamType type; };
struct AnimState      { std::string name; std::string clipKey; bool cyclic = false; bool loop = true; }; // clipKey = BARE clip name (e.g. "Walk"); loader resolves it against the owning model's assetKey -> "<assetKey>#anim/<clipKey>"
// `cyclic` = phase-sync/gait-match (dual-cursor crossfade between two cyclic states); `loop` = clip-time wrap for NON-cyclic states (default true; cyclic states always loop via Phase). Set loop:false for one-shots (e.g. Hit) so they hold the last frame.
struct AnimCondition  { std::string paramName; AnimCondOp op; float value; };
struct AnimTransition { std::string from;          // "*" = anyState
                        std::string to;
                        float duration = 0.2f;     // seconds
                        std::vector<AnimCondition> conditions; }; // ALL must hold (AND)

struct AnimatorController {
    std::string name;
    std::vector<AnimParam>      params;
    std::vector<AnimState>      states;       // index 0 = default/entry state
    std::vector<AnimTransition> transitions;  // anyState entries (from=="*") evaluated first
    // Resolved at load: clipKey -> AnimationStore handle (uint64), parallel to states.
    std::vector<uint64_t>       stateClipIds;
};
```

`AnimatorController` is immutable once stored. `clipKey` strings are resolved to `AnimationStore` handles at load (same `AssetKeyHash`/`#anim/<name>` machinery as SP3). A state whose clip fails to resolve gets handle 0 (logged `SM_WARN`; the evaluator falls back to bind pose for that state — see Error handling).

### `AnimatorComponent` (engine builtin)

```cpp
struct AnimatorComponent {
    uint64_t ControllerId = 0;                       // AnimatorControllerStore handle; 0 = none

    // Per-instance parameter values (bool/trigger stored as 0.0/1.0). Keyed by FNV-1a hash of the
    // param name so the component carries no strings. Seeded from the controller's param list when a
    // controller is assigned; gameplay writes values by name (hash) each tick.
    std::vector<std::pair<uint64_t, float>> Params;  // {nameHash, value}

    // --- runtime cursor (transient; not serialized) ---
    int   CurrentState      = -1;    // index into controller.states; -1 = uninitialized -> entry (0)
    int   FromState         = -1;    // -1 = not transitioning
    float TransitionElapsed = 0.0f;  // seconds into the active transition
    float TransitionDur     = 0.0f;  // cached duration of active transition
    bool  TransitionCyclic  = false; // both endpoints cyclic -> dual-cursor phase-sync this transition
    float Phase             = 0.0f;  // [0,1) normalized locomotion phase (drives cyclic cursors)
    float StateTime         = 0.0f;  // seconds into CurrentState's clip (non-cyclic / single-state path)
    std::vector<BonePose>  SnapshotPose; // frozen "from" pose for snapshot blends (filled at transition start)
};
```

**Serialization:** `ControllerId` + initial `Params` persist (game/world.json). All runtime-cursor fields are transient — the `to_json`/`from_json` write/read only `ControllerId` and `Params`; the rest default-construct on load. `AnimatorComponent` is engine-builtin: registered in the `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro, `ComponentSerialization.h`, `ComponentSerializers.cpp`, and both `ECSCommands.h` branches.

### `VelocityComponent` (game-owned)

```cpp
struct VelocityComponent {
    glm::vec3 Linear{0.0f};   // world units / second, post-collision (derived from actual transform delta)
    glm::vec3 PrevPos{0.0f};  // last tick's position (for the finite-difference)
    bool      Init = false;   // false until PrevPos seeded on the first tick
};
```

Game-owned (registered via the engine/game-boundary machinery: to_json/from_json + EditorUI hook; no `ecs.dll` rebuild). A dedicated `VelocitySystem` (Physics phase, registered **after** `KinematicMovementSystem`) computes `Linear = (Position - PrevPos) / dt` each tick for every entity carrying both `TransformComponent` and `VelocityComponent`, then stores `PrevPos = Position`. Deriving from the actual post-resolution transform (rather than threading a value out of the kinematic/navmesh resolver) makes it robust: stuck-against-a-wall yields zero velocity, and a stationary entity decays to zero instead of holding a stale value.

## Evaluator (engine, GameThread)

Runs in `PublishPaletteFrame` (GameThread, after `GameUpdate` so this tick's parameters are already set). For each entity with both `SkeletonComponent` and `AnimatorComponent` whose `ControllerId` resolves:

1. **Init:** if `CurrentState < 0`, set `CurrentState = 0` (entry), `Phase = StateTime = 0`.
2. **Advance:** `Phase = wrap01(Phase + dt / cycleDuration)` where `cycleDuration` = current state's clip duration (guard 0); `StateTime += dt` (clamped/looped per cyclic flag). Advance `TransitionElapsed` if transitioning.
3. **Transition selection** (only when *not* already transitioning):
   - Evaluate `anyState` transitions (`from == "*"`) first, then `CurrentState`'s outgoing, in declared order. A transition fires when **all** its conditions hold (`EvalCondition`). First match wins.
   - On fire: set `FromState = CurrentState`, `CurrentState = to`, `TransitionElapsed = 0`, `TransitionDur = duration`, `TransitionCyclic = states[from].cyclic && states[to].cyclic`. If **not** cyclic-cyclic, capture `SnapshotPose` = the pose rendered *this* frame for `FromState` (frozen). **Consume triggers** referenced by the fired transition's conditions (reset those params to 0).
4. **Pose:**
   - **Not transitioning:** sample `CurrentState`'s clip → pose (cyclic uses `Phase*duration`; else `StateTime`).
   - **Transitioning,** `w = clamp(TransitionElapsed / TransitionDur, 0, 1)`:
     - `TransitionCyclic` → **dual-cursor phase-synced**: `BlendPoses(sample(from, Phase*durFrom), sample(to, Phase*durTo), w)`.
     - else → **snapshot blend**: `BlendPoses(SnapshotPose, sample(to, toTime), w)`.
   - `w >= 1` → promote: `FromState = -1`, clear `SnapshotPose`.
5. `PoseToGlobals` → `ComputeSkinningPalette` → append palette range (existing SP2 transport, unchanged).

**Priority in `PublishPaletteFrame`:** `AnimatorComponent` (resolves) → else single-clip `AnimationComponent` (editor preview) → else `ComputeBindPoseGlobals`.

### Error handling / degradation (log, never silent — see project memory)
- `ControllerId` set but not in store → `SM_WARN` once-style message, fall back to `AnimationComponent`/bind pose.
- State clip handle 0 (unresolved) → `SM_WARN`, that state samples bind pose.
- Empty controller (no states) → bind pose.
- Parameter referenced by a condition but absent on the component → treated as 0 (no `SM_WARN` spam; documented).

## Common helpers (pure, unit-tested)

In `src/common/include/AnimatorController.h` (types) + reuse `AnimationClip.h` blend/sample:
- `float WrapPhase01(float phase)` and `float PhaseToTime(float phase, float duration)`.
- `bool EvalCondition(AnimCondOp op, float paramValue, float threshold)` (Bool/Trigger → `paramValue != 0`).
- `int SelectTransition(const AnimatorController&, int currentState, ParamLookup)` → transition index or -1 (anyState first, then outgoing, declared order, first all-conditions-true).
- Param helpers: `float GetParam(const AnimatorComponent&, uint64_t nameHash, float def=0)`, `void SetParam(AnimatorComponent&, uint64_t nameHash, float)`.

Crossfade/sample/blend compose existing `SampleClipPose` / `BlendPoses` / `PoseToGlobals`.

## JSON authoring

Controllers authored as `<model>.animctrl.json` next to the model; loaded into `AnimatorControllerStore` during the startup model directory scan (same place skeleton/clip keys are built). **`assets/models/Fox.animctrl.json` is a SP5 plan deliverable** — hand-authored and checked in as the canonical demo/smoke controller (no editor authors it in SP5; the next-spec node editor will). Example (this is the file to ship):

```json
{
  "name": "Fox",
  "params": [ { "name": "speed", "type": "Float" } ],
  "states": [
    { "name": "Idle", "clipKey": "Survey", "cyclic": false },
    { "name": "Walk", "clipKey": "Walk",   "cyclic": true  },
    { "name": "Run",  "clipKey": "Run",    "cyclic": true  }
  ],
  "transitions": [
    { "from": "Idle", "to": "Walk", "duration": 0.2, "conditions": [ { "paramName": "speed", "op": "Greater",      "value": 0.1 } ] },
    { "from": "Walk", "to": "Run",  "duration": 0.2, "conditions": [ { "paramName": "speed", "op": "Greater",      "value": 4.0 } ] },
    { "from": "Run",  "to": "Walk", "duration": 0.2, "conditions": [ { "paramName": "speed", "op": "LessEqual",    "value": 4.0 } ] },
    { "from": "Walk", "to": "Idle", "duration": 0.2, "conditions": [ { "paramName": "speed", "op": "LessEqual",    "value": 0.1 } ] }
  ]
}
```

`AnimatorController` JSON round-trips (load → in-memory → assert). Unknown JSON keys ignored (nlohmann default); missing optional fields default (`duration` 0.2, `cyclic` false). Per the early-dev policy, no migration shims.

## Editor

`AnimatorEditor` inspector panel (mirrors `AnimationEditor`/`SkeletonEditor`):
- **Controller picker** — dropdown from `AnimatorControllerStore::GetAssetList` (lazy-build the list only inside `BeginCombo`, per the SP1/SP3 per-frame-allocation lesson). Sets `AnimatorComponent.ControllerId` via the ECS command ring; on assign, seed `Params` from the controller's declared params.
- **Live debug readout** (read-only): current state name, transition target + `w`, `Phase`, and each param's value. Float params get a slider so a controller can be exercised manually without gameplay running.
- **No** state/transition editing — that is the next spec's node-graph editor.

## `AnimationComponent` cleanup

Strip the SP4 demo-scaffold blend fields from the engine-builtin `AnimationComponent`: remove `ClipB`, `TimeB`, `BlendWeight`. It returns to single-clip preview (`ClipId`, `Time`, `Speed`, `Looping`, `Playing`). `AnimationEditor` drops the Clip B dropdown, Blend Weight slider, and Time B controls. Breaking world.json change — acceptable (early-dev policy); `from_json` simply ignores the dead keys.

## Game side

- `VelocityComponent` + `VelocitySystem` (above): finite-difference of the post-resolution transform, Physics phase, after `KinematicMovementSystem`. Where collision zeroes a delta (wall), velocity reflects the *actual* zero — so the animator shows idle when stuck, not a walk-into-wall.
- `PlayerAnimParamSystem` (game ISystem, `SystemPhase::PostSimulation`, runs after Physics): for player entities with `AnimatorComponent`, `SetParam(speed, len(horizontal Velocity.Linear))` (XZ magnitude — vertical/ground-snap motion must not read as locomotion). Triggers/bools added as gameplay grows. A boss/AI param-setter is the same pattern over its own controller — no engine change.

## Testing

- **`test_animator`** (new suite, pure-fn, no World):
  - `EvalCondition` — every op; Bool/Trigger "is set".
  - `SelectTransition` — first-satisfied wins; anyState (`*`) evaluated before outgoing; declared-order tie-break; no-match → -1.
  - Trigger consume — fired transition resets its trigger params to 0.
  - Crossfade ramp — `w` from 0→1 over duration; promote at `w>=1`.
  - `PhaseToTime` / `WrapPhase01` — phase-sync mapping (`tA=phase*durA`, `tB=phase*durB`); wrap at 1.
  - Blend-mode selection — cyclic+cyclic → dual-cursor; otherwise snapshot.
  - Interrupt re-target — mid-transition condition fires a new transition; `FromState`/snapshot updated correctly.
- **`AnimatorController` JSON round-trip** test (load `Fox.animctrl.json`, assert params/states/transitions/durations).
- Existing 10 suites stay green.

## Scope

**In:** generic data-driven runtime; shared `AnimatorController` asset + JSON loader + store; `AnimatorComponent` builtin; evaluator (transition eval, anyState/triggers, snapshot + cyclic dual-cursor crossfade, phase-sync); `VelocityComponent` + `PlayerAnimParamSystem`; `AnimatorEditor` assign-picker + live debug readout; strip `AnimationComponent` blend fields + `AnimationEditor` cleanup; tests.

**Out (later SPs):** visual node-graph editor (next spec); additive/layered states (upper-body); blend-trees / 1D blendspaces; IK (SP6); root motion (SP7); render-side pose interpolation (>60 FPS smoothing); skinned shadows. A general non-locomotion param vocabulary (combat triggers, etc.) grows organically as gameplay needs it — the runtime already supports arbitrary params.

## Smoke test

1. Build (`msvc-win64-vs2026-community`), launch editor.
2. Fox entity: assign rigged Fox mesh + `SkeletonComponent`; add `AnimatorComponent`; pick the `Fox` controller (`Fox.animctrl.json`).
3. In the `AnimatorEditor` debug readout, drag the `speed` slider: `Idle → Walk → Run` crossfades fire at the thresholds; state/`w` readout matches.
4. Enter Play mode, drive the player (WASD): velocity-driven `speed` produces idle↔walk↔run crossfades; **walk↔run is phase-synced (no foot-slide)** — the SP4 regression is fixed.
5. Existing static + single-clip-preview meshes render unchanged.

## Risks / notes

- **Evaluator cost** is GameThread-side, per skinned entity, per tick — same envelope as SP3/SP4 sampling; a few entities is negligible.
- **Snapshot pose** capture must use the *exact* pose shown the frame the transition fires (so the crossfade starts seamless) — capture inside the evaluator before promoting state.
- **Phase definition** uses the current state's clip duration as the cycle length; for a cyclic→cyclic transition both cursors share `Phase`, which assumes the two locomotion clips are authored to the same gait phase convention (the standard assumption; mismatched authoring is a content issue, not a bug).
- **anyState loops:** an anyState transition whose target also satisfies the same condition could re-fire; the evaluator only selects transitions when not already transitioning and consumes triggers, which prevents trigger-driven loops. Non-trigger anyState conditions are the author's responsibility (documented).
