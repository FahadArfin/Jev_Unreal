# Unreal validation

Jev Editor **0.5.0** has native automation evidence on Unreal Engine **5.8.2**, Windows 11, from 2026-09-22 UTC. This is an original independent bridge; no upstream JevUnreal implementation was copied into this plugin. Native automation, live MCP checks and rendered acceptance are reported separately below.

## Current 0.5 native automation

The licensed build compiled and linked successfully. Visual Studio 14.51 still
emits Unreal's compiler-preference warning and engine-header deprecation warnings.
The final `artifacts/unreal-automation/index.json` report is timestamped
**`2026.09.22-04.28.42`**: **25 passed, 0 failed, 0 not run, 0 in progress**.
There are 23 clean suites and two warning-bearing suites with three warnings,
the same empty-bounds/typed-element fixture warnings described in the 0.4 section.

All 21 previous suites ran again. The four new suites passed without warnings:

| Test | Coverage |
| --- | --- |
| `Jev.Editor.MeshReplacement` | Both material policies, original identity/placement, exact materials, reviewed mesh metadata and settings, replay rejection and native Undo. |
| `Jev.Editor.MeshDuplicate` | Default and explicitly customized collision configuration, inheritance flag, disabled-actor/raw collision mode, nondefault visibility/tags/mobility, explicit location, distinct new identity, unchanged source and Undo. |
| `Jev.Editor.MeshGuards` | Unsupported components/settings/attachments/tags and active physics overrides; collision inheritance mismatch; asset/settings mutation, stale identities and notification-free default collision changes. |
| `Jev.Editor.MeshRollback` | Mixed replace/copy failure, earlier Undo preservation, transacted source/unrelated changes, actual actor destruction during callback and identity restoration; isolated transient-world switch returns unknown without using another world's Undo stack. |

The world-switch fixture restores its original editor context on the same game
thread, explicitly undoes only its retained transaction and destroys its temporary
world. The production bridge exposes no world-switch, actor-destruction, test-hook
or arbitrary-code operation. Runtime failures were fixed before this final report:
collision-profile assignment had cleared mesh inheritance, and an inactive physics
cache differed from class defaults after component registration. Unsupported active
overrides still refuse copying.

## Current 0.5 live acceptance

The official stdio client discovered **40 tools** against bridge **0.5.0** in the
exact repository sandbox. The new mesh smoke passed both replacement policies,
controlled copying, six fresh checks, source-preservation diff, separate new actor
identity, stale source material/transform refusal and one-shot replay rejection.
Four native 1014 × 479 viewport captures record the fixture changes. Existing
blockout, measured-edit and roadmap/reconnect smokes passed again. The saved
encrypted-key launcher also authenticated the correct native editor and discovered
all 40 tools. These tests made zero provider calls and requested no saves.

Separate rendered automation passed **1/1 `Jev.Rendered.ReviewPanel`**, zero
warnings, report **`2026.09.22-04.29.18`**. Its fresh
[1000 × 567 capture](images/review-panel-v0.5.png) was viewed: controls are arranged
and readable, with initial Apply disabled. The new mesh review text's before/after
content is tested through the native controller, not a representative user study.

This is bounded fixture evidence. It does not certify arbitrary prop cloning,
collision simulation, representative game projects, accessibility or production
reliability. [Full evidence](VALIDATION.md) separates these observations from the
historical installer, multiple-editor and live-provider runs below.

## Historical 0.4 native automation

The fresh `artifacts/unreal-automation/index.json` report is timestamped
**`2026.09.22-03.35.34`**. All **21 `Jev.Editor` suites passed: 19 clean and two
with warnings**, with **0 failed, 0 not run and 0 in progress**. All 14 earlier
native suites in the historical table below ran again, alongside these seven:

