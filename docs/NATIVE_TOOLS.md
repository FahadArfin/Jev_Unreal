# Native editor inspection and edits (bridge 0.5.0)

These actions use the existing authenticated loopback bridge. They execute on the
editor game thread and do not call a model provider. Requests remain JSON objects
with `action` and `params`; responses retain the `ok` / `result` or `error` envelope.
The MCP client checks authenticated status before every operation and compares its
project identity with the configured project or selected connection profile.
Edits, camera framing and project-job start/cancel require an explicit binding.
The listener uses `127.0.0.1` with `JEV_BRIDGE_PORT` (1024–65535; default 9845), a
matching Host header and bearer authentication. Profiles bind an endpoint, project
and token file together; see [connection profiles](CONNECTION_PROFILES.md).

| Action | Parameters | Result |
| --- | --- | --- |
| `status` | No fields. | Engine/bridge version, exact project, session/world/current-level/revision, PIE/Simulate flags and supported native capabilities. |
| `context` | Optional `query` string, at most 200 characters; optional integer `limit` 1–200, default 50. | Project, session, engine, editor world, current level, revision, PIE/simulation flags, actor snapshots, selected actor paths, and dirty package names. Each collection has a truncation flag. |
| `asset_details` | Required `path`: exact `/Game/...Asset.Asset` or `/Engine/...Asset.Asset` object path, at most 512 characters. | Registry identity; static meshes additionally report bounds in centimeters, material slots, up to 16 LOD vertex/triangle counts, and collision configuration. |
| `actor_details` | Required `actor_paths`: 1–20 unique exact actor paths, each at most 1,024 characters. | Ordered actor snapshots, world bounds, material assignments, editability reasons, and project/session/world/current-level/revision identity from one game-thread inspection. |
| `validate` | Optional `query` string, at most 200 characters; optional integer `limit` 1–200, default 100. | Deterministic warnings with actor/component paths, scan counts, revision, and completeness flags. |
| `capture` | Optional integer `max_dimension` 64–1024, default 1024. | Base64 PNG in `data`, `mime_type`, width, height, world, revision, and `source: "editor_viewport"`. |
| `frame` | Required `actor_paths`: 1–20 unique exact actor paths, each at most 1,024 characters. Optional finite `padding` 1–4, default 1.2; optional `view`: `current` (default), `isometric`, `top`, `front`, or `right`. | Moves the current editor viewport camera to fit those actors; returns their paths, combined unpadded bounds in centimeters, padding, chosen view, actual camera location/rotation, world and revision. |
| `pending_plans` | Optional integer `limit` 1–64, default 20. | Newest pending native plan summaries, total pending count and explicit truncation; never applies them. |
| `plan_status` | Required nonempty `plan_id`, at most 64 characters. | Retained native review/outcome, affected paths and retention/expiry information; historical, not current-scene verification. |

Unknown parameters, coerced values such as boolean limits, asset subobjects,
redirectors, and filesystem paths are rejected. Native StaticMeshActor snapshots
also report `static_mesh_path`, `collision_enabled`, `collision_profile`, and
`material_slot_count`. Actor snapshots also include `folder`, effective editor
`materials` as `{slot, path}` records, and `materials_truncated`. Material lists
are capped at 64 slots; an unassigned slot has `path: null`. Nonmesh actors report
`static_mesh_path: null`, `collision_enabled: null`, and an empty material list.
An actor without a mesh reports `static_mesh_path: null`.

Every response is capped at 1 MiB after UTF-8 serialization. Exceptionally long
existing actor labels or paths can still exhaust that byte budget despite the
collection limits; `response_too_large` then asks the caller to reduce `limit` or
narrow the query instead of sending a partial JSON response.

## Scope and limits

`status`, `context`, `pending_plans` and `plan_status` remain available during Play
or Simulate. Context actors and selection describe the editor world, not the PIE
game-world copy. Other scene actions, including `actors`, `assets`, exact actor
details, preview/apply, framing and capture, require Play/Simulate to stop.

