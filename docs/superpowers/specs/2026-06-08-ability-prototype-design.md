# Combat/Ability Prototype Design

**Status:** Design approved, pre-implementation.
**Branch:** `feat/ability-prototype`
**Depends on:** SP5 data-driven animator + the node-graph editor (both merged to main). Reuses `AnimatorController`, `AnimatorComponent`, `EvaluateAnimator`, `PlayerAnimParamSystem`, the graph editor.

## Goal

A minimal combat/ability prototype that gives the animation state machine its first **non-locomotion consumer**: press LMB → the player plays a one-shot "Attack" state that auto-returns to locomotion, and movement is locked during an early window of the animation. This proves the state machine end-to-end beyond `speed`, and — crucially — establishes the **generic seam** by which gameplay drives and reads animation **without any ability/AoE/root data entering the engine**.

This is deliberately a *prototype*: it builds the reusable engine primitives (exit-time, cursor read) and one hardcoded game-side ability, structured so real abilities plug into the same seam later. It does NOT build an ability/data model, multiple abilities, or combat.

## Why this, and why now

The animator currently only transitions on `speed` (the only parameter gameplay wires). Adding IK/root-motion would widen the gap between the animation tech and the gameplay that drives it. A small ability consumer instead: (1) wires the second parameter (a Trigger) and proves the FSM beyond locomotion; (2) surfaces a real missing primitive (one-shot states need a "finished" transition — exit-time); (3) establishes how gameplay reads animation timing to make decisions (root policy) without coupling ability semantics into the engine — the pattern every future ability/IK/root-motion feature will use.

## Architecture & boundary

Two layers, sharply separated:

- **Engine — ability-agnostic generic primitives only:**
  1. **Exit-time** on transitions: a one-shot (non-cyclic) state's outgoing transition can fire when the state's playback reaches a normalized point — so the `Attack` state auto-returns to locomotion at clip end with no parameter. Reusable for every future one-shot (hit-react, dodge, cast).
  2. **Runtime-cursor read:** the game reads the animator's current state + normalized time. `AnimatorComponent` already carries `CurrentState`/`StateTime`/`Phase` (plain ECS fields the game can `GetComponent`); the engine adds only a small shared helper to compute normalized time consistently. The engine never learns what a "fireball" or "root window" is.

- **Game — owns ALL ability semantics:**
  - Reads LMB → sets an `attack` Trigger parameter (gameplay → anim).
  - Reads the animator cursor (state name resolved via the controller it authored + normalized time) → applies a **game-owned root policy** → locks player movement (anim → gameplay).
  - The root *window* (which fraction of which state roots) is game data/code. Different abilities root differently — fireball during cast only, heavy/channel for the whole state — all expressible game-side by reading `(stateName, normalizedTime)`; none of it touches the engine.

## Engine changes

### Exit-time transitions

