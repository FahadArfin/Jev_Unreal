# Replace and duplicate selected static meshes

`unreal_mesh_preview` prepares a reviewed mesh replacement or duplication from
fresh inspection of exact loaded actor paths. It works locally without Jev or an
OpenRouter key. The helper returns a native preview; call `unreal_apply` separately
after reviewing it. Nothing is automatically applied, retried or saved.

This workflow targets exact native `AStaticMeshActor` instances in editable loaded
levels; duplication additionally requires the current level. It is intended for
blockout swaps, repeated architectural pieces and copying a supported mesh actor's
declared settings. It is not a general actor
clone, Blueprint editor, mesh asset editor or import pipeline.

## Inspect, preview, apply and verify

1. Confirm the configured editor project with `unreal_status`, then inspect the
   exact actors with `unreal_actor_details`. Use returned paths and live
   `instance_id` values rather than labels. For replacement, inspect the chosen
   exact mesh asset with `unreal_asset_details`.
2. Optionally call `unreal_snapshot` for the selected actor paths. A later diff
   compares the reported source state; it does not detect every actor in a map.
3. Call `unreal_mesh_preview` with one recipe below. The helper obtains a fresh
   actor inspection and binds native preview to that session, world and revision.
4. Review the normalized operations, source identities, material policy, copied
   settings and native `preview.plan_id`. An intervening source or scene change
   invalidates the reviewed plan.
5. Apply that plan ID once. Read its apply verification and native plan receipt;
   an uncertain response is a reason to inspect, not to retry blindly.
6. Use fresh exact actor details and `unreal_verify` to check the requested result.
   Compare the source snapshot where preservation matters. Frame the exact result
   with `unreal_frame` and request `unreal_capture` for a separate visual review.

All examples below are strict JSON arguments for named MCP tools. Replace the
illustrative actor paths with paths returned by your current editor. Units are
centimeters; rotation vectors use `[pitch, yaw, roll]` in degrees.

## Replace geometry while keeping placement

Arguments for `unreal_mesh_preview`:

```json
{
  "recipe": {
    "kind": "replace",
    "actor_paths": ["/Game/Maps/Blockout.Blockout:PersistentLevel.StaticMeshActor_0"],
    "asset_path": "/Engine/BasicShapes/Sphere.Sphere",
    "material_policy": "preserve_slots"
  }
}
```

Each selected actor keeps its live identity, world transform, label, folder and
declared common settings. Its component receives the selected existing native
StaticMesh asset. The mesh asset itself is not edited. Changing geometry can
change world bounds and collision behavior even though placement is unchanged.
Inspect those effects and run project-owned gameplay checks when they matter.

When `use_mesh_default_collision` is true, replacement requires equivalent
old/new mesh default collision profiles, modes, object types and channel responses.
Incompatible or missing inherited defaults are refused during preview. An explicit
component collision configuration can be retained across differing mesh defaults;
collision geometry still follows the selected mesh. A source whose inheritance
flag disagrees with its effective configuration is refused until corrected in Unreal.

`material_policy` is required; there is no silent material choice:

| Policy | Behavior |
| --- | --- |
| `preserve_slots` | Requires matching mesh material-slot counts. Preserves each effective source material by slot index, including its current override result. It does not match slots by name or semantic purpose. |
| `mesh_defaults` | Clears component material overrides and uses the replacement mesh's current default materials. Existing custom assignments can disappear; review them first. |

An empty source slot cannot mask a nonempty replacement default with a null
override. That `preserve_slots` case is rejected; assign a material first or
explicitly choose `mesh_defaults`.

For a blockout swap that should take the destination asset's defaults:

```json
{
  "recipe": {
    "kind": "replace",
    "actor_paths": ["/Game/Maps/Blockout.Blockout:PersistentLevel.StaticMeshActor_0"],
    "asset_path": "/Game/Architecture/SM_Wall.SM_Wall",
    "material_policy": "mesh_defaults"
  }
}
```

