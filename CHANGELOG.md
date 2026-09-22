# Changelog

## 0.8.0a1

- Added ten MCP tools (54 total) covering the next ten roadmap engineering slices.
- Added project-approved ordinary Blueprint math-input literal previews, native Undo
  and fresh compilation. Pin editing requires a separate local policy switch.
- Added scalar/vector material-instance and native light edits plus repeatable
  perspective camera poses, with expiring one-shot plans and native readback receipts.
- Extended mesh copies to decal reception, custom depth/stencil and translucent
  sorting, preserving existing refusal and fresh-verification rules.
- Added approved-surface trace placement, slope/overlap refusal, bounded mesh/import
  dependency diagnosis, native Recast route/agent checks, stored skeleton/animation
  compatibility and Widget Blueprint design-tree diagnostics.
- Added bounded editor timing/memory captures, cancellation and comparisons that
  reject incomplete or mismatched measurements. No GPU or productivity claim.
- Added native failure/Undo tests, rendered red-to-green material and camera-restore
  evidence, official MCP schema/transport checks and domain workflow documentation.
- These are bounded initial scopes; wider graph operations, hierarchy copies,
  DCC round trips, pawn movement, runtime UI and independent user studies remain open.

## 0.7.0a1

- Added four MCP tools for project-approved Blueprint compile targets, state-bound
  previews, one-shot compilation and retained diagnostics. The catalog now has
  44 tools. Inspection also supports exact native Widget and Animation Blueprints.
- Added reviewed interrupted-install recovery with write-ahead receipts, exact
  before/after file hashes and a kernel-held lease. Recovery preserves concurrent
  edits and distinguishes incomplete operations from committed installs.
- Added selected-validator compatibility metadata and clearer non-verdict reasons;
  queued jobs recheck scene revision before the first trusted callback.
- Added project-owned door, navigation, interaction and combat functional recipes,
  including setup, owned cleanup and deliberately failing cases.
- Improved narrow review layouts and native accessibility feedback, with explicit
  automation and manual acceptance boundaries.
- Added preregistered, counterbalanced workflow studies with exact evidence hashes,
  explicit missing/failure denominators and paired comparisons. This does not
  establish an independent productivity advantage.

See [validation evidence](docs/VALIDATION.md) for actual completed checks and
remaining real-user, clean-host and representative-project acceptance.

## 0.6.0a1

- Reworked native plan review into readable Before/After fields for all seven
  supported operation types, with exact transform values, units, source identities,
  material policy explanations and separately expandable mesh technical records.
- Made inspection, review, technical detail and outcomes selectable, read-only and
  keyboard-focusable. Explicit actions focus their relevant result; background
  countdown/status updates preserve text selection and focus.
- Added recovery guidance and separate refresh diagnostics. A failed passive read
  disables Apply without replacing the last action outcome. Expired and consumed
  plans remain disabled, with no global Apply shortcut or automatic retry.
- Added localization-ready interface labels and native presentation, recovery and
  rendered keyboard workflow fixtures. These do not establish screen-reader,
  translated-language or representative artist acceptance.
- The MCP catalog remains at 40 tools; bridge operations, execution permissions
  and provider behavior are unchanged. See [review workflow](docs/REVIEW_PANEL.md)
  and [validation evidence](docs/VALIDATION.md) for scope and completed checks.

## 0.5.0a1

- Added `unreal_mesh_preview`, increasing the MCP catalog to 40 tools. Freshly
  inspected replacement and copy recipes bind an explicit actor selection and
  revision to the native preview.
- Added native `replace_mesh` with explicit preserve-slots/default-material policy,
  and `duplicate_mesh` for controlled copies of supported native static mesh props.
  Previews disclose mesh bounds, material slots and copied collision/render settings.
- Added mesh/settings verification, material override inspection, asset change
  invalidation and review-panel details for both operations. Copies require fresh,
  distinct actor identities; the source is retained unchanged.
- Preserved mesh-default collision inheritance explicitly. Replacement previews
  refuse incompatible inherited collision defaults before an edit is applied.
- Mesh edits remain unsaved Undo transactions. Arbitrary cloning, attachments,
  simulated physics, automatic pivot correction and unknown customizations are
  outside this bounded adapter. See [mesh workflows](docs/MESH_WORKFLOWS.md) and
  [validation evidence](docs/VALIDATION.md).

## 0.4.0a2

Fix an intermittent Windows client request-spacing failure exposed by post-merge
Python 3.12 CI. The bridge rechecks its monotonic deadline after early timer
wakeups and starts the next interval after the HTTP attempt completes, including
failed attempts. The existing one-shot/no-retry execution behavior is unchanged.
Deterministic timing regressions cover early wakeups, dispatch overhead, failures
and cancellation. Native JevEditor remains 0.4.0; this patch changes Python only.

## 0.4.0a1

- Added **Window → Jev Review**, using the native inspect/preview/apply path for
  human review, translations and actor metadata changes.
- Added bounded native plan receipts and pending-plan discovery. MCP reconnects
  can inspect outcomes while the same editor stays open; editor crashes/restarts
  still lose receipts. Reentrant edits cannot evict an in-flight apply record.
- Added read-only loaded Blueprint graphs/pins/variables/stored diagnostics,
  direct asset dependency/referencer inspection and redacted import provenance.
- Added project-approved native Data Validation jobs and named functional tests
  in an existing standalone PIE session. Both are disabled until a maintainer
  configures an explicit allowlist. Trusted project callbacks are not sandboxed.
- Added reviewed source install/update/repair/uninstall plans with hash guards,
  owned-file manifests, retained backups, rollback and project enablement.
- Added setup diagnostics and startup-selected connection profiles that bind
  project, loopback endpoint and token file together. Native bridge ports are
  configurable through `JEV_BRIDGE_PORT` (1024–65535).
- Added a bounded benchmark harness with separate inputs/labels, keyword/Jev
  routing runs and independently labelled human-attested workflow evidence.
- Expanded the MCP surface from 27 to **39 tools**. See the
  [roadmap](docs/ROADMAP.md) for implemented scope and outstanding acceptance.

The [validation report](docs/VALIDATION.md) separates mock tests, live provider
requests, native automation, real editor calls and visual acceptance. These
foundations do not establish broad usability, productivity gains or readiness
for millions of users.

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