Project inspection and job actions below have separate policies. Blueprint and
registry inspection, rule/test listing, job reads and cancellation remain
available during play subject to their identity/ownership checks. Data Validation
cannot start during Play/Simulate and pending validation cancels if play begins.
Functional tests require one already-running standalone PIE session and reject
Simulate/multiplayer. The adapter never automatically starts or stops a session;
approved project callbacks still execute trusted code with their own side effects.

Status capabilities describe implementation support, not permission to run a
validator or test. The MCP bridge checks exact capabilities for native history,
project inspection and job actions, as well as `actor_details`,
`preview_expected_state`, `set_material`, `set_metadata` and `frame_views` where
needed. Missing support returns `capability_unavailable` before dispatch.

Context and validation collect at most 5,000 loaded actor candidates, sort those
paths, and report an incomplete scan when that cap is reached. Context returns at
most `limit` actors, selected paths, and dirty package names independently; actor
filtering does not hide the editor selection. Dirty packages describe loaded world
and content packages across the current editor process. Revision computation binds
actor identity, transforms, labels/folders, attachments, visibility and locks. For
static mesh actors it also binds mesh identity, world bounds, material and override
counts, and the first 64 effective material and raw override identities. It does
not certify shader graphs, textures, mesh vertex contents, unloaded actors, or
every component property. Asset edits outside this bounded state require a fresh
inspection and human review where appropriate.

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
moves. The default uses the current camera orientation; optional presets require an
existing perspective viewport and never switch projection mode. `isometric` looks
diagonally along +X/+Y and down, `top` looks down -Z, `front` looks along -Y, and
`right` looks along +X. They remain perspective views. Framing instantly focuses the
union of the requested bounds with the chosen padding. It does not select actors, change geometry,
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
existing actors. Existing limits remain: 20 operations,
120-second single-use plans, explicit project binding, stale-world checks, one Undo
transaction, and no automatic save.

`StaticMeshPlacement` automation verifies selected-asset assignment and Undo,
invalid/nonmesh/redirector rejection, and asset-object replacement invalidation.
The replacement fixtures exist only in memory and are removed from the asset registry
after the test; no asset package is saved.

## Inspect, edit, and verify existing actors

`actor_details` first resolves every requested actor in the editor world. A missing
actor rejects the whole request; a successful response preserves input order and
returns `truncated: false`. Each actor's opaque `instance_id` combines the bridge
session with Unreal's lifetime object key, including its weak-object serial. It is
stable while that actor lives, changes on same-path replacement, and is not a
persistent asset identifier. Revisions also bind live world/current-level identity.
Each actor has `bounds_available` and `bounds_cm`, where
the latter is either `null` or `{min, max, center, size}` with finite three-number
world-space vectors in centimeters. Actors without usable component bounds remain
inspectable but cannot support bounds-based spatial recipes. `editable` and
`edit_blockers` describe native bridge edit eligibility, not whether every possible
operation would be valid: a requested material slot must also exist, for example.

All existing-actor edits require an exact native `AStaticMeshActor` with a root
component in an editable level, without actor locks, attached parents/children, or
child-actor ownership. Blueprints and actor subclasses are excluded. A plan can
target an existing actor only once across all edit types. Use separate inspected
plans when changing both its placement and its material.

| Preview operation | Required fields | Optional fields |
| --- | --- | --- |
| `set_transform` | `actor_path` | At least one of `location`, `rotation`, `scale`. |
| `set_material` | `actor_path`, exact `material_path`, integer `slot` 0–63 | None. |
| `set_metadata` | `actor_path` | At least one of `label`, `folder`. |
| `replace_mesh` | `actor_path`, `asset_path`, `material_policy` (`preserve_slots` or `mesh_defaults`) | None. |
| `duplicate_mesh` | source `actor_path`, new `label` | Absolute `location`, `rotation`, `scale`; omitted values inherit the source. |

