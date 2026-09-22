# Measured actor editing

`unreal_spatial_preview` prepares translations of existing actors from fresh
`unreal_actor_details` measurements. It supports alignment, distribution, grid
snapping, and grounding on an explicit height. Calculations run locally without a
model or provider key. The result is a preview, not an applied edit.

Use exact actor paths returned by the editor. Each recipe accepts 1–20 unique
paths; distribution needs at least three. Unsupported, attached, locked, or
otherwise uneditable actors are rejected by the native editor's editability
checks. An actor also needs finite world bounds. No actor names are guessed and
no selection is silently omitted.

## Inspect, preview, apply, verify

1. Inspect the intended project and actors. Obtain their exact paths and check
   `instance_id`, `editable`, `edit_blockers`, `bounds_available`, and `bounds_cm`.
2. Call `unreal_spatial_preview` with a recipe below. The helper obtains its own
   fresh `actor_details` response, computes translations, and passes its exact
   session, world, and revision to the native preview. A change between inspection
   and preview invalidates that request.
3. Review `operations`, `actors[].before`, `actors[].after`, and the native
   `preview`. The before/after bounds are measured and predicted respectively.
4. Apply `preview.plan_id` separately through the existing guarded apply workflow.
   Native plans remain short-lived and single-use. A stale or failed preview is
   not automatically retried.
5. Inspect fresh actor details and run the returned `verification_checks` through
   the scene verification workflow. Review a rendered capture as well; successful
   numeric checks do not establish visual quality or gameplay suitability.

Python integrations can call `compile_spatial(recipe, actor_details)` for a pure
calculation or `await preview_spatial(bridge, previews, recipe)` for inspection
followed by a state-bound preview. Neither function applies, saves, or calls a
model. The recipe types are `AlignRecipe`, `DistributeRecipe`, `SnapGridRecipe`,
and `GroundRecipe`, combined in the discriminated `SpatialRecipe` type.

## Alignment

```json
{
  "kind": "align",
  "actor_paths": ["/Game/Maps/Test.Test:PersistentLevel.StaticMeshActor_0"],
  "axis": "x",
  "anchor": "min",
  "target_cm": 200
}
```

Translate each actor until its world-axis-aligned bounding box's selected
`min`, `center`, or `max` lies on `target_cm` along `x`, `y`, or `z`.
`anchor` defaults to `center`. Other location axes, rotation, and scale remain
unchanged. The bounds center may differ from the actor pivot, including for
rotated or offset meshes.

## Distribution

```json
{
  "kind": "distribute",
  "actor_paths": [
    "/Game/Maps/Test.Test:PersistentLevel.StaticMeshActor_0",
    "/Game/Maps/Test.Test:PersistentLevel.StaticMeshActor_1",
    "/Game/Maps/Test.Test:PersistentLevel.StaticMeshActor_2"
  ],
  "axis": "x",
  "mode": "equal_gaps"
}
```

Actors are ordered by their world AABB center along the selected axis, then by
exact path to resolve equal centers deterministically. Operations and per-actor
results retain the original requested order; `measurements.axis_order` records
the geometric order.

- `centers` is the default. It keeps the first and last bounds centers fixed and
  places intermediate centers at uniform intervals. Unequal widths can still
  produce overlaps.
- `equal_gaps` preserves the entire selection's minimum and maximum bounds along
  the axis. It accounts for each actor's measured width and computes a common
  nonnegative edge-to-edge gap. A zero gap means touching AABBs. If the summed
  widths exceed the available outer span, the recipe is rejected. Existing
  overlaps can be resolved when the total span permits it.

For equal gaps the endpoints are the outer bounds of the whole selection, which
can belong to actors other than the first and last in center order. The first
actor's pivot may therefore move. Neither mode changes the other axes or tests
three-dimensional collision.

## Grid snapping

```json
{
  "kind": "snap_grid",
  "actor_paths": ["/Game/Maps/Test.Test:PersistentLevel.StaticMeshActor_0"],
  "grid_cm": [10, 20, 10],
  "origin_cm": [0, 0, 5]
}
```

Snap actor **pivot locations** independently on all three world axes. `grid_cm`
can be a positive scalar for all axes or an `[x, y, z]` spacing. `origin_cm`
defaults to `[0, 0, 0]`.

The nearest grid point wins. Exact half-grid ties round away from the supplied
origin: with a 10 cm grid at zero, +5 becomes +10 and −5 becomes −10. Decimal
arithmetic based on the supplied numbers' decimal representations makes decimal
ties such as 0.15 on a 0.1 cm grid stable. This rule is explicit and does not
depend on an editor preference or Python's nearest-even rounding.

## Grounding

```json
{
  "kind": "ground",
  "actor_paths": ["/Game/Maps/Test.Test:PersistentLevel.StaticMeshActor_0"],
  "z_cm": 0
}
```

Translate each actor vertically so its world AABB minimum Z is exactly the
specified height. This uses measured geometry, including a displaced pivot. It
does not trace terrain, find a floor, inspect collision, or rotate objects to
match a surface.

## Limits and verification evidence

Recipes reject unknown fields, coercion from numeric strings or booleans,
nonfinite numbers, duplicate paths, and invalid object paths. Target coordinates
and grid origins are bounded to ±1,000,000 cm; grid spacing is 0.001–1,000,000 cm.
Each vector has exactly three values. Calculated actor locations must also stay
within ±1,000,000 cm.

Inspection must return the exact requested paths in the requested order and
explicitly report `truncated: false`. Each actor must include its opaque live-object
`instance_id`, report `editable: true`,
no edit blockers, `bounds_available: true`, and consistent finite bounds. World
bounds coordinates are limited to ±1,000,000,000 cm. Rotation must fit the native
±36,000 degree limit and positive scale must stay within 0.001–1,000. Truncated
material metadata is unrelated to geometry and does not invalidate an otherwise
complete spatial inspection. Per-field verification likewise checks observed
transforms/bounds despite material truncation; complete snapshots and diffs still
require untruncated material records.

The helper emits only existing `set_transform` operations, with the calculated
location and the original measured rotation and scale. Native permission,
project, editability, and stale-plan checks remain authoritative.

`verification_targets` contains the expected transform and full translated AABB
for each actor. `verification_checks` contains transform and bounds-size checks,
plus the selected AABB anchor for alignment, centers or minimum edges for
distribution, and bottom heights for grounding. Grid snapping checks the pivot
transform and bounds size. There are at most 60 checks for 20 actors. The anchor
checks detect bounds shifting independently of an unchanged transform and size.
They establish the intended AABB placement along the recipe axis within ordinary
verification tolerances; they do not establish exact mesh clearance, collision,
rendering, or every off-axis AABB coordinate. Compare fresh inspection data with
the full targets when those details matter.

Every generated check includes the inspected actor's `expected_instance_id`.
A replacement object at the same path cannot satisfy these checks: verification
reports `unverifiable` with reason `actor_replaced`, even for an identical clone.

After an intended apply, use the original **project/session/world** identity to
bind verification, but do not require the pre-edit revision to remain unchanged:
a successful edit changes the revision. `measurement_state` records the original
revision for preview safety, not a post-edit expected revision. `applied: false`
in the spatial result describes the preview operation; later apply and readback
results provide their own evidence.

The focused Python tests exercise pure geometry and mocked bridge boundaries.
Actual engine build, editor automation, live integration, and visual acceptance
are recorded separately in [VALIDATION.md](VALIDATION.md).
