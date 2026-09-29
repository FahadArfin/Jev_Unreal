# Reviewed gameplay workflows

Jev can start and stop one explicitly approved standalone Play In Editor (PIE)
session, capture its actual game viewport, and use the existing named functional
tests to evaluate gameplay. Startup is not gameplay acceptance; a screenshot is
not a passing test. These tools execute trusted project lifecycle callbacks.

Configure the project locally in `Config/DefaultGame.ini`:

```ini
[JevEditor.RuntimeGameplay]
bEnabled=true
+Maps=/Game/Maps/MyTestMap
```

The exact current editor map package must appear in `Maps`. There is no remote
map loading, URL argument, console command, input injection, multiplayer launch,
or packaged-game control. Discovery is safe while disabled. Starting is refused
if any session is running or queued. Online subsystem login is disabled for the
requested one-client, in-process session. Existing editor play preferences are
copied into a private settings object rather than overwritten.

1. Inspect `unreal_status` and `unreal_runtime_status`. Review the current project
   and map identity and ensure the project approves its lifecycle callbacks.
2. Request `unreal_runtime_preview(operation="start", expected_state=...)`.
3. Review the returned plan, then call `unreal_runtime_apply(plan_id=...)` once.
4. Read `unreal_runtime_receipt` until `running`, an error, or `uncertain`. Retain
   its `owned_session_id`. Polling does not restart execution.
5. Use `unreal_functional_tests`, `unreal_functional_start`, and
   `unreal_functional_job` for approved project-owned gameplay assertions.
6. Call `unreal_runtime_capture(owned_session_id=..., max_dimension=1024)` for a
   bounded PNG from that session's game viewport. Record the returned world time
   and PIE world path alongside functional-test results.
7. Inspect current editor state, preview `stop` with the owned session ID, apply
   once, and read its receipt until `stopped`.

Plans expire after 120 seconds and receipts after 15 minutes. A failed or stale
apply consumes its plan. After a transport timeout, read its existing receipt;
never retry execution blindly. MCP reconnection preserves native ownership and
receipts while the editor service remains loaded. A different or user-started
session is never adopted. Stopping the owned session remains available if local
start permission is revoked. Plugin unloading does not automatically stop PIE.
Callbacks cannot be forcibly interrupted and may have side effects or save
through their own code. A transition reported `uncertain` needs human inspection.

## Trigger opens a door

The additional Blueprint vocabulary is disabled unless both graph edits and an
exact target alias are approved:

```ini
[JevEditor.BlueprintCompilation]
bEnabled=true
bEnableGraphEdits=true
bEnablePinEdits=true
bEnableGameplayGraphEdits=true
+Targets=door|/Game/Doors/BP_Door.BP_Door
+GameplayTargets=door
```

Use `unreal_blueprint_graph_preview` and the existing one-shot
`unreal_blueprint_compile` workflow for each edit. The vocabulary includes:

- `add_event`: `ReceiveBeginPlay` or `ReceiveActorBeginOverlap`, with no duplicate
  event allowed. Use the inspected event when it already exists.
- `add_branch`, and exact primitive/exec-pin `connect`/`disconnect` operations.
- `add_variable`: a new local `bool`, `int`, or `double` member, plus
  `add_variable_get` and `add_variable_set` for those local members.
- `add_actor_call`: exactly `K2_SetActorRelativeLocation`,
  `SetActorEnableCollision`, or `SetActorHiddenInGame`, operating on self.
  Relative location requires three bounded centimetre values in `location`.

No arbitrary function names, object links, casts, arbitrary node classes,
timelines, variable deletion, inherited member editing, implicit type conversion,
automatic link replacement, or graph cycles are permitted. Only existing
ordinary Actor-derived Blueprints' event graphs support gameplay additions.
Primitive unlinked defaults can be changed through the approved pin preview.
Compilation does not automatically save or imply runtime acceptance.

An overlapping door uses an existing movable root component with overlap enabled:
`ReceiveActorBeginOverlap → Branch(CanOpen) → SetActorRelativeLocation(0,0,300)`.
Inspect GUIDs after each compile, connect only the indicated pins, and leave the
false branch unconnected. A reviewed project functional test should move a probe
into the trigger, assert the permitted door rises, and assert a locked door does
not move. Jev does not configure arbitrary collision geometry through graph edits.

The source-only `Jev.Editor.RuntimeDoorWorkflow` automation fixture builds this
graph with the same graph-edit implementation, compiles it, drives real overlaps
in owned PIE, and checks positive and negative behavior. It also exercises policy,
stale-state, one-shot plans and refusal to adopt another service's PIE session.
See `VALIDATION.md` for the measured run status; presence of a fixture is not proof
that a particular engine build or visual result passed.
