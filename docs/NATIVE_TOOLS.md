# Native editor inspection (bridge 0.2.0)

These actions use the existing authenticated loopback bridge. They execute on the
editor game thread and do not call a model provider. Requests remain JSON objects
with `action` and `params`; responses retain the `ok` / `result` or `error` envelope.
The MCP client still checks the exact project identity before every operation.

| Action | Parameters | Result |
| --- | --- | --- |
| `context` | Optional `query` string, at most 200 characters; optional integer `limit` 1–200, default 50. | Project, session, engine, editor world, current level, revision, PIE/simulation flags, actor snapshots, selected actor paths, and dirty package names. Each collection has a truncation flag. |
| `asset_details` | Required `path`: exact `/Game/...Asset.Asset` or `/Engine/...Asset.Asset` object path, at most 512 characters. | Registry identity; static meshes additionally report bounds in centimeters, material slots, up to 16 LOD vertex/triangle counts, and collision configuration. |
| `validate` | Optional `query` string, at most 200 characters; optional integer `limit` 1–200, default 100. | Deterministic warnings with actor/component paths, scan counts, revision, and completeness flags. |
| `capture` | Optional integer `max_dimension` 64–1024, default 1024. | Base64 PNG in `data`, `mime_type`, width, height, world, revision, and `source: "editor_viewport"`. |
| `frame` | Required `actor_paths`: 1–20 unique exact actor paths, each at most 1,024 characters. Optional finite `padding` 1–4, default 1.2. | Moves the current editor viewport camera to fit those actors; returns their paths, combined unpadded bounds in centimeters, padding, camera location/rotation, world and revision. |

Unknown parameters, coerced values such as boolean limits, asset subobjects,
redirectors, and filesystem paths are rejected. Native StaticMeshActor snapshots
also report `static_mesh_path`, `collision_enabled`, `collision_profile`, and
`material_slot_count`.

Every response is capped at 1 MiB after UTF-8 serialization. Exceptionally long
existing actor labels or paths can still exhaust that byte budget despite the
collection limits; `response_too_large` then asks the caller to reduce `limit` or
narrow the query instead of sending a partial JSON response.

## Scope and limits

`status` and `context` remain available during Play or Simulate. Their actors and
selection describe the editor world, not the PIE game-world copy. Other actions
require Play/Simulate to stop. No action automatically stops a game session.

Context and validation collect at most 5,000 loaded actor candidates, sort those
paths, and report an incomplete scan when that cap is reached. Context returns at
most `limit` actors, selected paths, and dirty package names independently; actor
filtering does not hide the editor selection. Dirty packages describe loaded world
and content packages across the current editor process. Revision computation keeps
the existing actor identity/transform semantics; it does not certify every asset
or component property.

Validation examines at most `limit` matching actors, 64 static mesh components per
actor, 64 material slots per component, and returns at most 400 warnings. It checks
missing static meshes, missing material assignments, disabled or absent simple
collision, and negative/zero/non-finite actor scales. Decoration, intentional
mirroring, and complex collision can be valid design choices; findings are warnings.
Unloaded World Partition actors, Blueprint compilation, rendering quality, cooking,
physics behavior, and gameplay are outside these checks. `scan_incomplete: false`
means the requested loaded-actor scan completed, not that the entire game passed.

Asset inspection loads only the explicitly selected static mesh; Unreal can load
its normal dependencies. Other asset classes return registry metadata without
loading the asset. No directory is scanned from disk, and no asset is modified or
saved. Geometry counts describe the mesh LOD data, not measured render cost.

Capture reads only the current level editor viewport. It never captures the desktop
or writes a screenshot path. It preserves aspect ratio and may reduce resolution
further to keep PNG data at or below 720 KiB, leaving room for base64 and metadata
inside the 1 MiB bridge response limit. Source viewports above 16 million pixels are
rejected to bound renderer readback. Missing/headless viewports return
`viewport_unavailable`; no screenshot is fabricated. Capture can briefly stall the
game thread while waiting for the renderer. It pushes pending component render
updates and explicitly redraws the selected editor viewport before readback, so
background-editor throttling does not return an older camera view. Capture also
returns the camera location and rotation used for that image. Existing editor
screenshot/movie requests must finish first; the bridge does not take them over.

`frame` is an explicit viewport-state mutation and requires the MCP client's expected
project binding. Every actor and its component bounds are checked before the camera
moves. It uses the current camera orientation and instantly focuses the union of the
requested bounds with the chosen padding. It does not select actors, change geometry,
or save the map. Piloted/actor-locked viewports are rejected to avoid moving a camera
actor. Missing targets, invalid bounds, PIE, and absent viewports return errors;
there is no fallback to another world or a desktop camera. Frame first, then capture
the resulting viewport to inspect a created layout.

Native automation checks input rejection, context limits and play-mode policy,
scene warnings, known mesh dimensions, and the no-viewport error path. A successful
real viewport capture and visual inspection remain a separate local acceptance
check; headless automation does not prove image quality.
The `FrameSafety` suite also checks exact target validation and preservation of actor
transforms, counts, and selection when a frame request is rejected.

## Place an inspected static mesh

`preview` additionally accepts `spawn_static_mesh` operations with an exact
`asset_path`, `label`, and the same optional `location`, `rotation`, and `scale`
fields as primitive spawning. `shape` and `actor_path` are not accepted for this
operation. Paths follow the same `/Game` or `/Engine` restrictions as asset inspection.
Only registry-confirmed native `UStaticMesh` assets are loaded; Blueprints, other
asset classes, and redirectors are rejected.

The preview binds the live mesh object identity. Apply resolves every asset before
opening the existing Undo transaction and rejects a replaced or collected mesh object,
including replacement at the same path. Spawning creates an exact native
`AStaticMeshActor` with the mesh's existing materials. It does not replace meshes on
existing actors or edit material assignments. Existing limits remain: 20 operations,
120-second single-use plans, explicit project binding, stale-world checks, one Undo
transaction, and no automatic save.

`StaticMeshPlacement` automation verifies selected-asset assignment and Undo,
invalid/nonmesh/redirector rejection, and asset-object replacement invalidation.
The replacement fixtures exist only in memory and are removed from the asset registry
after the test; no asset package is saved.