The two mesh operations require native bridge 0.5 capabilities. They expose a
reviewed subset of native static mesh actor behavior; duplication is not a general
UObject clone. See [mesh workflow contracts](MESH_WORKFLOWS.md) for copied settings,
material-slot policy, unsupported customizations, pivot/collision limitations and
fresh verification. Normalized previews include effective materials and overrides,
component settings and mesh review details. The human review panel displays them.

Material assignment accepts only registry-confirmed native `Material` or
`MaterialInstanceConstant` assets under `/Game` or `/Engine`; redirectors, dynamic
instances, other classes, subobjects, and filesystem paths are rejected. Only the
selected asset and its normal Unreal dependencies are loaded. The slot must exist
on the target component. No material asset or parameter is edited.

Labels are trimmed before preview, must contain 1–80 valid characters, and reject
control characters. Folders are at most 256 characters and use relative forward
slash-separated segments. Empty string moves the actor to the world outliner root.
Leading/trailing slashes, empty/dot/parent segments, padded segments, controls,
backslashes, colons, and the reserved `None` name are rejected. Metadata edits do
not rename an actor's UObject path. Preview shows normalized requested fields and
the baseline `location`, `rotation`, and `scale` for material and metadata edits.

To bind a measured recipe to the inspection that produced it, include an optional
`expected_state` object in `preview` parameters:

```json
{"operations": [{"op": "set_metadata", "actor_path": "<exact inspected path>", "folder": "Blockout/Walls"}],
 "expected_state": {"session_id": "<inspected session>", "world_path": "<inspected world>", "revision": "<inspected revision>"}}
```

All three state fields are mandatory when the object is present; unknown fields
and coerced values are rejected. A mismatch returns `stale_plan` before any plan
is issued. Apply still checks the current revision, weak actor/asset identities,
target editability, baseline metadata/material assignments, and selected slots.
The bridge loads and validates all selected assets before beginning mutations.

Apply refuses an already active or unavailable Undo transaction with `editor_busy`.
Successful edits share one independent editor Undo transaction. If a mutation
fails after earlier operations succeeded, the bridge closes and undoes that whole
transaction, discards its redo entry, and verifies the restored scene revision.
`apply_failed` means this rollback was verified; `rollback_failed` means restoration
could not be confirmed and the current scene must be inspected. Neither failure
should be retried automatically. Rollback is bound to the exact transaction ID;
if Unreal discarded a no-op transaction, the bridge checks the unchanged scene
without undoing an earlier user action. Earlier user Undo entries remain intact.
The scene-edit adapter requests no map or asset save; arbitrary project callback
side effects are outside its rollback guarantee.

Native suites `ActorDetails`, `ExpectedState`, `MetadataEdits`, `MaterialEdits`, and
`EditRollback` cover these contracts. The rollback suite uses an internal,
automation-only fault hook after metadata, material, transform, and spawn operations
have completed, then checks state restoration, removal of the failed redo entry,
and preservation of the earlier Undo entry. It also fails after a no-op, checking
that Unreal's removal of the empty transaction cannot undo an earlier edit or
destroy the existing Redo entry. The fault hook is not a bridge action.

## Shared review and native history

**Window → Jev Review** and MCP share native plans and apply. The panel can
inspect selected actors and preview a translation or label/folder change; it also
lists MCP-created pending plans. Its two-second refresh never applies or retries.
An apply attempt consumes the preview, and nested preview/apply calls are refused
while another plan applies. Full behavior is in [the panel guide](REVIEW_PANEL.md).

Native history holds at most 64 records for 15 minutes from creation, while a
pending plan is executable for 120 seconds. Completed records are evicted before
pending records; an applying record is protected from expiry/capacity pruning.
Statuses are `pending`, `applying`, `applied`, `rejected`, `expired`, `rolled_back`
and `unknown`. An unknown/in-flight execution outcome is explicitly null.