| Test | Result | Coverage |
| --- | --- | --- |
| `Jev.Editor.NativePlanHistory` | Passed | Pending/applied/rejected/expired/rolled-back receipts, history after Undo, session boundaries, bounded retention and completed-first eviction. Nested reads and blocked mutations cannot evict or overwrite an applying plan. |
| `Jev.Editor.ReviewSelection` | Passed | Panel controller inspection, translation and label/folder previews, before/after review text, shared native apply and Undo; empty, oversized and unsupported selections reject. This suite does not render Slate. |
| `Jev.Editor.BlueprintInspection` | Passed | Stored Blueprint graph/variable identities and diagnostics without compilation or dirtying; bounded results, cyclic subgraph traversal and omission of extension callbacks; unsupported assets and inputs reject. |
| `Jev.Editor.AssetProjectInspection` | Passed | Direct dependency/referencer registry queries, bounded pages, import provenance with private directories removed, and read-only metadata results. |
| `Jev.Editor.ValidationJobs` | Passed | Real native fixture validator execution, one asset/rule per tick, valid/invalid/not-validated outcomes, completed evidence after cancellation, exact session checks and disabled execution policy. |
| `Jev.Editor.ValidationJobSafety` | Passed | Rule allowlists, duplicate rejection, bounded diagnostics, world/configuration changes, cooperative timeout and callback reentry. Callback shutdown cannot append stale success or replace job ownership. |
| `Jev.Editor.FunctionalJobs` | Passed | An isolated real PIE fixture moves a probe and checks native success/failure, assertions followed by an explicit success result, stale state, allowlists, cancellation, timeout, permission revocation and PIE loss. Destruction/restart during callbacks cannot produce a pass; cancelling an old run leaves a later run on the same actor alone. |

There are **three warning entries across two suites**. `ActorDetails` reports
Unreal's empty-bounds navigation warning from its deliberately empty mesh fixture.
`SceneValidation` reports an externally referenced typed element being destroyed,
followed by Unreal's reference-tracking suggestion. These warnings did not fail
assertions; this was not a warning-free run. No suite reports an error.

This report comes from the dedicated repository sandbox and headless
`UnrealEditor-Cmd.exe -NullRHI` automation. The functional suite starts and stops
its own isolated PIE fixture; the production bridge has no PIE start/stop action.
The report establishes native fixture behavior, including the panel's nonvisual
controller path. It does not establish rendered panel usability, a live 0.4 MCP
workflow, or correctness of another project's validators and gameplay tests.
Raw reports and logs remain local under `artifacts/unreal-automation/` and the
sandbox's `Saved/Logs/`; those directories are not release contents.

## Historical 0.4 live and rendered acceptance

Both repository-owned sandbox projects compiled against UE 5.8.2. The second
project used the reviewed installer rather than the repository's additional plugin
directory, and completed install, upgrade, build, live connection and reviewed
source removal. Uninstall restored its previous project entry while preserving
generated binaries and backups. This is not a clean-machine installation test.

`Jev.Rendered.ReviewPanel` passed in a rendered D3D12 editor, report timestamp
**`2026.09.22-03.31.06`**, one clean pass. It opens the registered Slate tab, checks
ten tagged controls and disabled initial apply, and captures the actual content.
The [1000 × 567 native panel capture](images/review-panel-v0.4.png) was viewed and
shows readable labels and all controls. Native controller automation separately
checks preview/apply/Undo. This combination does not replace observed task
completion by artists, keyboard users or assistive-technology users.

Four real stdio MCP smoke paths passed against **39 tools**, bridge **0.4.0**:

- Existing baseline auth/project/preview/apply/stale-plan/layout/capture checks.
- Measured inspect/edit/verify workflows, negative verification cases and four
  1014 × 479 native captures; the new [isometric](images/verified-workflow-v0.4-isometric.png)
  and [top](images/verified-workflow-v0.4-top.png) images were viewed.
- New project inspection, disabled-policy refusals, and native applied-plan
  recovery after restarting MCP followed by fresh actor verification.
- Two editors with separate ports/tokens/projects: conflicting legacy variables
  cannot redirect profiles; cross-editor plans and wrong-project reads reject;
  own plans apply once and pass fresh label/instance checks. Explicitly approved
  native localization/material validators return `valid`/`not_validated` for the
  cube respectively, with its package remaining clean.

The final native automation also covers rejection of mismatched project/session/
world/revision at validation's queue boundary, preventing a restart between the
client preflight and native dispatch from redirecting a validation job. Live
functional execution remains covered by the separate native PIE fixture suite;
the live MCP smoke checks its default-disabled policy, not another game's tests.
The saved encrypted-key launcher initialized and discovered all 39 tools against
the correct 0.4 editor. All editor smokes made zero provider requests and requested
no map save. [Full evidence and limitations](VALIDATION.md) include report paths.

