# Selected-actor snapshots and deterministic verification

Snapshots and checks inspect explicitly selected loaded actors through the authenticated `actor_details` bridge action. They never edit the scene, save files, inspect unrelated projects, contact Jev, or upload project data. Their scope is reported actor state, not complete asset contents, gameplay, simulated collision, or visual quality.

## Capture and compare

`SceneSnapshots(bridge).capture(actor_paths)` accepts 1–20 unique exact actor paths. It requests authoritative details once and stores an isolated copy containing project/session/world identity, current level, revision, and the selected actors' instance identity, class, label, folder, transforms, static mesh path, collision flag, material slots, world bounds, editability and blockers. The response includes `snapshot_id`, the captured actors, identity, revision and remaining lifetime at capture.

Storage is local to that MCP process: at most **32 snapshots**, a **15-minute lifetime**, and **2 MiB total serialized normalized actor data**. Python object overhead is additional. Oldest snapshots are evicted to respect the count/byte limits; eviction counts are returned. A single oversized selection is rejected. Snapshots disappear when the process stops and are never written to disk. Returning actor details does not expose a mutable reference to the stored baseline.

`diff(snapshot_id)` obtains a fresh exact-selection read and compares it with the baseline. The synchronous `compare(snapshot_id, actor_details_result)` uses a supplied result instead. Both require the same project, editor session and world, matching actor order and complete records. A changed current level is reported separately. Missing actors, truncation, malformed values, an expired/missing snapshot or an unavailable editor yield **`unverifiable`**, never a false unchanged result. An absent actor could be unloaded or unavailable; it is not proof of deletion.

Comparison reports `unchanged`, `changed` or `unverifiable`, with before/after values. `changed` includes transforms, bounds, labels/folders, mesh/collision metadata and editability. `added` and `removed` refer to material slots on the selected actors. A null slot path means a known unassigned material; the `before_exists`/`after_exists` flags distinguish that from an absent slot. These results do **not** detect scene-wide actor additions/removals. A global revision change without changes to the selected records remains `unchanged` within that stated scope.

Each actor must include an opaque `instance_id` identifying the live Unreal object within its editor session. Reusing a path for a replacement object does not preserve this identity. Such a comparison returns `unverifiable` with reason `actor_replaced` and a `replaced` list containing the old/new instance tokens. It does not compare a clone as though the original actor survived. Tokens must be paired with the project/session/world identity; they are not persistent asset IDs.

Bounds may be explicitly unavailable (`bounds_available: false`, `bounds_cm: null`). Snapshots preserve that state without inventing spatial evidence. Material truncation, duplicate slot IDs, missing required fields, invalid finite vectors, inconsistent bounds or mismatched selected paths invalidate a record. A declared `material_slot_count` must agree with the observed slots and truncation flag. Identity strings must be nonblank, valid Unicode and free of control characters. Material and edit-blocker ordering is normalized before comparison.

## Checks

`verify(checks, actor_details_result)` evaluates supplied complete details. `await verify_fresh(bridge, checks)` first requests authoritative details for the checks' exact actor paths. Both accept **1–64 checks** referencing at most **20 unique actors**. No checks is an invalid request, not a vacuous pass. An optional unique `id` names each check; otherwise IDs are assigned in order.

| `kind` | Required fields beyond `kind` | Comparison |
| --- | --- | --- |
| `transform_equals` | `actor_path`, at least one of `location`, `rotation`, `scale` | World transforms; rotation is `[pitch,yaw,roll]` in degrees and equivalent Euler orientations are accepted |
| `bounds_size` | `actor_path`, `expected_cm: [x,y,z]` | World-axis-aligned bounding-box dimensions, not local mesh dimensions |
| `bounds_anchor` | `actor_path`, `axis: "x"/"y"/"z"`, `anchor: "min"/"center"/"max"`, `expected_cm` | One world AABB anchor coordinate along the selected axis |
| `bottom_z` | `actor_path`, `expected_cm` | The world's AABB minimum Z, not proof of ground contact |
| `material_slot` | `actor_path`, `slot`, `expected_path: string or null` | Exact assigned path at an existing reported slot; null means unassigned |
| `label` | `actor_path`, `expected` | Exact actor label |
| `folder` | `actor_path`, `expected` | Exact editor folder; `""` is the root folder |
| `min_gap` | `first_actor_path`, `second_actor_path`, `axis: "x"/"y"/"z"`, `minimum_cm` | Symmetric separation between two world AABB intervals on one axis |