`AnimTransition` (in `src/common/include/AnimatorController.h`) gains:
```cpp
bool  hasExitTime = false;
float exitTime    = 1.0f;   // normalized [0,1] of the FROM state's clip
```
`SelectTransition` gains a `float normalizedTime` argument (the current state's normalized progress). A transition fires when:
```
(all conditions hold)  AND  (!hasExitTime || normalizedTime >= exitTime)
```
- A transition with `hasExitTime` and **no** conditions = a pure time-based return (the `Attack → Idle` case).
- A transition with both = "fire when the param holds AND we're past exitTime."
- **anyState** transitions (`from == "*"`) ignore `exitTime` (there is no single FROM state to measure) — the evaluator does not apply exit-time to anyState transitions, and `ValidateController` warns if an anyState transition sets `hasExitTime`.

The evaluator (`EvaluateAnimator` in `GameThread.cpp`) computes the current state's normalized time and passes it to `SelectTransition`:
```
normalizedTime = (clip && clip->duration > 0)
    ? (state.cyclic ? Phase : clamp(StateTime / clip->duration, 0, 1))
    : 0
```
JSON: `to_json`/`from_json` for `AnimTransition` (de)serialize `hasExitTime`/`exitTime` with defaults (`false`/`1.0`) so existing controllers are unaffected.

### Normalized-time accessor

A small free helper so the game, the evaluator, and the editor readout share one definition:
```cpp
// In AnimatorController.h (pure) — duration supplied by caller to stay engine-free.
inline float NormalizedStateTime(const AnimState& s, float stateTime, float phase, float clipDuration) {
    if (clipDuration <= 0.0f) return 0.0f;
    return s.cyclic ? WrapPhase01(phase) : std::min(stateTime / clipDuration, 1.0f);
}
```
The game resolves the clip duration via `AnimationStore::Get(controller.stateClipIds[i])->duration`. (Pure + testable; no new engine dependency.)

### Editor

- Graph-editor selected-link inspector (`AnimatorGraphPanel`): add a `hasExitTime` checkbox + an `exitTime` slider `[0,1]` (enabled when `hasExitTime`). Shown only for non-anyState links (anyState ignores it).
- `AnimatorEditor` live readout: show the current state's normalized time alongside the existing state/phase readout.

### Tests (engine)

`test_animator` (or `test_animgraph`):
- `SelectTransition` with exit-time: does NOT fire below `exitTime`; fires at/after `exitTime`; with a condition, fires only when both hold; anyState transition with `hasExitTime` ignores it (fires on conditions regardless of normalizedTime).
- `NormalizedStateTime`: non-cyclic = `stateTime/duration` clamped to 1; cyclic = wrapped `phase`; 0-duration → 0.
- JSON round-trip of `hasExitTime`/`exitTime` + default when the keys are absent.

## Game changes

### `PlayerAbilitySystem` (Simulation phase)

Gated on `GameStateComponent.Current == InLevel`. For player entities (`PlayerComponent` + `AnimatorComponent`):
- Read `InputStateComponent.MousePressed[MOUSE_BUTTON_LEFT]`; detect the **rising edge** (held a prev-frame bool in the system, or compare against a stored last-state) so one click = one trigger.
- On the edge (and not within a short cooldown), set the `attack` Trigger parameter on the player's `AnimatorComponent` (`AssetKeyHash("attack")`, value 1.0) — the same param-write pattern as `PlayerAnimParamSystem` does for `speed`. The evaluator consumes (resets) the trigger when `anyState → Attack` fires.
- A short cooldown constant prevents spam; the evaluator's in-flight-crossfade gate already blocks mid-attack re-trigger.

(May be a new system, or merged into `PlayerAnimParamSystem` — decided in the plan; logically distinct, so likely its own system.)

### Root policy → movement lock

A game function — factored as a pure helper for testing — decides whether the player is rooted this tick:
```cpp
// Game-owned. The root WINDOW is game data/code; the engine never sees it. Prototype: one rule.
constexpr float kAttackRootEnd = 0.6f; // root for the first 60% of the Attack clip (fireball-cast-style window)
inline bool ShouldRootMovement(const std::string& stateName, float normalizedTime) {
    return stateName == "Attack" && normalizedTime < kAttackRootEnd;
}
```
A game system reads the player's animator cursor (`CurrentState` → name via the controller; `StateTime`/clip duration → normalized time via `NormalizedStateTime`), calls `ShouldRootMovement`, and records the result so the mover can honor it (e.g. a transient game-owned `MovementLockedComponent { bool Locked }`, or a single bool the player-mover reads). The comment makes explicit that real abilities carry their own per-ability windows (a future `AbilityComponent`) — whole-state root for heavy/channel, partial for cast — all by varying this game-side logic.

### `PlayerMovementSystem` lock guard

One guard: if the player is rooted this tick, skip writing `MoveIntentComponent` (no movement). Locomotion otherwise unchanged. (The animator's `speed` then reads ~0 → blends toward Idle under the attack, which is fine — the Attack state overrides via the FSM anyway.)

### `Fox.animctrl.json` (ship the update)

- Param: `{ "name": "attack", "type": "Trigger" }`.
- State: `{ "name": "Attack", "clipKey": "Survey", "cyclic": false, "loop": false }` (Survey is the placeholder — no real attack clip exists; the prototype proves the mechanism, not the pose).
- Transition `anyState → Attack`: `{ "from": "*", "to": "Attack", "duration": 0.1, "conditions": [ { "paramName": "attack", "op": "Greater", "value": 0.0 } ] }`.
- Transition `Attack → Idle`: `{ "from": "Attack", "to": "Idle", "duration": 0.15, "hasExitTime": true, "exitTime": 1.0, "conditions": [] }`.

(Authorable equivalently in the graph editor; shipping the JSON makes the prototype runnable out of the box.)

### Tests (game)

- `ShouldRootMovement`: roots in `Attack` below the window end; not after; not in other states. Pure, no World.

## Scope

**In:** exit-time transitions (data + evaluator + JSON + validation) + `NormalizedStateTime` helper; graph-editor exit-time control + normalized-time readout; `PlayerAbilitySystem` (LMB-edge → `attack` trigger + cooldown); cursor-read root policy + movement lock; `PlayerMovementSystem` lock guard; `Fox.animctrl.json` `Attack` state; pure-fn tests.

**Out (game-specific / later):**
- An `AbilityComponent` / ability data model (cast windows, AoE, damage, cooldowns, charge/channel as data).
- **Animation notify events** (named hit-frame/spawn markers at authored times the game maps to effects) — the next generic engine layer, built on the same cursor; not needed for root policy.
- Multiple abilities / skill slots / input mapping.
- Combat: damage, targets, projectiles, AoE volumes.
- Networking abilities.
- A real attack animation asset (Survey is the placeholder).

## Smoke test

1. **Prerequisite (local `world.json`, gitignored):** the Player entity must be a rigged animated entity — assign the Fox mesh + `SkeletonComponent` (Fox) + `AnimatorComponent` (Fox controller) + `VelocityComponent` + `PlayerComponent`. The code works for any such entity; the scene wiring is the user's local file.
2. Build (`msvc-win64-vs2026-community`), launch, enter play (`InLevel`).
3. WASD → walk/run locomotion still works (no regression).
4. **LMB** → the player plays the `Attack` (Survey placeholder) one-shot; **movement is locked during the early window** (`< kAttackRootEnd`), then resumes; the state **auto-returns to locomotion at clip end** via exit-time — no sticking, no parameter needed for the return.
5. Open the graph editor on the Fox controller: the `Attack` node + `anyState → Attack` + `Attack → Idle (exit-time)` render; selecting the `Attack → Idle` link shows the `hasExitTime`/`exitTime` control; the live readout shows normalized time advancing 0→1 during Attack.

## Risks / notes

- **Player must be rigged.** If the scene's Player isn't a rigged Fox, nothing animates. This is a local `world.json` setup step, documented in the smoke prereq — not a code change.
- **`Survey` placeholder** will look like the fox surveying, not attacking. Expected; the prototype proves trigger → one-shot → exit-time-return + root, not visual fidelity.
- **Exit-time semantics on anyState** are intentionally a no-op (warned by validation). Exit-time measures the FROM state's progress; anyState has no single FROM.
- **Root reads `speed`-driven blend interplay:** while rooted, `speed`→~0; but the `Attack` state is entered via the FSM and overrides locomotion regardless, so the lock + the Attack state are consistent.
- The root policy's single hardcoded window is a prototype stand-in; the design comments mark exactly where a real per-ability `AbilityComponent` window plugs in. No engine change is needed to generalize it.