## Historical 0.3 native checks

The following build details and 14-suite table describe the **0.3.0a1** run on
the same engine and platform, on 2026-09-22 UTC. Its timestamp and totals are
historical, separate from the current 21-suite result above.

`scripts/Build-Unreal.ps1` completed successfully with the installed UnrealBuildTool and Windows SDK. Visual Studio compiler 14.51 emitted Unreal's warning that its preferred compiler is 14.50; the plugin compiled and linked successfully.

`scripts/Launch-Unreal.ps1 -AutomationTests` launched only the repository's dedicated `examples/JevSandbox` project. The final report was checked, rather than treating process exit as proof:

| Test | Result | Coverage |
| --- | --- | --- |
| `Jev.Editor.PlanLifecycle` | Passed | Preview causes no scene mutation; primitive spawn; transform of the resulting actor; single-use plan rejection; native editor undo restores the transform and removes the spawn. |
| `Jev.Editor.PlanSafety` | Passed | Changed scenes, expiry, different bridge sessions, replacement actors with reused object paths, Unicode label changes, and replacement editor worlds reject old plans. |
| `Jev.Editor.SchemaSafety` | Passed | Unknown actions and fields; excessive or incorrectly typed limits; path traversal; invalid operation elements; disallowed shapes; incorrect scalar/vector types; negative scales; excessive coordinates; mixed valid/invalid plans cause no mutation. |
| `Jev.Editor.ContextInspection` | Passed | Combined bounded actors/selection/context, truncation, strict parameters, and status/context availability under the simulated play-mode policy. |
| `Jev.Editor.SceneValidation` | Passed | Missing mesh, negative/zero scale and disabled-collision warnings tied to exact actor paths; bounded scans and incomplete-result flags. |
| `Jev.Editor.AssetInspection` | Passed | Exact engine cube dimensions, material/LOD/collision metadata, bounded results, missing assets, and rejection of filesystem/traversal/subobject paths. |
| `Jev.Editor.CaptureSafety` | Passed | Bounded dimensions, strict types, no arbitrary output path, and explicit missing-viewport failure. |
| `Jev.Editor.FrameSafety` | Passed | Unique bounded exact actor paths, padding limits, missing targets/viewports, and preservation of actor transforms/count/selection after rejected requests. |
| `Jev.Editor.StaticMeshPlacement` | Passed | Exact registry mesh placement and native Undo; missing/wrong-class assets, redirectors, malformed paths, and replacement assets that reuse reviewed paths are rejected. |
| `Jev.Editor.ActorDetails` | Passed | Exact ordered selection, finite world bounds, material assignments, unavailable bounds, missing targets, edit blockers and distinct identity after replacement at the same path. |
| `Jev.Editor.ExpectedState` | Passed | Strict session/world/revision checks, state changes after measurement, invalid fields and native material/folder changes invalidate stale inspection. |
| `Jev.Editor.MetadataEdits` | Passed | Labels/folders, strict fields/path validation, one edit per target, native Undo and identity guards. |
| `Jev.Editor.MaterialEdits` | Passed | Exact material assets/slots, invalid assets and slots, assignment identity, preserved transforms and native Undo. |
| `Jev.Editor.EditRollback` | Passed | Injected failure after metadata/folder, material, transform and spawn changes restores the baseline; no-op failures preserve earlier user Undo/Redo contexts. |

Final report timestamp: **`2026.09.22-02.39.31`**. Totals: **14 passed (13 clean and one with a warning), 0 failed, 0 not run, 0 in progress**. Schema tests also verify that oversized responses are rejected by UTF-8 byte size. Machine-specific raw artifacts remain local under `artifacts/unreal-automation/` and `examples/JevSandbox/Saved/Logs/`. ActorDetails deliberately exercises an empty-bounds mesh, which emits one Unreal navigation warning; this is reported separately from failed assertions.

