# Domain workflows: inspect, preview, apply, verify

Version 0.8 adds ten bounded workflows from the roadmap. The MCP catalog has 54
tools. These workflows use local native engine APIs; no model key or provider
request is needed. They are an initial supported scope, not complete automation
of every Blueprint, material, terrain, animation or UI task.

## Shared workflow

1. Read `unreal_status`/`unreal_context` and verify the exact project and world.
2. Use `unreal_workflow_inspect` with one typed `query`. Results include the
   inspected `session_id`, `world_path` and `revision` plus domain evidence limits.
3. Use those three state fields in `unreal_workflow_preview`, or the dedicated
   Blueprint/surface preview. Review the exact target, before values and requested
   change. Project policy and user authorization remain separate from Jev confidence.
4. Apply the matching plan once. General domain edits use `unreal_workflow_apply`;
   Blueprint literal plans use `unreal_blueprint_compile`; surface plans use
   `unreal_apply`. A stale, expired or failed attempt consumes its plan.
5. Inspect fresh data and the receipt. Domain receipts have `after` and
   `readback_verified`; a completed API response can contain `readback_failed`.
   Check the status, not just `ok`. Capture rendered changes after settling.

Domain plans expire after 120 seconds and receipts are retained for 15 minutes,
up to 64 per editor session. `unreal_workflow_receipt` reads an outcome after MCP
reconnection; it never retries. An interrupted apply can have changed content.
The receipt identity describes the reviewed pre-edit state. Inspect again for
current state. Asset/light edits use native Undo; viewport changes do not.
There is no save request, crash persistence or comprehensive callback rollback.
The existing Slate scene-review panel currently displays scene plans, including
surface placement; domain and Blueprint plans are reviewed through MCP responses.

## 1. Reviewed Blueprint literal edits

Configure the exact target locally in `Config/DefaultGame.ini`:

```ini
[JevEditor.BlueprintCompilation]
bEnabled=true
bEnablePinEdits=true
+Targets=door|/Game/Blueprints/BP_Door.BP_Door
```

`bEnablePinEdits` is separate and disabled by default. `unreal_blueprint_inspect`
provides graph/node/pin identities. Pass `target_id`, `expected_state` and
`pin_edit: {node_id, pin_id, value}` to `unreal_blueprint_pin_preview`.
`value` is a primitive literal string, not an expression. Commit its returned
plan using `unreal_blueprint_compile`; inspect its retained compile receipt.

Supported nodes are exact native `UK2Node_CallFunction` calls to
`KismetMathLibrary.Add_IntInt`, `Multiply_IntInt`, `Add_DoubleDouble`,
`Multiply_DoubleDouble` and `Not_PreBool`. Only unconnected input bool/int/real
pins in an ordinary native Blueprint are editable. Container/reference/orphaned,
split, read-only, ignored and connected pins are refused. Numeric values are
bounded to ±1,000,000. No node creation/deletion, connection editing, generated
code, object references, Widget/Animation graph mutation or custom node callbacks
are exposed by this edit operation.

The edit and compile occur within one native Undo transaction. A failed compiler
result retains the literal and fresh diagnostics for explicit Undo or correction;
it is never called successful compilation. Compiler extensions and reinstancing
are trusted project code and their entire side effects cannot be rolled back.
Successful compilation does not establish gameplay semantics.

## 2. Material instance parameters

Inspect: `{"kind":"material","target_path":"/Game/Materials/MI_Prop.MI_Prop"}`.
Open the exact native MaterialInstanceConstant in Unreal first. The result includes
parent identity, effective scalar/vector parameters, association/index, resolution
and truncation. Up to 128 entries are returned; only global parameters are editable.

Explicit local edit policy:

```ini
[JevEditor.Workflows]
bEnableMaterialEdits=true
+EditableMaterials=/Game/Materials/MI_Prop.MI_Prop
```

Preview a `change` such as:

```json
{
  "kind": "material_scalar",
  "target_path": "/Game/Materials/MI_Prop.MI_Prop",
  "parameter": "Roughness",
  "value": 0.6
}
```

`material_vector` uses four RGBA values in 0..16. Scalar values are bounded to
±1,000,000. The name/type must already exist in the parent parameter set. The
allowlist permits at most 64 unique exact `/Game/` asset paths and is checked
again at commit. Unknown names, inherited project-policy changes, stale asset
state and replaced object identities fail closed. No static switches, material
layers, texture replacement, graph edits or implicit loading. A material affects
all its users; inspect dependencies and capture representative scenes.

## 3. Broader native mesh copies

`unreal_mesh_preview` now preserves and verifies four additional component
settings: decal reception, custom-depth rendering, stencil value (0..255), and
translucency sort priority (−32767..32767). These join the existing transform,
material-slot, collision, shadow, visibility, tags and folder state. Readback
requires the complete extended state; missing fields never become matching
defaults. The editor advertises `mesh_extended_settings`.

Copies remain single native static-mesh actors. Attached hierarchies, scripts,
custom components, simulation, vertex paint and other nondefault settings remain
explicitly unsupported. The extension does not silently flatten a hierarchy.

## 4. Terrain-aware placement

`unreal_surface_preview` accepts this typed `query`:

```json
{
  "kind": "surface",
  "actor_path": "/Game/Map.Map:PersistentLevel.Prop",
  "surface_paths": ["/Game/Map.Map:PersistentLevel.Floor"],
  "trace_channel": "visibility",
  "trace_up_cm": 1000,
  "trace_down_cm": 10000,
  "max_slope_degrees": 30,
  "clearance_cm": 1,
  "align_to_normal": true
}
```

