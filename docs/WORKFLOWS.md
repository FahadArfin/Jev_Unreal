# Practical workflows

All examples are MCP tool arguments. Start with `unreal_context` and confirm the
intended project. The 0.3 alpha exposes 27 tools. Local editor, discovery search,
layout/spatial recipes, snapshots, verification, asset filtering and diagnostic
grouping require no provider key. Optional Jev requests are explicit, bounded
judgments; they do not execute an operation. Replace every example actor/asset
path and state token with values from your own current editor inspection.

## Edit existing actors with fresh evidence

1. Call `unreal_context` and confirm the project, editor world and play state.
   Obtain exact actor paths with `unreal_actors` or the reported selection.
2. Call `unreal_actor_details` for 1–20 intended actors. Inspect their world bounds,
   assigned materials, live `instance_id`, `editable` flag and `edit_blockers`.
3. Call `unreal_snapshot` with the same paths to keep a before-edit baseline. Save
   its `snapshot_id` and project/session/world identity. This is selected-actor
   evidence, not a scene backup.
4. Define measurable post-edit requirements. Use `unreal_spatial_preview` for
   geometric translations, or `unreal_preview` for explicit supported operations
   with `expected_state` from the latest measurement. Review the normalized plan.
5. Apply the returned plan once, within its 120-second lifetime and the user's
   authorized scope. Inspect `applied` and immediate `verification` separately.
6. Call `unreal_verify` with the intended checks and original expected identity;
   then `unreal_diff` with the saved snapshot ID. Do not reuse the old revision
   as a post-edit requirement. An intended edit changes the revision.
7. Frame the resulting actor paths and capture the rendered viewport. Review the
   image and perform any needed gameplay/collision tests separately.

Inspection and snapshot take `{"actor_paths":["EXACT_ACTOR_PATH"]}`. Diff takes
`{"snapshot_id":"RETURNED_SNAPSHOT_ID"}`. For example, ground selected geometry
on an explicit world plane:

```json
{
  "recipe": {
    "kind": "ground",
    "actor_paths": ["/Game/Maps/Test.Test:PersistentLevel.StaticMeshActor_0"],
    "z_cm": 0
  }
}
```

`unreal_spatial_preview` obtains fresh native measurements itself. It returns
`preview.plan_id`, before/after bounds, `measurement_state`, and ready-to-use
`verification_checks`. Pass those checks unchanged to `unreal_verify` with the
original project/session/world identity. Generated checks include the measured
actor's `expected_instance_id`, so a replacement actor with the same path cannot
pass as the original. The remaining recipes are alignment, center/equal-gap
distribution, and pivot grid snapping. [Exact spatial semantics](SPATIAL_WORKFLOWS.md).

### Material and actor organization edits

Assign an existing material to an existing slot through `unreal_preview`:

```json
{
  "operations": [{
    "op": "set_material",
    "actor_path": "/Game/Maps/Test.Test:PersistentLevel.StaticMeshActor_0",
    "slot": 0,
    "material_path": "/Game/Materials/M_Cover.M_Cover"
  }],
  "expected_state": {
    "session_id": "LATEST_INSPECTION_SESSION",
    "world_path": "/Game/Maps/Test.Test",
    "revision": "LATEST_INSPECTION_REVISION"
  }
}
```

This changes the actor component's material assignment, not the material asset's
parameters or graph. Paths must identify supported existing material assets under
`/Game` or `/Engine`; the slot must exist and be 0–63.

For actor labels/folders, create a separate fresh preview:

```json
{
  "operations": [{
    "op": "set_metadata",
    "actor_path": "/Game/Maps/Test.Test:PersistentLevel.StaticMeshActor_0",
    "label": "Cover_Left",
    "folder": "Blockout/Cover"
  }],
  "expected_state": {
    "session_id": "LATEST_INSPECTION_SESSION",
    "world_path": "/Game/Maps/Test.Test",
    "revision": "LATEST_INSPECTION_REVISION"
  }
}
```

`set_metadata` accepts label, folder, or both; an empty folder places the actor in
the root folder. Labels are not actor paths: continue using returned object paths.
A plan allows at most one operation per existing actor, so material and metadata
edits to the same actor require separate reviewed plans and fresh measurements.
All existing-actor mutations require supported exact native StaticMeshActors with
no attachments or native edit blockers.

Verify material or metadata results with explicit checks, retaining the inspected
instance and project/session/world identity:

```json
{
  "checks": [{
    "kind": "material_slot",
    "actor_path": "/Game/Maps/Test.Test:PersistentLevel.StaticMeshActor_0",
    "expected_instance_id": "INSPECTED_ACTOR_INSTANCE",
    "slot": 0,
    "expected_path": "/Game/Materials/M_Cover.M_Cover"
  }],
  "expected_identity": {
    "project_file": "C:/Projects/Test/Test.uproject",
    "session_id": "ORIGINAL_EDITOR_SESSION",
    "world_path": "/Game/Maps/Test.Test"
  }
}
```