For that run, the launcher checked report freshness, completion counts, and each of the 14 expected test names. Headless execution used `UnrealEditor-Cmd.exe -NullRHI`; this does not establish rendered capture success. `-NoShaderCompile` was intentionally omitted because combining it with the installed headless engine previously caused a startup crash before tests ran.

## Historical 0.3 live editor and visual evidence

The real stdio MCP server discovered **27 tools** and passed `scripts/smoke_editor.py` against the authenticated rendered sandbox editor. `artifacts/editor-smoke-v0.3.json` records engine `5.8.2-56702186+++UE5+Release-5.8` and these checks:

- Authentication/origin rejection and exact project identity.
- Primitive preview without mutation, spawn/transform, consumed-plan rejection and stale-plan rejection.
- Actor/asset bounds, compact context, exact native mesh measurements, existing static-mesh placement and bounded scene warnings.
- Preview/application of grid, stairs and room layouts, with **4, 4 and 5 actors** respectively; all three readback verifications passed.
- Native camera framing followed by a **1014 × 479** PNG from the editor viewport.

In 0.2, visual inspection found that viewport invalidation alone could leave old pixels in a background editor capture. The bridge submits pending world/component updates, explicitly draws that viewport, and flushes rendering commands before reading pixels. The historical [staircase capture](images/verified-stair-blockout.png) preserves that evidence.

For 0.3, `scripts/smoke_verified_workflows.py` also passed the full exact-inspection,
snapshot/diff, state-bound preview/apply, measured alignment/distribution/grid/ground,
metadata/material and fresh-verification loop. Deliberate mismatches failed and
missing/replaced identity evidence stayed unverifiable. All four perspective camera
presets produced native captures with matching camera metadata. The
[isometric](images/verified-workflow-isometric.png) and
[top](images/verified-workflow-top.png) images visibly show the three rotated test
objects; front/right views were reviewed too. Objects can occlude one another in
a given view. A preset does not establish visibility, simulated grounding or art quality.

This is narrow visual acceptance of one blockout and the capture path. It is not gameplay, character traversal, collision simulation, an art review, or confirmation that every scene renders correctly. All smoke-test scene changes remain unsaved; no other game project was modified.

## Bridge guarantees and scope

