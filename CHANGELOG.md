# Changelog

## 0.3.0a1

- Added exact selected-actor inspection with world AABBs, material assignments,
  editability and native blockers. The MCP surface now contains 27 tools.
- Added bounded process-local snapshots, fresh selected-actor diffs, and explicit
  passed/failed/unverifiable checks for transforms, bounds, spacing, materials,
  labels and folders.
- Added deterministic alignment, distribution, pivot grid snapping and grounding
  previews from native measurements, with post-edit verification requirements.
- Added material assignment and label/folder edits for supported native actors
  through reviewed, one-shot Undo transactions. No material creation or arbitrary
  property editing is exposed.
- Added optional session/world/revision preconditions at preview creation and
  expanded native state checks for measured bounds, metadata and assigned assets.
- Added bounded plan records and local replay suppression. Interrupted apply
  outcomes remain unknown; receipt or readback problems do not hide a confirmed
  native apply result. Records are not persistent recovery or scene backups.
- Added current/isometric/top/front/right camera framing presets, preserving the
  editor's projection type; directional presets require a perspective viewport.
- Added CLI `inspect` and JSON-file `verify`, native workflow capability checks in
  `doctor`, the `verified_edit_workflow` prompt, and `jev://checks` examples.
- Documented the [spatial workflow](docs/SPATIAL_WORKFLOWS.md),
  [verification contract](docs/VERIFICATION.md), and
  [prioritized roadmap](docs/ROADMAP.md).

This is an alpha. Maps are not automatically saved. Snapshots and plan records are
bounded and local to one MCP process; restart or eviction loses them. See
[validation evidence](docs/VALIDATION.md) for the independently recorded test
layers. Earlier provider evaluations and v0.2 captures remain historical evidence,
not measurements of v0.3 productivity, production reliability or broad adoption.

## 0.2.0a1

- Added explicit local MCP catalog discovery, bounded pagination, versioned exact
  schemas, searchable action presets, local retrieval and optional Jev selection.
- Added compact editor context, static mesh metadata, deterministic scene warnings,
  native viewport framing and MCP image capture.
- Added existing static mesh placement and measured grid, stair and room previews
  through the same single-use native Undo transaction.
- Verify native actor identity/transform readbacks after applying tracked previews.
- Added local asset hard filters and diagnostic grouping with optional batched Jev
  assistance, explicit cloud status and offline fallback.
- Added CLI doctor/context/catalog/layout commands, two MCP workflow prompts,
  recipes and detailed user documentation.
- Reject numeric coercion in operation inputs, cap native UTF-8 responses, validate
  returned PNG structure and retain applied results when verification data is bad.

This remains an alpha. See `docs/VALIDATION.md` for separately recorded Python,
protocol, provider, native editor and rendered viewport evidence. No production
accuracy, broad engine compatibility or developer speedup is claimed.

## 0.1.0a2

- Fixed native PowerShell security-module resolution and added credential regression tests.

## 0.1.0a1

- Initial typed Jev client, stdio MCP server, authenticated editor bridge, primitive
  preview/apply transactions, Unreal sandbox and public evaluation harness.
