# Toward broad adoption

The goal is dependable everyday Unreal work for people and AI clients. A large
audience needs clear installation, compatible versions, understandable failures,
recovery and evidence of useful results. More commands alone do not establish any
of those outcomes. This is a prioritized product roadmap, not a support promise.

## Implemented foundation

0.1 provided typed Jev decisions and bounded native preview/apply. 0.2 added
discovery, inspection, asset selection, diagnostics, measured blockouts and native
images. 0.3 extends the **inspect → edit → verify** loop for existing actors:

- Exact actor inspection with measured world bounds and reasons an edit is blocked.
- Selected-actor baselines and fresh field diffs.
- Explicit pass/fail/unverifiable checks for transforms, dimensions, ground height,
  spacing, material slots, labels and folders.
- Alignment, distribution, grid snapping and grounding on a specified plane.
- Existing material assignment and actor naming/folders in native Undo transactions.
- State-bound previews and retained plan outcomes after interrupted requests.
- CLI inspection/check files, capability diagnosis and an MCP workflow prompt.

See [workflows](WORKFLOWS.md), [spatial editing](SPATIAL_WORKFLOWS.md),
[verification](VERIFICATION.md) and [validation evidence](VALIDATION.md) for actual
support. These features operate on selected loaded actors; they do not certify a
whole game or a large user population.

## Next priorities

| Priority | Feature/workflow | Why it matters | Evidence required before calling it ready |
| --- | --- | --- | --- |
| Next | Native review panel | People can select actors, see before/after values, inspect plans and view results inside Unreal. | Usability tests with artists and designers; accessible keyboard navigation; state matches MCP. |
| Next | Guided install/update/repair | Detect engine/toolchain versions, plugin mismatches, port conflicts and connection failures without asking users to debug scripts. | Fresh-machine installation and upgrade tests; package provenance; clean removal. |
| Next | Native Data Validation adapter | Run a project's existing asset rules and retain authoritative errors instead of inventing new verdicts. | Explicit rule selection, side-effect classification, bounded jobs, real asset fixtures and cancellation. |
| Next | Blueprint inspection and compiler diagnostics | Explain nodes, references, variables and compile errors before proposing a repair. | Exact graph identity, read-only coverage, known error fixtures; no silent graph rewrites. |
| Next | Project-owned functional tests | Verify a door opens, a pawn reaches a target or a weapon deals damage after an edit. | Named test allowlist, setup/cleanup isolation, explicit pass/fail/timeout, real gameplay evidence. |
| Next | Held-out workflow benchmarks | Determine when Jev actually improves routing, cost and completion quality. | Direct-agent, keyword and Jev baselines; representative tasks; correction time, failures and total cost. |
| Later | Reviewed Blueprint edits | Add bounded graph operations with compile checks and before/after diffs. | Transaction recovery, pin/type validation and semantic tests; project-specific construction side effects considered. |
| Later | Material parameter workflows | Inspect instances, change exposed parameters, compare rendered results. | Known parameter types, parent/asset identity, dependency checks and visual regression tests. |
| Later | Mesh replacement and duplication | Replace placeholders or make repeated prop arrangements while preserving intentional settings. | Pivot/material/collision handling, attachments and undo/rollback acceptance. |
| Later | Terrain-aware placement | Trace to approved surfaces, align to normals and verify overlap rules. | Explicit collision channel/filter semantics, slopes, missing geometry, streamed worlds and physics tests. |
| Later | Asset dependency and import diagnostics | Identify broken references, import scale, missing textures, LOD and collision problems. | Source provenance, bounded dependency traversal and representative DCC imports. |
| Later | Lighting and camera workflows | Inspect lights, propose bounded adjustments, capture repeatable viewpoints and compare results. | Stable exposure/time/view settings; render comparisons and performance measurements. |
| Later | Navigation and accessibility checks | Evaluate project-defined widths, step heights, reachability and interaction rules. | Actual navmesh/pawn configuration and project-owned gameplay requirements. |
| Later | Performance investigations | Summarize measured frame, memory and rendering costs and track regressions. | Reproducible capture protocol; distinguish asset counts from measured bottlenecks. |
| Later | Animation/rig validation | Catch missing bones, incompatible skeletons, root-motion and retargeting problems. | Representative rigs, animation playback and export/import tests. |
| Later | UI workflow support | Inspect widget structure, input focus, text overflow and resolution variants. | Running viewport captures, input tests and localization/accessibility checks. |
| Later | Multi-editor/project support | Work with multiple games or engine versions without relying on one fixed port. | Explicit endpoint/project/session binding, editor selection, reconnection and cross-project isolation tests. |
| Later | Multi-agent coordination | Prevent agents from unknowingly editing the same scene while preserving human edits. | Scene leases, conflicts, cancellation and concurrency tests; no confidence-based permission. |
| Later | Durable recovery records | Investigate an apply after an MCP/editor crash or reconnect. | Native receipts with clear lifetime, storage privacy, exact identity and crash-injection tests. Current records are process-local. |
| Later | Team policy profiles | Use project-approved asset roots, validators, conventions and execution limits. | Auditable configuration, scoped permissions and understandable refusal messages. |
| Later | Build/cook/package jobs | Run named project workflows and report real artifacts and failures. | Bounded job lifecycle, output validation, cancellation, disk budgeting and platform testing. |
| Later | Blender handoff | Carry dimensions, IDs, provenance, materials and acceptance checks across Blender and Unreal. | Editable source retention, unit/pivot checks, round-trip fixtures and actual in-engine acceptance. |
| Later | Wider platform/engine packages | Reduce installation friction and reach users outside the current Windows UE target. | Licensed native build matrix, signed/versioned artifacts and runtime acceptance on each supported platform. |
| Later | Localization and task recipes | Make common workflows discoverable to beginners and non-English-speaking teams. | Reviewed translations, executable examples and user testing; terminology must match Unreal. |

## Build on Unreal's existing systems

Epic documents [Editor Utility Widgets](https://dev.epicgames.com/documentation/en-us/unreal-engine/editor-utility-widgets-in-unreal-engine)
as a way to add editor tabs. A review UI should use supported editor UI APIs and
the same typed operations as MCP, so human and AI workflows stay consistent.

[Data Validation](https://dev.epicgames.com/documentation/en-us/unreal-engine/data-validation-in-unreal-engine)
supports project-defined asset rules and command-line validation. An adapter should
report those rules' results explicitly; validators can execute project code, so
listing rules and executing approved rules are separate capabilities.

[Functional Testing](https://dev.epicgames.com/documentation/en-us/unreal-engine/functional-testing-in-unreal-engine)
provides project-authored setup, completion and cleanup. For packaged or multiplayer
sessions, [Gauntlet](https://dev.epicgames.com/documentation/en-us/unreal-engine/gauntlet-automation-framework-overview-in-unreal-engine)
is another existing foundation to evaluate. These are roadmap integration points;
Jev_Unreal does not currently expose arbitrary tests, validators or jobs.

## Where Jev belongs

Use Jev for ambiguous tool selection, candidate choice and diagnostic categories
over compact explicit evidence. Compute geometry, compare measurements and enforce
permissions deterministically. Keep direct tool use available. Publish mistakes
and abstentions alongside successes, and measure complete workflows before claiming
productivity improvements. None of the current evidence establishes readiness for
millions of users.