The asset path must identify a registry-confirmed exact native `UStaticMesh` under
`/Game` or `/Engine`. Redirectors, filesystem paths, subobjects, Blueprints and
other asset classes are rejected. Resolving a selected asset may load its normal
Unreal dependencies; it is not an import or asset conversion.

## Duplicate a configured piece

Arguments for `unreal_mesh_preview`:

```json
{
  "recipe": {
    "kind": "duplicate",
    "actor_paths": ["/Game/Maps/Blockout.Blockout:PersistentLevel.StaticMeshActor_0"],
    "offset_cm": [300, 0, 0],
    "label_prefix": "",
    "label_suffix": "_Copy"
  }
}
```

The helper creates one copy per selected source. Each new location is the source's
inspected world location plus `offset_cm`; rotation and scale are preserved. The
new actor has its own object path and `instance_id`. Source actors remain unchanged.
The label is the supplied prefix, source label and supplied suffix; labels remain
subject to native validation and are not object identities.

This is bounded construction of a new native StaticMeshActor with explicitly
copied settings. It does not invoke Unreal's general actor duplication or copy
arbitrary components/properties, Blueprint logic, attachments, references to other
actors, or asset contents. Review the declared copy scope:

| Copied state | Evidence |
| --- | --- |
| Existing mesh, effective materials and bounded override references | `static_mesh_path`, `materials`, `material_slot_count`, `material_override_count` |
| Folder and the chosen resulting label | `folder`, `label` |
| Mobility and collision configuration | `mesh_settings` mobility, mesh-default collision inheritance, collision mode/profile/object type, 32 channel responses and actor collision enablement |
| Common rendering/visibility flags | `mesh_settings` cast shadow, component visibility/hidden-in-game, actor hidden-in-game/hidden-in-editor |
| Bounded actor and component tags | `mesh_settings` actor/component tag arrays |

Other editable settings must remain within the native supported defaults. Copies
ignore known cached physics values only while their explicit override is disabled;
enabled unsupported overrides still refuse the copy. The workflow
also refuses an owner, a nonzero editor pivot offset, per-instance vertex painting
or baked-lighting overrides. Collision checks inspect the reported behavior of up
to 256 shapes and refuse custom shape responses, changed shape collision modes
and mask filters. Dormant private overrides that currently match the defaults
cannot be distinguished through Unreal's public getters; the copy contract covers
the reported behavior, not those private fields. Actors with unsupported
components, simulation or other unsupported configuration fail closed instead of
producing an incomplete clone. A material or mesh reference is shared with the
source; no new asset is created.

## Direct typed operations

Clients that already inspect and construct operations can pass the same native
operations to `unreal_preview`. The higher-level recipe is convenient because it
performs fresh inspection and carries its state into the preview request.

```json
{
  "operations": [
    {
      "op": "replace_mesh",
      "actor_path": "/Game/Maps/Blockout.Blockout:PersistentLevel.StaticMeshActor_0",
      "asset_path": "/Engine/BasicShapes/Sphere.Sphere",
      "material_policy": "preserve_slots"
    }
  ],
  "expected_state": {
    "session_id": "<inspected session>",
    "world_path": "<inspected editor world>",
    "revision": "<inspected revision>"
  }
}
```

Direct duplication uses an explicit label and optional absolute transform fields.
Omitted transform fields use the inspected source values:

```json
{
  "operations": [
    {
      "op": "duplicate_mesh",
      "actor_path": "/Game/Maps/Blockout.Blockout:PersistentLevel.StaticMeshActor_0",
      "label": "Wall_Copy",
      "location": [300, 0, 100]
    }
  ],
  "expected_state": {
    "session_id": "<inspected session>",
    "world_path": "<inspected editor world>",
    "revision": "<inspected revision>"
  }
}
```

Normalized duplicate previews distinguish `source_actor_path` and
`source_instance_id` from the identity of the new actor returned by apply. Do not
mistake the source `actor_path` in an operation for the newly created actor path.

