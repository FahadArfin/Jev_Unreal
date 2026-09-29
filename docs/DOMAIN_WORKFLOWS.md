# Domain workflows: inspect, preview, apply, verify

Version 0.9 expands the ten bounded domain workflows introduced in 0.8.
These workflows use local native engine APIs; no model key or provider
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
   Blueprint literal/graph plans use `unreal_blueprint_compile`; surface plans use
   `unreal_apply`. A stale, expired or failed attempt consumes its plan.
5. Inspect fresh data and the receipt. Domain receipts have `after` and
   `readback_verified`; a completed API response can contain `readback_failed`.
   Check the status, not just `ok`. Capture rendered changes after settling.

Domain plans expire after 120 seconds and receipts are retained for 15 minutes,
up to 64 per editor session. `unreal_workflow_receipt` reads an outcome after MCP
reconnection; it never retries. An interrupted apply can have changed content.
The receipt identity describes the reviewed pre-edit state. Inspect again for
current state. Asset/light edits use native Undo; viewport changes do not.
There is no save request or comprehensive callback rollback. Native plans remain
session-local. Optional [durable MCP receipts](TEAM_WORKFLOWS.md) retain historical
intent/outcomes across process interruptions, never reusable native apply plans.
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
bounded to ±1,000,000. The literal operation exposes no generated code, object
references, specialized Widget/Animation graph mutation or custom node callbacks.

