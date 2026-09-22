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

Version 0.4 adds the following concrete surfaces. Each row identifies what is
implemented and what remains outside its acceptance evidence:

| Area | Implemented in 0.4 | Boundary and remaining acceptance |
| --- | --- | --- |
| [Human review](REVIEW_PANEL.md) | Window → Jev Review selection inspection, translation/metadata previews, native before/after records and one-shot application; MCP previews appear in the same panel. | Artist/designer usability, keyboard/accessibility and localization need representative users. This is not an automatic approval policy. |
| [Install/update/repair](SETUP.md) | Local diagnostics and reviewed source plans, exact file hashes, project enable/restore, retained backups, safe upgrade/removal and modified/untracked-file protection. | Disposable-project lifecycle tests are not fresh-machine acceptance. No engine/compiler download, automatic compilation or signed native packages. |
| [Data Validation](PROJECT_INSPECTION.md#selected-native-data-validation-rules) | Project-configured loaded native validators, exact asset selection, bounded jobs, native verdicts, retained diagnostics and cooperative cancellation. | Trusted project callbacks can have side effects. Arbitrary validators, Blueprint/Python discovery and global post-validation aggregation are not exposed. |
| [Blueprint inspection](PROJECT_INSPECTION.md#blueprint-inspection) | Loaded native Blueprint graph/node/pin identities, variable types, links and stored compiler diagnostics. | No graph editing, compile request, implicit Blueprint loading, widget/animation Blueprint support or proof that stored compiler status is current. |
| [Gameplay checks](FUNCTIONAL_TESTS.md) | Approved placed AFunctionalTest actors in an existing single standalone PIE session, state-bound start, native outcomes, timeout, cancellation and owned cleanup. | Requires project-authored tests. No PIE launch/stop, multiplayer, packaged build/device orchestration or generic guarantee that a game's mechanics work. |
| [Connection profiles](CONNECTION_PROFILES.md) | One explicit project/endpoint/token-file bundle per MCP server, configurable native loopback ports and fail-closed project isolation. | No runtime switching, endpoint scanning, multi-agent editing leases or certified engine-version matrix. |
| [Asset references/provenance](PROJECT_INSPECTION.md#dependency-and-import-provenance) | Paginated direct dependencies/referencers and recorded import basenames, timestamps and hashes without reading DCC source files. | Registry references do not prove dynamic runtime dependencies, import scale, texture completeness or successful round trips. |
| [Evaluation infrastructure](BENCHMARKS.md) | Frozen datasets/catalogs, separate answer keys, content hashes, keyword/Jev routing comparisons and human-attested direct-agent/workflow imports. | Public synthetic fixtures are not an untouched external study. No representative full-workflow productivity improvement has been established. |
| [Reconnect receipts](REVIEW_PANEL.md#outcomes-after-reconnecting-mcp) | Bounded native plan history survives an MCP reconnect while Unreal remains open; uncertain transport outcomes remain explicit. | Receipts are historical editor memory. They do not survive an editor crash, restore a scene or prove its current state. |

See [workflows](WORKFLOWS.md), [spatial editing](SPATIAL_WORKFLOWS.md),
[verification](VERIFICATION.md) and [validation evidence](VALIDATION.md) for actual
support and completed checks. These foundations do not complete this roadmap,
certify a whole game or establish support for a large user population.

Version 0.5 adds [reviewed mesh replacement and controlled prop copies](MESH_WORKFLOWS.md).
Replacement preserves actor placement/identity and requires an explicit material
policy. Copies carry a bounded set of native mesh settings; unsupported customized
properties are refused. Both use fresh inspection, state-bound one-shot previews,
Undo and fresh mesh/material/settings verification. General actor cloning,
attachment hierarchies, physics simulation, automatic pivot compensation and
representative game collision/play acceptance remain outside this release.

Version 0.6 improves the [native human review workflow](REVIEW_PANEL.md): all
supported operations have explicit Before/After fields, complete bounded mesh
detail is separately expandable, and inspection/review/results are selectable
and keyboard-focusable. Status polling preserves reviewed text and explicit
action outcomes; read failures and expiry disable Apply with recovery guidance.
Native key-event fixtures cover deliberate application and focus behavior.
Representative artist, physical-keyboard, screen-reader and translated-language
acceptance remain open; automation is not a substitute for those studies.

Version 0.7 implements the next engineering pass across all six immediate priorities:

| Priority | New implementation | Acceptance still required |
| --- | --- | --- |
| Review accessibility | Wrapped narrow-layout labels, names on focusable inputs, bounded explicit-action accessibility announcements and expanded-label rendered fixtures. | Artists/designers, physical keyboards, actual assistive software and reviewed translations. |
| Installation reliability | Write-ahead recovery receipts, exact hash-bound recovery plans, active-process exclusion and process-crash/locked-file regressions. | Clean Windows hosts and additional supported toolchains; no power-loss guarantee. |
| Validator compatibility | Explicit lifecycle/cleanup limits, native helper-result coverage, engine-rule fixtures, dirty transitions and queued-revision protection. | Representative project validators and shader-platform configuration. |
| Blueprint diagnostics | Loaded native Widget/Animation Blueprint inspection; allowlisted state-bound compile previews, one-shot compilation and retained fresh diagnostics. | Real project compiler extensions and subclass-specific runtime/visual acceptance. |
| Gameplay recipes | Project-owned door, interaction, combat and native-navigation recipes with setup, cleanup and deliberate regressions. | Adaptation to each game's mechanics; nav paths do not prove pawn movement or multiplayer. |
| Measured usefulness | Frozen trial protocols, counterbalanced method order, evidence-bound observations, paired comparisons and missing/failure accounting. | An independently evaluated representative study; no claimed speedup from harness tests. |

See [Blueprint workflows](BLUEPRINT_WORKFLOWS.md), [gameplay recipes](GAMEPLAY_RECIPES.md),
[setup recovery](SETUP.md), [study protocol](BENCHMARKS.md) and
[validation evidence](VALIDATION.md). Engineering coverage and external acceptance
are tracked separately so these features do not become unsupported readiness claims.
The [community acceptance protocol](COMMUNITY_ACCEPTANCE.md) provides concrete
observed tasks and clean-host/project matrices for collecting the remaining evidence.

## Next acceptance priorities and later features

| Priority | Feature/workflow | Why it matters | Evidence required before calling it ready |
| --- | --- | --- | --- |
| Next | Representative review-panel usability and accessibility | Validate the 0.6 review and keyboard foundations with artists/designers and assistive workflows. | Observed task completion, physical keyboard and screen-reader behavior, accessible errors and translated-language review. |
| Next | Fresh-machine install and upgrade acceptance | Turn the source installer into a demonstrated onboarding path across supported toolchains. | Clean Windows hosts, missing dependencies, Blueprint-only/C++ projects, previous releases, locked files and recovery after interruption. |
| Next | Real-project validator compatibility | Establish which common native rules work with the selected-rule adapter and how their side effects behave. | Representative project fixtures, documented unsupported shared/global state, cleanup and cancellation evidence. |
| Next | Broader Blueprint compile acceptance | Exercise the implemented Widget/Animation inspection and reviewed compiler workflow on representative project assets. | Subclass-specific runtime contracts, compiler extensions and known failing/working projects. |
| Next | Project adoption of gameplay recipes | Adapt the implemented door, navigation, interaction and combat examples to real project requirements. | Project-owned acceptance criteria, setup/teardown and reproducible negative cases. |
| Next | Independent real workflow study | Determine when Jev improves routing, completion quality, correction time and total cost using the implemented benchmark harness. | Separately held answer keys; direct-agent, keyword and Jev baselines; representative tasks; full-workflow evidence and all failures. |
| Later | Reviewed Blueprint edits | Add bounded graph operations with compile checks and before/after diffs. | Transaction recovery, pin/type validation and semantic tests; project-specific construction side effects considered. |
| Later | Material parameter workflows | Inspect instances, change exposed parameters, compare rendered results. | Known parameter types, parent/asset identity, dependency checks and visual regression tests. |
| Later | Broader mesh copy compatibility | Extend the implemented replacement/copy workflow to additional intentional actor settings and approved hierarchies. | Representative props, explicit attachment semantics, collision/play acceptance and further undo/rollback fixtures. |
| Later | Terrain-aware placement | Trace to approved surfaces, align to normals and verify overlap rules. | Explicit collision channel/filter semantics, slopes, missing geometry, streamed worlds and physics tests. |
| Later | DCC import and dependency diagnosis | Build on direct registry/provenance inspection to identify broken references, import scale, missing textures, LOD and collision problems. | Source availability, measured unit/pivot contracts, bounded traversal and representative DCC imports. |
| Later | Lighting and camera workflows | Inspect lights, propose bounded adjustments, capture repeatable viewpoints and compare results. | Stable exposure/time/view settings; render comparisons and performance measurements. |
| Later | Navigation and accessibility checks | Evaluate project-defined widths, step heights, reachability and interaction rules. | Actual navmesh/pawn configuration and project-owned gameplay requirements. |
| Later | Performance investigations | Summarize measured frame, memory and rendering costs and track regressions. | Reproducible capture protocol; distinguish asset counts from measured bottlenecks. |
| Later | Animation/rig validation | Catch missing bones, incompatible skeletons, root-motion and retargeting problems. | Representative rigs, animation playback and export/import tests. |
| Later | UI workflow support | Inspect widget structure, input focus, text overflow and resolution variants. | Running viewport captures, input tests and localization/accessibility checks. |
| Later | Editor compatibility and reconnect UX | Extend explicit profiles with clearer connection management across supported engine versions. | Real multiple-editor sessions, restart/version mismatch cases and licensed runtime acceptance for every advertised version. |
| Later | Multi-agent coordination | Prevent agents from unknowingly editing the same scene while preserving human edits. | Scene leases, conflicts, cancellation and concurrency tests; no confidence-based permission. |
| Later | Durable crash recovery | Investigate an apply after the editor or machine crashes. | Persistent receipts with privacy controls, exact identity, partial-write recovery and crash-injection tests. Current native receipts survive MCP reconnects only. |
| Later | Broader team policy profiles | Extend the existing validator/test allowlists to project-approved asset roots, conventions and execution limits. | Auditable configuration, scoped permissions and understandable refusal messages; policy remains independent of model confidence. |
| Later | Build/cook/package jobs | Run named project workflows and report real artifacts and failures. | Bounded job lifecycle, output validation, cancellation, disk budgeting and platform testing. |
| Later | Blender handoff | Carry dimensions, IDs, provenance, materials and acceptance checks across Blender and Unreal. | Editable source retention, unit/pivot checks, round-trip fixtures and actual in-engine acceptance. |
| Later | Wider platform/engine packages | Reduce installation friction and reach users outside the current Windows UE target. | Licensed native build matrix, signed/versioned artifacts and runtime acceptance on each supported platform. |
| Later | Localization and task recipes | Make common workflows discoverable to beginners and non-English-speaking teams. | Reviewed translations, executable examples and user testing; terminology must match Unreal. |

## Build on Unreal's existing systems

The current review panel uses native Slate and ToolMenus and shares the same typed
operations as MCP. Epic also documents
[Editor Utility Widgets](https://dev.epicgames.com/documentation/en-us/unreal-engine/editor-utility-widgets-in-unreal-engine)
as a way for projects to add editor tabs. Human and AI workflows should keep the
same validation, state checks and explicit execution boundaries.

[Data Validation](https://dev.epicgames.com/documentation/en-us/unreal-engine/data-validation-in-unreal-engine)
supports project-defined asset rules and command-line validation. The current
adapter reports selected native rules' results explicitly; validators can execute
project code, so listing rules and executing approved rules remain separate
capabilities. Global commandlet orchestration remains future work.

[Functional Testing](https://dev.epicgames.com/documentation/en-us/unreal-engine/functional-testing-in-unreal-engine)
provides project-authored setup, completion and cleanup. For packaged or multiplayer
sessions, [Gauntlet](https://dev.epicgames.com/documentation/en-us/unreal-engine/gauntlet-automation-framework-overview-in-unreal-engine)
is another existing foundation to evaluate. The current adapter covers named tests
in an existing standalone PIE world. Packaged/multiplayer orchestration remains
future work; arbitrary tests, validators or command execution are not exposed.

## Where Jev belongs

Use Jev for ambiguous tool selection, candidate choice and diagnostic categories
over compact explicit evidence. Compute geometry, compare measurements and enforce
permissions deterministically. Keep direct tool use available. Publish mistakes
and abstentions alongside successes, and measure complete workflows before claiming
productivity improvements. None of the current evidence establishes readiness for
millions of users.