Records survive MCP reconnection to the same editor session, but not editor
restart or crash. Undo, map changes and later edits do not rewrite a historical
receipt. `unreal_plan` first queries native `plan_status`; if that fails it may
return the client's bounded observation with `native_lookup_error`. That fallback
does not establish native receipt or completion. Inspect and verify fresh state;
do not automatically replay a plan after a timeout.

## Project inspection and jobs

These actions use the same authenticated route and game thread. They add explicit
project-owned checks without a generic script, class-construction or filesystem
endpoint. The detailed [project inspection](PROJECT_INSPECTION.md) and
[functional test](FUNCTIONAL_TESTS.md) guides define policy and result limits.

| Action | Parameters and scope |
| --- | --- |
| `blueprint_inspect` | One exact already-loaded native Blueprint `asset_path`; optional `graph_limit` 1–32, `node_limit` 1–256 and `pin_limit` 1–1024 (defaults 16/128/512). Reads stored identities/diagnostics without loading, compiling or editing; extension graph callbacks are omitted. |
| `asset_dependencies` | Exact `asset_path`; `direction` dependencies/referencers, `category` package/manage/searchable_name, `offset` 0–100000 and `limit` 1–200. Direct sorted registry edges only; defaults dependencies/package/0/100. |
| `asset_import_info` | Exact `asset_path`. Up to 16 recorded source basenames/timestamps/hashes from bounded registry metadata; no source-file read or availability claim. |
| `validation_rules` | No fields. Lists explicit configured aliases and loaded native validator availability; invokes no validator. |
| `validation_start` | 1–8 unique `rule_ids`, 1–20 unique exact `asset_paths`, plus `expected_project` and `expected_state` containing session/world/revision. The Python bridge supplies this binding from authenticated status; native code checks it before queueing. Requires an enabled project allowlist and no Play/Simulate. |
| `validation_job`, `validation_cancel` | Exact canonical UUID `job_id`. Reads a retained result or cancels remaining work between callbacks; cannot undo validator effects. |
| `functional_tests` | No fields. Lists configured aliases, exact placed test availability and standalone PIE eligibility; starts nothing. |
| `functional_start` | Approved `test_id` and inspected `expected_state` containing session/world/revision. Queues the corresponding placed test in its existing single standalone PIE world. |
| `functional_job`, `functional_cancel` | Exact canonical UUID `job_id`. Reads evidence or finishes/cleans up only the surviving exact test run owned by that job, leaving PIE running. |

Asset paths in these project tools are exact `/Game` or `/Engine` object paths,
at most 1,024 characters; unsupported classes, malformed paths and unknown fields
are rejected. Blueprint graph scans and results report explicit partial coverage.
Registry pages bound returned records, not Unreal's internal query cost, and can
change as the registry updates. Stored diagnostics are not a fresh compile result.

Execution policies default to disabled and cannot be changed through MCP. Only one
project job may run across validation and functional testing. Validation runs one
selected asset/rule pair per tick and distinguishes `valid`, `invalid` and
`not_validated`. Functional jobs require the actual native result, surviving exact
actor/world/run identities and completed cleanup; observed assertion errors prevent
a pass even if the test reports `Succeeded`. Cancellation of an old run cannot
stop a replacement run on the same actor.

Callbacks may load assets, mutate state, save or access services. They are trusted
project code, and cleanup cannot prove full restoration. Receipts explicitly use
`save_requested=false`, `saved=null` and `callback_side_effects_tracked=false`.
Configured 1–120-second budgets and cancellation are cooperative between callbacks;
a blocking callback can exceed that budget. Validation retains eight job records,
functional testing 64, for 15 minutes after completion in editor memory. Restart
discards them. Read [Unreal validation evidence](UNREAL_VALIDATION.md) for the
native, live and rendered checks actually completed.