The separate `unreal_blueprint_graph_preview` operation adds reviewed creation,
removal, connection and disconnection for these same native math nodes when
`bEnableGraphEdits=true`. Links require exact primitive types, an unoccupied input,
native schema approval and no cycle. Removal requires an unlinked allowed node.
See [graph identities, operations and limits](BLUEPRINT_WORKFLOWS.md#reviewed-math-nodes-and-connections).

The edit and compile occur within one native Undo transaction. A failed compiler
result retains the literal and fresh diagnostics for explicit Undo or correction;
it is never called successful compilation. Compiler extensions and reinstancing
are trusted project code and their entire side effects cannot be rolled back.
Successful compilation does not establish gameplay semantics.

## 2. Material instance parameters

Inspect: `{"kind":"material","target_path":"/Game/Materials/MI_Prop.MI_Prop"}`.
Open the exact native MaterialInstanceConstant in Unreal first. The result includes
parent identity, effective scalar/vector/texture/static-switch parameters,
association/index, resolution and truncation. Up to 128 entries are returned;
truncated parameter sets cannot be edited through this workflow.

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
state and replaced object identities fail closed. Existing layer/blend parameters
use an explicit association and index; changing the layer stack itself remains
unsupported. Texture2D references require a separate exact texture allowlist;
static switches require `bEnableStaticSwitchEdits=true`. Neither performs implicit
loading or arbitrary material graph edits. See [editing extensions](EDITING_EXTENSIONS.md)
for texture compatibility and shader-completion limits. A material affects all
its users; inspect dependencies and capture representative scenes.

## 3. Broader native mesh copies

`unreal_mesh_preview` now preserves and verifies four additional component
settings: decal reception, custom-depth rendering, stencil value (0..255), and
translucency sort priority (−32767..32767). These join the existing transform,
material-slot, collision, shadow, visibility, tags and folder state. Readback
requires the complete extended state; missing fields never become matching
defaults. The editor advertises `mesh_extended_settings`.

Copies default to single native static-mesh actors. Explicit
`preserve_attachments: true` supports a complete closed hierarchy of at most 20
native mesh actors. Every parent/child must be selected; copies attach only to
copied parents and preserve reviewed relative transforms. Sockets, external
attachments, nonuniform parent scale, scripts, custom components, simulation and
vertex paint remain unsupported. See [mesh hierarchy review](MESH_WORKFLOWS.md).

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
  "align_to_normal": true,
  "support_samples": 5,
  "trace_complex": false,
  "max_support_variation_cm": 10
}
```

Every sampled first blocking hit must belong to one of 1..32 explicit surfaces.
The default five probes cover the center and footprint corners; nine adds edge
centers, and one retains an explicit center-only query. Optional complex traces
use existing triangle collision. A nearer unapproved blocker is a refusal, never
ignored. Trace channels are `visibility` or `camera`; slopes are bounded to 60°.
Placement supports the rotated/scaled mesh bounds on the hit plane, optionally
aligns its up axis to the normal, lifts above the highest accepted sampled bump,
and rejects other WorldStatic/WorldDynamic
overlaps using a conservative oriented box. The source and selected support are
ignored by the overlap query. Existing actor scale/position limits still apply.

Review the returned measurement and ordinary native scene plan. Apply with
`unreal_apply`, inspect the actor, trace again and capture. Bounds can overestimate
concave geometry; finite probes do not prove continuous support or physics stability.
World Partition and streaming-level worlds are refused because unloaded collision
cannot be verified. See [support sampling and bounds](ADVANCED_INSPECTIONS.md).

## 5. Import and dependency diagnosis

Query `asset_diagnosis` with `target_path` and `dependency_depth` (1..3).
It reads loaded mesh bounds, up to eight render LOD triangle/vertex counts,
simple collision shape count/trace mode, bounded material-slot names, unassigned
material slots, and recorded
import basenames/hashes. Package-registry traversal is bounded to 128 edges and
128 queued packages, with truncation reported. Cycles do not cause unbounded walks.

Findings are evidence, not automatic repairs. No simple shapes may be intentional
with complex collision. An absent package-registry entry does not establish a
broken dynamic reference. This native inspection opens no source files. The
separate Blender handoff contract and host-side bundle verifier check explicitly
listed source/mesh/texture hashes and compare expected dimensions, local center
and material slots against fresh native inspection through `unreal_handoff_verify`.
Provenance/license fields are author declarations. A matching bounded contract
does not prove every DCC import option, texture interpretation or visual result.

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
reviewed plan. Use `unreal_capture` before/after. A separate `camera_render` change
controls fixed/automatic exposure, EV100, lit/unlit view mode, realtime and motion
blur, with restoration through another reviewed plan. Competing viewport overrides
are refused. These controls do not freeze world time, temporal history or shader
work; matching settings alone do not guarantee deterministic pixels. See
[repeatable viewport settings](EDITING_EXTENSIONS.md#repeatable-viewport-settings).

## 7. Navigation and project accessibility requirements

Inspect `kind: "navigation"` with exact `nav_data_path` for an existing native
Recast actor, `start`, `end`, positive three-component `projection_extent_cm`
(at most 1000 each), `required_width_cm`, and `maximum_step_cm`.

The query reports actual native endpoint projection, a complete/non-partial path,
up to 256 path points and path length. Width and step comparisons use the selected
nav-agent radius and default-resolution step height. They are configuration checks,
not a measurement of each corridor or stair. Optional `probe_geometry: true`
adds bounded ground-height samples and Pawn-channel capsule sweeps at
`probe_spacing_cm` intervals. Its `geometry_probe_complete` and `geometry_clear`
verdicts remain separate from `complete_path`. Build activity/locks are reported;
disabled editor auto-update is explicitly identified because nav data may be stale.
The inspection performs no rebuilding or pawn movement. The separate
[owned Character traversal recipe](GAMEPLAY_RECIPES.md) exercises actual movement
ticks and detects a deliberately blocking wall. Door interactions, special links,
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

Additional `engine_published_timings` report native game/render/RHI/GPU0 cycle
counter observations. Missing/zero counters are unavailable, never zero-cost
measurements. Their observation frame is not the producing frame:
`frame_aligned` remains false because these APIs omit cross-thread production
frame identities. Counters can lag or repeat. The comparison tool continues
comparing its declared ticker metric. See [measurement semantics](ADVANCED_INSPECTIONS.md#timing-measurements).

## 9. Skeleton and animation validation

Query `rig` with an exact loaded native skeletal mesh or skeleton, optional loaded
`animation_path`, and up to 64 `required_bones`. The result reports up to 512 bone
names/indices/parents, missing required bones, assigned skeleton identity, sequence
duration, exact skeleton match and stored root-motion flag. Native root-track
extraction adds 1..64 intervals (`root_motion_samples`, default 8) and the full
sequence transform while compilation is idle. `inspect_skin_weights: true`
checks up to 65,536 imported LOD0 vertices for normalization, missing influences
and invalid section bone-map references, with explicit incomplete coverage.

A mismatched skeleton may be usable through a retargeter; an enabled root-motion
flag alone does not prove useful extracted motion. Extracted transforms and valid
stored weights do not establish playback, graph behavior, notifies, root-motion
application to a character, visible deformation or retarget quality. These remain
separate runtime acceptance tasks. See [rig inspection limits](ADVANCED_INSPECTIONS.md#animation-and-weights).

## 10. Widget structure and UI diagnostics

Query `widgets` with an exact loaded native Widget Blueprint. Traverse up to 256
stored design-tree widgets and report parent/class, stored visibility/enabled
state, native button focusability, bounded text and binding presence, text-overflow
policy, stored canvas offsets and fixed-anchor sizes. Stretched-anchor margins are
not reported as actual sizes. Inspection reads fixed serialized fields even when
the designer has cached Slate widgets; it creates no runtime widgets and executes
no text bindings.

Add `runtime_instance_path` to inspect an existing on-screen instance of that
exact Widget Blueprint in active PIE. The Blueprint must directly derive from
native UserWidget. Read cached allocated/desired sizes, game viewport dimensions,
accumulated layout/application scale and current keyboard focus without creating
or ticking the instance. Removed or unrelated instances are refused.

Zero sizes/fixed anchors are review hints. They do not prove clipping or broken
focus: DPI, layout, named-slot/user-widget content, runtime bindings and localization
can change the result. Use project-owned play tests and captures at actual viewport
sizes for overflow, input and accessibility acceptance. Runtime geometry adds
evidence, but desired-size overflow hints are not a pixel verdict, accumulated
scale is not isolated DPI, and keyboard focus is not screen-reader acceptance.
The rendered fixture exercises two widget allocation sizes, not two physical
devices. See [runtime widget scope](ADVANCED_INSPECTIONS.md#runtime-widgets).

See [validation](VALIDATION.md) for mock, native, rendered and live-bridge evidence,
and [roadmap](ROADMAP.md) for remaining expansion and external acceptance.
For introductory workflows and draft French/Spanish support, see
[localization and beginner recipes](LOCALIZATION_RECIPES.md).