## Fresh requirement checks

Replacement previews return `verification_checks` for the preserved source
identity. Successful replacement and duplicate applies return fresh-check
arguments only when their readback matches the reviewed expectations. For
duplication those checks resolve the newly created path and instance identity;
there is no new actor identity to claim before apply.

Run those returned checks through `unreal_verify`, using the inspected
project/session/world identity. Do not require the pre-edit revision after an
intended change. Each `mesh` check compares a complete declared expectation:
`asset_path`, label/folder, effective and override material records, material slot
and override counts, and `mesh_settings`. A separate `transform_equals` check
covers location, rotation and scale. The checks are bound to
`expected_instance_id`; a replacement actor at the same path is unverifiable.

The new material records include `slot`, effective `path` and `override_path`
(null means no override). Collision configuration exposes numeric `collision_mode`
(0–5), `collision_object_type` (0–31) and 32 `collision_responses` (0–2), alongside
`collision_profile` and the Boolean `use_mesh_default_collision`. That flag
records whether collision follows the mesh asset's defaults; preserving it avoids
silently switching a copy to independently configured collision. `collision_mode`
is the raw BodyInstance mode, independent
of `actor_collision_enabled`: disabling the actor does not discard the component's
configured mode. A nonempty array of passed checks verifies those stated
requirements; it does not simulate physics or validate unreported properties.
Complete snapshots retain this mesh state so source preservation can be checked
independently after duplication. A selected-source diff can remain unchanged even
when duplication adds another actor elsewhere in the same scene.

## Limits and acceptance

Recipes accept 1–20 unique actor paths and produce at most 20 operations. A source
or existing actor can appear only once in a plan. Exact native actors, editability,
attachments, component configuration and asset identities are checked by the
editor, with a current-level requirement for duplication. Play/Simulate, an active
incompatible transaction, stale
state, replaced source objects and unsupported configuration refuse the edit.
Plans remain single-use with a 120-second execution lifetime.

Inputs reject unknown fields, malformed paths, nonfinite/coerced numbers and
transforms outside native limits. Absolute positions must stay within
±1,000,000 cm, rotation within ±36,000 degrees and positive scale within
0.001–1,000. Combined labels must fit the native 1–80-character constraint.
Material slots and overrides are limited to 64. Actor and component tags are
limited to 32 each, with at most 128 characters per tag; `tags_truncated` must be
false for a complete copy. Partial metadata cannot prove preservation.

Native preview binds selected sources, relevant copied configuration and resolved
asset references. These checks are not a general hash of every engine object or
arbitrary asset byte. A rejected or ambiguous result does not authorize a retry.
Use native history plus fresh inspection. Supported edits use one native Undo
transaction and request no map/asset save; external callback effects remain
outside the bridge's rollback guarantee.

`scripts/smoke_mesh_workflows.py` is a real official-SDK stdio acceptance script
restricted to this repository's exact `examples/JevSandbox/JevSandbox.uproject`.
It checks bridge 0.5.0 and 40 tools, creates unsaved public engine-mesh fixtures,
exercises both replacement policies and duplication, checks preview immutability,
fresh verification, source preservation, stale source changes and replay refusal,
and captures the exact targets before/after. It writes sanitized summaries and
native PNGs only under ignored `artifacts/`; it does not publish them or invoke a
provider, console, shell, engine process control or arbitrary bridge code.
Failures retain a fixed stage name and allowlisted error code, without raw response
text. A separate ignored `mesh-workflows-v0.5-diagnostics.json` also records the
local plan ID for investigation; do not publish that diagnostics file. Failed
operations are never automatically retried.

Native automated fixtures cover Undo and deeper rejected/nondefault configuration
cases that the live typed setup does not create. A script or test existing is not
evidence of a completed run. Refer to [validation evidence](VALIDATION.md) for the
actual native build, automated, live and visual checks completed for the release.
Numerical/readback checks do not establish art quality, collision simulation,
gameplay correctness or broad project compatibility.
