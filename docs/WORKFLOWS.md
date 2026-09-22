# Practical workflows

All examples are MCP tool arguments. Start with `unreal_context` and confirm the
intended project. Local editor, discovery search, layout, asset filtering and
diagnostic grouping tools require no provider key. Optional Jev requests are
explicit, bounded judgments; they do not execute an operation.

## Build and inspect a blockout

1. Call `unreal_context` with `{"limit":30}`. Check project, play state and selection.
2. Call `unreal_layout_preview` with one of the examples below.
3. Review the normalized operations, dimensions and the 120-second plan lifetime.
4. Call `unreal_apply` with `{"plan_id":"RETURNED_PLAN_ID"}` once.
5. Check `applied` and `verification`. Verification checks native readback from
   that operation; a mismatch does not undo the already-applied change.
6. Call `unreal_validate` with the layout's label prefix as `query`.
7. Call `unreal_frame` with the returned actor `path` values, then `unreal_capture`.
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
Recipes contain at most 20 cubes; they are blockout geometry, not a building-code,
accessibility, navigation or gameplay guarantee.

Native Undo covers supported scene changes. Framing only changes the current
editor camera. Neither operation saves project packages. No implicit retry occurs
after a timeout; inspect the current scene before deciding the next action.

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
uv run jev-unreal layouts
uv run jev-unreal catalog status
uv run jev-unreal catalog search "inspect collision" --limit 5
uv run jev-unreal catalog get "epic:EXACT_DISCOVERED_TOOL_NAME"
```

`doctor` checks the editor connection/project binding without a provider request.
Its provider status describes the current process environment, not encrypted
credentials that only the Windows MCP launcher has decrypted. An unbound or
unreachable editor returns exit code 1 with structured next steps.
Each catalog CLI command performs a fresh bounded discovery; MCP search/get reuse
the explicitly refreshed in-memory snapshot. No private catalog is persisted.

## Performance and acceptance

Grouping and batch recipes reduce interface round trips by design, but are not
measurements of developer productivity. Compare direct agent use, deterministic
retrieval and optional Jev assistance on held-out real tasks. Record completed-task
quality, mistakes, correction effort, total latency, tokens and cost. Include
unsupported requests and abstentions. Never optimize a demo by hiding failures.