The first blocking hit of a simple downward collision trace must belong to one
of 1..32 explicit surfaces. A nearer unapproved blocker is a refusal, never
ignored. Trace channels are `visibility` or `camera`; slopes are bounded to 60°.
Placement supports the rotated/scaled mesh bounds on the hit plane, optionally
aligns its up axis to the normal, and rejects other WorldStatic/WorldDynamic
overlaps using a conservative oriented box. The source and selected support are
ignored by the overlap query. Existing actor scale/position limits still apply.

Review the returned measurement and ordinary native scene plan. Apply with
`unreal_apply`, inspect the actor, trace again and capture. Bounds can overestimate
concave geometry and a single trace does not prove support across an entire prop.
No collision generation, streamed-world loading or physics simulation is implied.

## 5. Import and dependency diagnosis

Query `asset_diagnosis` with `target_path` and `dependency_depth` (1..3).
It reads loaded mesh bounds, up to eight render LOD triangle/vertex counts,
simple collision shape count/trace mode, unassigned material slots, and recorded
import basenames/hashes. Package-registry traversal is bounded to 128 edges and
128 queued packages, with truncation reported. Cycles do not cause unbounded walks.

Findings are evidence, not automatic repairs. No simple shapes may be intentional
with complex collision. An absent package-registry entry does not establish a
broken dynamic reference. No source files are opened: source availability, source
units, texture completeness, pivot intent and round-trip correctness remain unknown.

## 6. Lights and repeatable views

Inspect a native Point/Spot/Rect/Directional actor with `kind: "light"` and
`target_path`. Local lights report their actual intensity-unit enum and attenuation
radius. Preview `kind: "light"`, the exact path, intensity (0..1,000,000
in that light's native units), and `color_rgb` (three linear channels in 0..1).
The actor must be editable in the current level. Readback accounts for Unreal's
stored color quantization. Baking, exposure and performance acceptance are separate.

Inspect `{"kind":"camera"}` to retain a viewport pose. Preview a `camera`
change with `location`, `[pitch,yaw,roll]` rotation and `fov_degrees` (5..170).
It requires an unlocked perspective level viewport, rejects a changed viewport
or moved camera, and can restore a previously inspected pose through another
reviewed plan. Use `unreal_capture` before/after. Exposure, time, temporal history,
view mode and render settings are not frozen; matching poses alone do not make a
controlled lighting experiment.

## 7. Navigation and project accessibility requirements

Inspect `kind: "navigation"` with exact `nav_data_path` for an existing native
Recast actor, `start`, `end`, positive three-component `projection_extent_cm`
(at most 1000 each), `required_width_cm`, and `maximum_step_cm`.

The query reports actual native endpoint projection, a complete/non-partial path,
up to 256 path points and path length. Width and step comparisons use the selected
nav-agent radius and default-resolution step height. They are configuration checks,
not a measurement of each corridor or stair. Build activity/locks are reported;
disabled editor auto-update is explicitly identified because nav data may be stale.
No rebuilding or pawn movement is performed. Door interactions, special links,
dynamic obstacles, disabilities and the project's real controller require gameplay
acceptance. The implementation follows Epic's [native synchronous path API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/NavigationSystem/UNavigationSystemV1/FindPathSync).

## 8. Editor performance investigations

Use `unreal_performance_start(protocol_id, expected_state, sample_count)` after
settling the editor. A capture records 10..600 intervals between game-thread ticker
visits, excludes the partial first interval, and expires after 60 seconds. It also
records whole-process physical memory before/after. One capture is active at a
time; up to 16 receipts are retained for 15 minutes.

Poll `unreal_performance_job`; cancellation, timeout and scene changes retain an
explicit incomplete outcome. `unreal_performance_compare` fetches two distinct
complete receipts and requires the same project, session, world, protocol, metric
and sample count. It reports mean/median/p95 changes and a user-selected regression
threshold. It does not establish statistical significance from one pair.

Record viewport pose/size, realtime/throttle/vsync, polling rate, asset compilation,
hardware and workload in the named protocol. The ID is an operator declaration,
not verification that these conditions matched. Ticker intervals include idle,
UI and background work; they are **not** GPU/render execution cost, packaged FPS,
per-asset cost or proof of a bottleneck. Use Unreal Insights for attribution.

## 9. Skeleton and animation validation

Query `rig` with an exact loaded native skeletal mesh or skeleton, optional loaded
`animation_path`, and up to 64 `required_bones`. The result reports up to 512 bone
names/indices/parents, missing required bones, assigned skeleton identity, sequence
duration, exact skeleton match and stored root-motion flag.

A mismatched skeleton may be usable through a retargeter; an enabled root-motion
flag does not prove useful extracted motion. No retargeting, reimport, playback,
skin-weight validation or DCC file access occurs. Those remain acceptance work.

## 10. Widget structure and UI diagnostics

Query `widgets` with an exact loaded native Widget Blueprint. Traverse up to 256
stored design-tree widgets and report parent/class, stored visibility/enabled
state, native button focusability, bounded text and binding presence, text-overflow
policy, stored canvas offsets and fixed-anchor sizes. Stretched-anchor margins are
not reported as actual sizes. Inspection reads fixed serialized fields even when
the designer has cached Slate widgets; it creates no runtime widgets and executes
no text bindings.

Zero sizes/fixed anchors are review hints. They do not prove clipping or broken
focus: DPI, layout, named-slot/user-widget content, runtime bindings and localization
can change the result. Use project-owned play tests and captures at actual viewport
sizes for overflow, input and accessibility acceptance.

See [validation](VALIDATION.md) for mock, native, rendered and live-bridge evidence,
and [roadmap](ROADMAP.md) for remaining expansion and external acceptance.