- The bridge starts only with a printable non-space ASCII `JEV_BRIDGE_TOKEN` of 32–256 characters. A token should be randomly generated. The plugin never prints it.
- The listener explicitly binds `127.0.0.1` at `JEV_BRIDGE_PORT` (1024–65535; default 9845); invalid configuration or an occupied port fails closed. Requests require the matching loopback Host header and a bearer token, and requests with an Origin header are rejected.
- A selected [connection profile](CONNECTION_PROFILES.md) binds one MCP process to an exact project, loopback URL and token file together. It never mixes those settings with inherited legacy credentials or switches editors automatically. The client verifies the authenticated editor's exact project before every read or edit. Profiles do not launch editors or coordinate multiple clients editing the same project.
- Unreal's HTTPServer dispatches the action handler on the game thread. Off-thread invocation is rejected. The plugin only unbinds its own route on shutdown and never stops shared HTTP listeners. Python serializes calls and paces all HTTP requests, including identity checks, to at most **20 per second per client**, below the native **30-per-second** authenticated limit. Throttled operations are not automatically retried.
- Scene operations remain bounded inspection, native viewport framing/capture, allowed primitive spawning, exact existing static-mesh placement, transforms, existing material assignment and actor labels/folders. Scene edits reject Play/Simulate. There is no Python, console, deletion, arbitrary file write, map save, asset import, or arbitrary class-construction endpoint.
- [Project inspection](PROJECT_INSPECTION.md) reads an already-loaded exact native Blueprint's stored graph data without loading/compiling it or invoking extension graph callbacks. Asset Registry dependency pages describe direct edges; import provenance returns recorded basenames/timestamps/hashes without reading source files. Bounds and explicit incomplete/truncation flags limit the returned evidence; it is not a fresh compile or a complete runtime dependency graph.
- Native Data Validation and [functional test jobs](FUNCTIONAL_TESTS.md) are disabled by default and require explicit project-owned allowlists. Validation invokes only selected loaded native validator classes on selected exact assets and refuses execution during PIE/Simulate. Functional jobs run one approved placed test in an already-running single standalone PIE session; they never start/stop PIE or support multiplayer/Simulate. Job ownership, session/world identity, configuration changes and callback reentry are checked; cancellation and deadlines are cooperative between callbacks.
- Validators and functional tests execute trusted project code, whose loading, mutation, saving and cleanup side effects cannot be sandboxed or fully tracked. Their receipts say `save_requested=false`, `saved=null` and `callback_side_effects_tracked=false`. A cleanup attempt is not proof that every side effect was undone. Native result distinctions preserve invalid, skipped, timed-out, cancelled and interrupted outcomes rather than presenting them as success.
- Existing-actor edits are intentionally restricted to the exact native `AStaticMeshActor` class with no attached parent, attached children, or child-actor ownership. Blueprint-derived actors are rejected because editor callbacks may run construction scripts with side effects.
- `spawn_static_mesh` resolves an exact `/Game` or `/Engine` registry object path and accepts only native StaticMesh assets. Redirectors/subobjects are rejected. Preview retains the mesh's weak UObject identity; apply rejects a missing/replaced object even when a replacement reuses its path, and holds resolved meshes strongly during the transaction. This does not fingerprint in-place changes to mesh contents.
- A plan is bound to the editor session, project, weak world identity, weak target identities, and original scene actor identities. Its revision covers live actor/world/level identity, object path, class, root path, label/folder, transforms, attachment, hidden/locked state and mesh/material assignment identity. UTF-8 hashing preserves Unicode label changes. It does **not** hash full material, mesh, Blueprint, or asset contents. Actor details return session-scoped `instance_id`; fresh verification can reject replacement at a reused object path.
- Plans expire after 120 seconds, contain at most 20 operations, and are consumed on an apply attempt. At most 64 live plans are retained. Preview validation runs before an editor Undo transaction is opened, and an existing active transaction blocks apply. Failure rollback only undoes the exact Jev transaction GUID; a discarded no-op transaction must not undo the user's preceding action. Supported edits/spawns are restored; arbitrary third-party callback side effects are outside this guarantee. An uncertain restoration returns `rollback_failed` and requires inspection.
- The [Jev Review panel](REVIEW_PANEL.md) and MCP share native previews and apply. Refreshing the panel never applies a plan. Up to 64 native history records remain for 15 minutes in editor memory; applying records are protected from retention pruning and nested mutation. MCP reconnection can recover those receipts, while editor restart/crash loses them. Receipts remain historical after later edits or Undo and require fresh verification; an MCP-local fallback explicitly reports the native lookup error.
- The MCP layer compares successful apply readback against normalized previews, including actor paths, transforms and applicable mesh identity. Equivalent Euler rotations are accepted. Malformed readback is reported as failed verification rather than hiding an already-applied result. Readback does not certify visual/gameplay behavior; never blindly retry an ambiguous apply.
- Scene/asset queries return at most **200 entries**. Context/validation report independent truncation/incomplete flags. Extended inspection scans at most 5,000 candidate actors, 64 components/material slots per applicable item, and 400 warnings; asset LOD details are capped at 16.
- Native capture uses the existing editor viewport, without desktop capture or arbitrary output paths. Requested maximum dimensions are 64–1024; source area is capped at 16 million pixels and compressed PNG at 720 KiB. Existing screenshot/movie operations block bridge capture. Python checks bounded PNG structure/checksums and dimensions before emitting MCP image content; this does not assess visual quality.
- Handler request bodies are capped at **64 KiB**, and serialized responses at **1 MiB of UTF-8**. Unreal's HTTPServer parses the body before the handler, so these application limits are not a transport-level defense against a hostile local process.

## Remaining acceptance boundaries

There is no evidence yet for production-map performance, sustained multi-client load, other Unreal versions/platforms, multiplayer, broad gameplay/visual quality, or every third-party editor plugin. Current native fixtures also do not establish fresh-machine installation, crash-durable recovery, rendered panel accessibility/usability or full workflow efficiency. External MCP discovery is metadata-only and separate from this authenticated editor bridge. Actual discovery of the installed Epic server's full catalog remains unverified; real SDK/network tests use synthetic local servers. [Python, provider and catalog evidence](VALIDATION.md) is reported separately.