Label and folder checks use `kind: "label"` or `"folder"` with `actor_path`,
optional `expected_instance_id`, and `expected: "DESIRED_VALUE"`.
[Verification contracts](VERIFICATION.md) list transform, bounds-anchor,
bounds-size, bottom-height and minimum-gap checks and their tolerances.

### Interpret the evidence and interrupted requests

Snapshots store at most 32 records/2 MiB for 15 minutes in one MCP process. Diffs
cover the selected loaded actors and reported fields only. Missing/replaced actors,
incomplete records, changed identity, expiration or lost snapshots are
`unverifiable`. They do not prove deletion or a successful edit. Diff material-slot
additions/removals are not scene-wide actor additions/removals.

Verification returns `passed`, `failed` or `unverifiable` for each requirement.
Only all-passed checks produce an overall pass. AABBs establish measured world
bounds; they do not prove terrain contact or collision behavior. A successful
apply readback describes that transaction; a fresh verify/diff describes a later
observation. Neither establishes that a map was saved.

If apply is interrupted, query `unreal_plan` with `{"plan_id":"RETURNED_PLAN_ID"}`
and inspect the actors again. Unknown means the client cannot establish whether
the operation completed. Do not replay it. Plan records retain the last observed
status and available details, at most 64 records/2 MiB for 15 minutes; large details
may be omitted. Receipt failures do not hide a confirmed native apply result.
Local repeated-attempt suppression is separately bounded to 64 IDs/15 minutes;
native single-use plans remain authoritative. Records and snapshots disappear on
MCP restart or eviction and are not persistent recovery, Undo or auto-save.

## Build and inspect a blockout

1. Call `unreal_context` with `{"limit":30}`. Check project, play state and selection.
2. Call `unreal_layout_preview` with one of the examples below.
3. Review the normalized operations, dimensions and the 120-second plan lifetime.
4. Call `unreal_apply` with `{"plan_id":"RETURNED_PLAN_ID"}` once.
5. Check `applied` and `verification`. Verification checks native readback from
   that operation; a mismatch does not undo the already-applied change.
6. Call `unreal_validate` with the layout's label prefix as `query`.
7. Use explicit fresh `unreal_verify` checks for required dimensions/positions,
   then call `unreal_frame` with returned actor `path` values and `unreal_capture`.
   Review the actual image. Headless execution cannot provide visual acceptance.

```json
{"layout":{"kind":"grid","rows":2,"columns":3,"size_cm":[100,100,150],"gap_cm":[100,150],"label_prefix":"Cover","origin":[0,0,0],"yaw_degrees":30}}
```

```json
{"layout":{"kind":"stairs","steps":8,"rise_cm":18,"tread_depth_cm":30,"width_cm":150,"label_prefix":"Entrance","origin":[0,0,0]}}
```

```json
{"layout":{"kind":"room","inner_size_cm":[800,600,300],"wall_thickness_cm":20,"floor_thickness_cm":20,"ceiling":false,"label_prefix":"Warehouse","origin":[0,0,0]}}
```

Grid origin is its starting bottom corner. Stair treads rise along local positive X;
each tread is solid down to the base. Room origin is the interior floor corner;
walls grow outward so the requested inner dimensions stay clear. A room has no
door opening. Yaw rotates geometry around its origin. All dimensions are cm.
Recipes contain at most 20 cubes of blockout geometry. Validate navigation and
gameplay separately in the engine.

Native Undo covers supported scene changes. Framing only changes the current
editor camera. Neither operation saves project packages. No implicit retry occurs
after a timeout; inspect the current scene before deciding the next action.

## Frame and review the result

```json
{
  "actor_paths": ["/Game/Maps/Test.Test:PersistentLevel.StaticMeshActor_0"],
  "padding": 1.2,
  "view": "isometric"
}
```

`unreal_frame` accepts `view: "current"` by default, or `"isometric"`, `"top"`,
`"front"`, or `"right"`. Directional presets require a perspective level-editor
viewport; they do not switch its projection mode. Padding is 1–4. Every requested
target is checked before the camera moves, and actor selection is preserved.
Piloted/locked or unavailable viewports are rejected.

Follow with `unreal_capture` using `{"max_dimension":1024}`. The returned PNG is
the current rendered editor view, not a desktop screenshot. Framing then capture
can help compare views, but does not lock lighting/exposure or certify repeatable
rendering. NullRHI/headless execution cannot supply visual acceptance. The server
sends no image to Jev; your MCP client may use its own vision provider.

## Find and place an existing mesh