Spatial checks use `tolerance_cm`, default **0.1 cm**, bounded to **0–100 cm**. Transform checks instead expose `location_tolerance_cm` with those same limits, `rotation_tolerance_degrees` default **0.1**, bounded **0–5**, and `scale_tolerance` default **0.001**, bounded **0–0.1**. Tolerances are absolute. Numeric strings, booleans and non-finite values are rejected. Material slot indices are 0–63. Expected positions/sizes are bounded to magnitude 1e12 cm; minimum gaps must be nonnegative.

`min_gap` computes the larger of `B.min - A.max` and `A.min - B.max` along the requested world axis. Positive means separation; negative means overlapping intervals. It passes when `gap + tolerance >= minimum`. AABBs can overlap even when rotated or concave geometry does not. This check establishes neither exact mesh clearance nor collision/physics behavior.

Each result contains `passed`, `failed` or `unverifiable`, with expected and actual evidence. Missing bounds make only the corresponding spatial checks unverifiable when the remaining record is valid. Unlike complete snapshots/diffs, per-field verification accepts material truncation: observed unique slots and unrelated transform/bounds/metadata fields can still be checked. An absent material slot is unverifiable and cannot match an expected null assignment. Overall status is `failed` if any check fails, otherwise `unverifiable` if any cannot be evaluated, and **`passed` only when all requested checks pass**.

Optional `expected_identity` contains `project_file`, `session_id` and `world_path` (plus optional `current_level` metadata). Different project/session/world makes verification unverifiable. Optional `expected_revision` requires exact equality with the observed revision. Do not pass a pre-edit revision when verifying an intended edit: the edit is expected to change it. Use the captured identity and intended post-edit check values instead. The fresh helper also checks a configured bridge project against the returned actor-details identity.

All single-actor checks optionally accept `expected_instance_id`; `min_gap` accepts `first_expected_instance_id` and `second_expected_instance_id`. An identity mismatch returns `unverifiable`/`actor_replaced` even when the replacement has identical properties. Omission checks the currently observed object at the specified path. Spatial previews always bind generated checks to the inspected actor instances; keep that binding and pass their project/session/world identity when checking after apply.

Example checks for one native block:

```json
[
  {"kind":"transform_equals","actor_path":"/Temp/Map.Map:PersistentLevel.Block","location":[50,50,50]},
  {"kind":"bounds_size","actor_path":"/Temp/Map.Map:PersistentLevel.Block","expected_cm":[100,100,100]},
  {"kind":"bottom_z","actor_path":"/Temp/Map.Map:PersistentLevel.Block","expected_cm":0},
  {"kind":"folder","actor_path":"/Temp/Map.Map:PersistentLevel.Block","expected":"Blockout"}
]
```

Replace the example path with the exact current editor path. A supplied result alone is not evidence of freshness; use the fresh helper for current observations. Native revision coverage still does not fingerprint every property or asset byte. Snapshot equality cannot certify unreported state.

## Validation scope

Synthetic tests cover strict fields, unknown/truncated records, duplicate identities/material slots, malformed and non-finite numbers, snapshot isolation/expiry/eviction/byte limits, changed project/session/world, same-path actor replacement, missing actors, explicit absent bounds, null versus absent material slots, declared material counts, gimbal-equivalent rotations, AABB anchors/gaps, tolerance limits and mixed failed/unverifiable results. These are implementation tests with injected bridge results; actual editor and gameplay evidence must be recorded separately in the release validation documents.
