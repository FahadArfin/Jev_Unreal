# Toward broad adoption

The goal is dependable everyday Unreal work for people and AI clients. A large
audience needs clear installation, compatible versions, understandable failures,
recovery and evidence of useful results. More commands alone do not establish any
of those outcomes. This is a prioritized product roadmap, not a support promise.

## Version 0.10: current implementation

Python 0.10.0a1 and native bridge 0.10.0 implement the ten requested improvements
within the scopes below. The complete catalog has 91 tools; core has eight.
Implementation status and acceptance evidence are separate: use the dated
[validation record](VALIDATION.md) for actual completed checks and
[compatibility matrix](COMPATIBILITY.md) for supported targets.

| # | Improvement | Implemented scope | Boundary and next evidence |
| --- | --- | --- | --- |
| 1 | [Real agent benchmark adapter](BENCHMARK_AGENT.md) | Fresh ephemeral Codex threads, three actual native read tools, off/on routing hooks, independent measured-answer verification, state/configuration hashes and bounded reports. | One off-only read smoke passed. Latest OpenRouter authentication returned 401; no paired benefit established. Editing, ambiguity and recovery workloads still need expansion and independent measurement. |
| 2 | [Native compact reads](COMPACT_WORKFLOWS.md) | Selected fields are projected inside Unreal; unrequested material/LOD arrays and mesh settings are skipped. Frozen bounded transport pages preserve identity, blockers and truncation; older bridges retain fallback. | Full bounded capture is gathered for honest deltas. Identity/revision work still runs; smaller bytes do not prove lower latency or billed tokens. |
| 3 | [Durable task recipes](WORKFLOW_RUNS.md) | Spatial/layout inspect-preview-review-apply-verify runs, durable one-shot claims, undispatched cancellation and observational reconciliation after reconnect. | Uncertain dispatch is never resubmitted. This is checkpoint history, not automatic rollback, map saving or universal task execution. |
| 4 | [Setup and connection assistant](DOCTOR.md) | One doctor report distinguishes exact editor identity, native capabilities, provider credential sources and actionable repair steps. | Authentication needs a separate explicit probe. Independent clean Windows installation/upgrade evidence remains open. |
| 5 | [Gameplay acceptance](RUNTIME_GAMEPLAY.md) | Reviewed project-approved standalone PIE start/stop, native ownership/receipts, actual game viewport capture and integration with named functional tests. | One local in-process client and approved maps only. Representative project tests, packaged sessions, Gauntlet and multiplayer remain outside scope. |
| 6 | [Useful Blueprint edits](RUNTIME_GAMEPLAY.md#trigger-opens-a-door) | Approved Actor events, primitive local variables/get/set, branches and three self-only actor calls, with existing preview/compile/Undo and a door overlap fixture. | Fixed vocabulary only; no arbitrary calls, casts, object links or full game generation. Real project semantics and visual acceptance remain necessary. |
| 7 | [Groups without reconnecting](COMPACT_WORKFLOWS.md#advertise-a-smaller-tool-catalog) | Explicit session-local activation sends actual MCP list-change notifications. Other sessions retain their catalog; failed notification restores previous groups. | Requires a compatible client to opt in; server cannot prove UI refresh. Startup configuration and reconnect remain the fallback. Groups grant no permissions. |
| 8 | [Version-control-aware checkpoints](CHECKPOINTS.md) | Approved-file disk hash manifests and comparisons; fixed pinned Git/Perforce status, scoped paths and bounded output. | Unsaved buffers are excluded. Manifests are not backups, and tools never automatically stage, restore or check out. Real Perforce acceptance remains untested. |
| 9 | [Blender round trip](HANDOFF_IMPORT.md) | Retained editable source and measured export contract, reviewed alias-based native FBX import/reimport, and fresh dimension/pivot/material-slot verification. | Static meshes only; no texture/material import, automatic save or general Blender executor. Representative asset appearance/collision and source updates still need acceptance. |
| 10 | [Release and adoption validation](COMPATIBILITY.md) | Explicit platform/client/workflow matrix and a reproducible [community acceptance procedure](COMMUNITY_ACCEPTANCE.md), covering setup, reconnect, reviewed workflows and upgrade/removal. | A published procedure is not independent user evidence. Clean hosts, accessibility, translation review and broad project adoption remain open. |

The next work is deeper acceptance of these implemented paths: resolve provider
access before a paired experiment, add measured edit/recovery tasks, validate
clean-host onboarding and representative project assets, and test supported
clients/services independently. More advertised tools alone are not the goal.

## Historical implementation stages

The following version tables describe what existed and remained open **at those
versions**. Some of their limits were addressed later. The 0.10 table above is
the current implementation map; do not interpret historical rows as today's backlog.

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

## Version 0.8: next ten engineering slices

The next ten engineering rows were implemented in roadmap order. The supported
scope is deliberately explicit; the remaining acceptance column is not closed by
the existence of an API. See [domain workflows](DOMAIN_WORKFLOWS.md) for examples,
policy, inputs, failure handling and evidence limits.

| Order | Priority | Implemented scope | Remaining expansion and acceptance |
| --- | --- | --- | --- |
| 1 | Reviewed Blueprint edits | Allowlisted native math-input literals, typed pin checks, preview/diff, one-shot edit/compile, Undo and receipts. | Broader nodes/links, semantic project tests, compiler-extension side effects and transaction recovery. |
| 2 | Material parameters | Loaded MIC scalar/vector discovery, exact project policy, reviewed global overrides, readback/Undo and rendered color fixture. | Layers, static switches, textures, representative parent chains and broader visual regressions. |
| 3 | Mesh copy compatibility | Decal reception, custom depth/stencil and translucent sorting join the existing explicit copy/readback state. | Approved attachment hierarchies, more intentional settings, real prop/collision/play acceptance. |
| 4 | Terrain placement | Approved first-hit collision surfaces, bounded slopes, normal alignment, supported bounds and overlap-refusing scene previews. | Multiple support samples, complex/concave collision, streaming and physics acceptance. |
| 5 | DCC/dependency diagnosis | Bounded registry traversal, mesh bounds/LODs/collision/material findings and recorded import basenames/hashes. | Actual DCC source availability, unit/pivot intent, texture completeness and import round trips. |
| 6 | Lights/camera | Native intensity/color review and Undo; inspected/restorable perspective poses/FOV, native capture and rendered fixture. | Exposure/time/render-setting controls, lighting bake and measured performance acceptance. |
| 7 | Navigation/accessibility | Existing Recast endpoint projection/path length, agent width/step comparisons, explicit build/staleness limits. | Actual corridor/step geometry, controller movement, interactions and project accessibility requirements. |
| 8 | Performance investigations | Bounded editor ticker/memory capture, cancellation, complete paired receipt comparison and regression thresholds. | Frame-synchronized render/GPU attribution, representative workloads and repeated statistical evidence. |
| 9 | Animation/rig validation | Stored hierarchy, required bones, exact skeleton compatibility, sequence duration and root-motion flag. | Real rigs, playback, extracted motion, skin weights, retargeting and export/import acceptance. |
| 10 | UI workflow support | Bounded stored widget tree, native focus/text/overflow/canvas diagnostics without binding execution or runtime construction. | Running viewport geometry, DPI/resolution variants, input, localization and assistive-user acceptance. |

The 0.8 catalog had **54 tools**. No additional provider request or measured
game-development speedup is claimed. Native, rendered, Python and live bridge
evidence is tracked separately in [validation](VALIDATION.md).

## Version 0.9: implementation pass

Version 0.9 implements another bounded engineering pass across every remaining
roadmap area that can be developed on the current licensed Windows host. The
catalog has **66 tools**. Features, automation and independent acceptance remain
separate: an API or local test does not close a human study or a platform matrix.

| Area | Implemented in 0.9 | Work still required |
| --- | --- | --- |
| Review and onboarding | Draft French/Spanish panel catalogs, native culture-switch fixture, eight beginner recipes in three languages, compiler/linker/SDK diagnostics and localization-aware reviewed installation. | Representative artists, physical keyboards, screen readers, human translation review and truly clean Windows hosts. |
| Acceptance evidence | Hash-bound reports for all six immediate priorities, missing/failure accounting, required clean-host/project cells and linked paired-study scoring. | Actual independent participants, real-project evidence and external study collection; attestations are not independent verification. |
| Blueprint editing | Separately approved add/remove pure math nodes and exact-type connect/disconnect operations, complete graph identity, cycle/implicit-break refusal, compile/Undo and specialized Widget/Animation compile fixtures. | Broader node families, compiler extensions and semantic production-project tests. |
| Materials | Approved Texture2D and static-switch overrides plus existing exposed global/layer/blend parameter associations, parent identity checks and Undo. | Structural layer-stack editing, representative parents and larger visual regression sets. |
| Mesh hierarchies | Explicit complete native mesh forests, preserved copied-parent relations, fresh local-pose verification, stale attachment checks and partial-failure rollback. | Skeletal/Blueprint actors, sockets, partial hierarchy cloning and representative prop/gameplay acceptance. |
| Terrain | 1/5/9 support samples, complex traces, uneven-support limits, highest-support placement and streaming-state refusal. | Representative landscapes, concave surfaces, world partition and physics configurations. |
| DCC and Blender | Editable `.blend` retention, bounded static-mesh exporter, hashed manifests, texture availability, dimension/pivot/slot contracts and fresh native comparison. | Broader authored asset round trips, rendered material equivalence, rigged assets and import-policy UI. |
| Lighting/camera | Reviewed exposure, fixed EV, lit/unlit, realtime and motion-blur controls with restore checks. | Time-of-day control, bake orchestration and deterministic rendered comparisons under project settings. |
| Navigation/gameplay | Capsule corridor/support geometry probes plus an actual CharacterMovement traversal recipe with blocked-path negative case and owned cleanup. | Game-specific controllers, interactions, accessibility rules, multiplayer and packaged sessions. |
| Performance | Engine-published game/render/RHI/GPU timing distributions, distinct from editor ticker intervals, explicit missing data and `frame_aligned: false`. | Frame-correlated attribution, representative workloads, GPU tooling and repeatable statistical comparisons. |
| Rig/animation | Bounded extracted root-motion sampling and optional available CPU skin-weight diagnosis. | Representative rigs, playback, retargeting, export/import and deforming-mesh visual acceptance. |
| Runtime UI | Inspection of an exact already-running approved Widget instance, cached geometry/DPI/focus/overflow hints and two rendered allocation fixtures. | Physical input, display-DPI/resolution matrices, localized text and assistive-user acceptance. |
| Connections | Explicit-profile live health/version checks, independent project identity and no endpoint scanning or implicit switching. | Licensed engine/version/platform acceptance for each advertised configuration. |
| Team coordination/policies | Opt-in exact-project action/root/alias policies, atomic cross-process cooperative leases, renewal, conflicts and one-shot policy-bound preview authorization. | Larger team trials; humans and unconfigured clients remain outside cooperative leases. |
| Crash investigation | Private bounded SQLite metadata receipts, pre-dispatch durable writes, retention/forget, explicit uncertain outcomes and process-crash fixtures. | Scene restoration, machine power-loss testing and broader recovery usability. Lost responses are never automatically replayed. |
| Build/cook/package | Named fixed Win64 jobs, pinned engine entrypoints and bundled .NET, reviewed one-shot launch, process-tree containment, cancellation and output/artifact budgets. | Project-specific build/cook/package acceptance, other target platforms and packaged gameplay tests. |
| Wider distribution | Updated cross-platform Python CI and explicit installed-engine/toolchain diagnostics; source distribution contains no proprietary engine code/binaries. | Licensed Linux/macOS/other-engine builders, native runtime matrices, signing and supported binary packages. |

Guides: [editing](EDITING_EXTENSIONS.md), [advanced inspection](ADVANCED_INSPECTIONS.md),
[team workflows](TEAM_WORKFLOWS.md), [DCC handoff](DCC_HANDOFF.md),
[localization/recipes](LOCALIZATION_RECIPES.md), [acceptance reports](ACCEPTANCE_REPORTS.md).
The [validation record](VALIDATION.md) records the actual completed checks.

## Historical MCP efficiency and acceptance pass

That MCP-only development pass implemented all six immediate improvements below.
Its catalog contained **75 tools**; selecting `core` exposed
seven. Those changes used the existing 0.9 native bridge and preserved its execution
and project-policy boundaries.

| Improvement | Implemented behavior | Remaining evidence or limits |
| --- | --- | --- |
| Compact responses | Explicit field projections, native safety metadata, frozen pages and fresh selected-field deltas with bounded expiring receipts. | Native reads still fetch full bounded metadata; pagination does not extend the native scan cap. No billed-token or workflow-speed claim. |
| Selective routing | Explicit tool or sole candidate resolves locally; ambiguous cases defer unless cloud advice is requested. Outcomes retain request/cache/failure accounting. | No learned routing threshold or confidence-based permission. Representative routing-on/off trials remain required. |
| Provider health | Saved-key presence, dated authentication observation and latest request failure are separate; explicit synthetic probe bypasses cache once. | No automatic probe or claim that historical authentication guarantees current access. Live provider evaluation remains a separate action. |
| Smaller tool groups | Startup-scoped advertised/callable catalogs, compact group membership, one-schema discovery and exact active catalog hash/byte count. | Clients must reconnect after environment changes. Group selection is context management, not authorization. |
| Paired workflow benchmarks | Two-arm counterbalanced runner with fresh adapter contexts, frozen identities, budgets, verification, failure/missing accounting and nullable timing/token/cost telemetry; CLI demo/template/report. | The runnable demo uses fake tools. A real agent adapter or independently collected observations are required for a real experiment. |
| Visual/gameplay acceptance | Identity/camera-bound native images, bounded pixel comparison, explicitly attributed visual review, approved playtest start/poll and post-test evidence. | Pixels do not prove visual quality; editor captures are not PIE frames. Representative project gameplay and human visual acceptance remain open. |

Guides: [compact reads and groups](COMPACT_WORKFLOWS.md),
[routing and health](ROUTING_HEALTH.md), [paired benchmarks](PAIRED_BENCHMARKS.md),
[acceptance workflows](ACCEPTANCE_WORKFLOWS.md). See [validation](VALIDATION.md)
for separate mock, native integration and visual inspection evidence.

The next evidence priorities are real model-specific paired trials when provider
access is available, representative approved playtests, and clean-host/client
reconnection checks. Feature breadth alone does not establish readiness for a
large user population.

## Remaining acceptance and broader scope

These are evidence gaps or extensions beyond the implemented 0.10 scopes.
They are not requests to rebuild the features above.

| Priority | Next acceptance work | Evidence required |
| --- | --- | --- |
| First | Complete the paired agent experiment once provider authentication succeeds | Same measured starting state, model/configuration, exact tools, verified outcomes and all failures. Expand beyond the two public asset reads before drawing workflow conclusions. |
| First | Independent clean-host installation and upgrade | New Windows hosts/toolchains, Blueprint-only and C++ projects, missing dependencies, older releases, locked files, interrupted installation and preserved user changes. |
| First | Representative recipe and gameplay adoption | Project-owned positive/negative tests, reconnects during uncertain outcomes, collision/interaction semantics and explicit cleanup. Separate compilation, runtime and visual acceptance. |
| First | Client and source-control compatibility | Independent MCP list-refresh/reconnect checks, real Perforce authentication/server behavior and exact-file Git results in representative repository layouts. |
| Next | Broader artist and accessibility acceptance | Observed tasks, physical keyboard and screen-reader behavior, usable error recovery and reviewed translations. |
| Next | Representative Blender assets and Blueprint/compiler extensions | Retained source updates, measured reimports, appearance/material/collision review, specialized project classes and callback side effects. |
| Later | Packaged and multiplayer acceptance | Explicit Gauntlet/project test contracts, server/client lifecycle, remote-device ownership and reproducible failure cleanup. Existing named packaging jobs do not supply this evidence. |
| Later | Wider native platforms and engine versions | Licensed builds and runtime/visual tests on each claimed platform/version; Python portability alone is insufficient. |

Shared multiuser hosting and a general Blender executor require a separately
reviewed design. No automated crash rollback or automatic replay of uncertain
operations is planned through the authenticated bridge.

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