Search with `unreal_assets`, then call `unreal_asset_details` for exact paths.
Details include unscaled local bounds, material slots, LOD counts and body setup.
A body setup is not proof that collision is correct for a gameplay use case.

For a shortlist, pass known metadata to `jev_rank_assets`:

```json
{
  "goal":"wooden doorway for a warehouse",
  "candidates":[
    {"id":"door_a","path":"/Game/Props/DoorA.DoorA","description":"wooden doorway","class_name":"/Script/Engine.StaticMesh","dimensions_cm":[120,20,220],"has_collision":true},
    {"id":"crate_b","path":"/Game/Props/CrateB.CrateB","description":"metal storage crate","class_name":"/Script/Engine.StaticMesh","dimensions_cm":[100,100,100],"has_collision":true}
  ],
  "filters":{"class_names":["/Script/Engine.StaticMesh"],"max_dimensions_cm":[150,60,250],"require_collision":true},
  "use_jev":false
}
```

These example asset paths are placeholders, not included content. Required unknown
metadata fails a hard filter. Descriptions do not override measured fields. Set
`use_jev:true` only when the supplied shortlist should be sent to the provider.
Jev may choose one item or defer; it does not visually inspect a mesh.

Place a known asset through `unreal_preview`:

```json
{"operations":[{"op":"spawn_static_mesh","asset_path":"/Game/Props/DoorA.DoorA","label":"WarehouseDoor","location":[200,0,110],"rotation":[0,90,0],"scale":[1,1,1]}]}
```

The native bridge resolves the exact static mesh; Blueprints and other asset
classes cannot be instantiated through this operation. Preview is followed by
the same one-shot apply, validation and image review as primitive blockouts.

## Work through a diagnostic log

Pass a small excerpt to `jev_diagnostics` with `use_jev:false`. The result groups
identical normalized messages, reports counts and first/last line numbers, and
preserves the original issue details. Build/test exit codes remain authoritative.
If category selection is ambiguous, opt into one batched Jev call with
`use_jev:true`. Redaction is best effort; inspect confidential excerpts yourself.

Use `jev_catalog_search` to locate a relevant external inspection tool, then
`jev_catalog_get` with the returned version to retrieve its actual schema. Use
that server's existing client for authorized execution. Catalog metadata does not
establish the target editor's identity or make a tool available to the host client.

## Human-friendly command line

```powershell
uv run jev-unreal doctor
uv run jev-unreal context
uv run jev-unreal inspect "/Game/Maps/Test.Test:PersistentLevel.StaticMeshActor_0"
uv run jev-unreal verify checks.json
uv run jev-unreal layouts
uv run jev-unreal catalog status
uv run jev-unreal catalog search "inspect collision" --limit 5
uv run jev-unreal catalog get "epic:EXACT_DISCOVERED_TOOL_NAME"
```

`doctor` checks the editor connection/project binding and reports the native
capabilities missing from the inspect/edit/verify workflow, without a provider request.
Its provider status describes the current process environment, not encrypted
credentials that only the Windows MCP launcher has decrypted. An unbound or
unreachable editor or a missing required capability returns exit code 1 with
structured next steps. Rebuild/relaunch the matching native plugin when a feature
is unavailable; the bridge does not silently discard a requested state precondition.

`inspect` accepts one or more exact actor paths and reads details without editing.
`verify` reads at most 64 KiB from the explicitly supplied JSON file. Its shape is
the same as `unreal_verify` arguments, for example:

```json
{
  "checks": [{
    "kind": "bottom_z",
    "actor_path": "/Game/Maps/Test.Test:PersistentLevel.StaticMeshActor_0",
    "expected_cm": 0,
    "tolerance_cm": 0.1
  }]
}
```

Add `expected_identity` and inspected instance IDs when continuity matters; an
unbound check examines the current actor at that path. `expected_revision` is
optional and requires exact equality, so omit the pre-edit revision after an
intended change. CLI verification exits with code 0 only for `passed`, and code 1
for failed, unverifiable or invalid input. A successful check does not apply or
save anything.

Each catalog CLI command performs a fresh bounded discovery; MCP search/get reuse
the explicitly refreshed in-memory snapshot. No private catalog is persisted.

MCP clients can use the `verified_edit_workflow`, `blockout_workflow` and
`diagnostic_workflow` prompts, plus `jev://checks` and `jev://layouts` resources.
The [roadmap](ROADMAP.md) describes future review UI, installation improvements,
project-owned tests and Blender handoff; those are not additional current tools.

## Performance and acceptance

Grouping and batch recipes reduce interface round trips by design, but are not
measurements of developer productivity. Compare direct agent use, deterministic
retrieval and optional Jev assistance on held-out real tasks. Record completed-task
quality, mistakes, correction effort, total latency, tokens and cost. Include
unsupported requests and abstentions. Never optimize a demo by hiding failures.
