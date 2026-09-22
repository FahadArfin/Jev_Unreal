# Validation evidence

## Current 0.6 native review workflow acceptance

The 0.6 alpha improves the human review workflow while retaining the same 40 MCP
tools and execution contracts. Tests used the exact isolated JevSandbox on
Windows 11 / licensed Unreal **5.8.2**. No representative artist, physical keyboard,
screen-reader, localization or production-readiness claim follows from these fixtures.

| Layer | Result | Scope |
| --- | --- | --- |
| Python | **1,273 passed**, Python 3.13.2, **50.15 seconds** | Existing primarily synthetic contract suite; Python changes are version and live smoke-script updates. |
| Dependency/lint/package | Locked extras sync, Ruff and wheel/source build passed | Source distributions include native source, not licensed engine/editor binaries. |
| Licensed native build | Compiled and linked successfully, **6.56 seconds** on final incremental build | Installed compiler preference and engine-header warnings remain; this is not an install or performance benchmark. |
| Native automation | **27/27 suites passed**, 25 clean and two warning-bearing suites | Three existing empty-bounds/typed-element warning entries; zero failed/skipped/incomplete suites. |
| Rendered automation | **2/2 suites passed**, zero warnings | Native Slate event routing and captures; separate from headless tests and representative user acceptance. |
| Live MCP/editor | **40 tools**, baseline editor smoke and mesh workflows passed | Exact authenticated sandbox, fresh mesh checks, unchanged-source/new-identity checks, stale/replay refusal and native captures. |
| Saved launcher | Connected to bridge **0.6.0**, discovered 40 tools | Existing encrypted key available; **zero provider requests**. |

The final native report is timestamped **`2026.09.22-04.57.09`**, and the rendered
report **`2026.09.22-04.57.45`**. New `Jev.Editor.ReviewPresentation` and
`Jev.Editor.ReviewRecovery` suites cover all seven supported operation types,
both mesh policies, exact small numeric changes, malformed/incomplete/oversized
records, and recovery messages that retain uncertainty about an attempted apply.

`Jev.Rendered.ReviewWorkflow` routes actual Slate Tab/Shift+Tab, text-entry, Enter
and Space events in a private native test window. It enters a 25 cm translation,
verifies preview immutability and copyable read-only text, proves Enter in review
does not execute, then deliberately focuses Apply and confirms one actual actor
move. It checks fresh inspection, expiry, selection errors, pending mesh review,
keyboard access to the collapsed technical detail, and selection/focus stability
through normal two-second refreshes. Advancing the receipt clock past 15-minute
retention verifies that a failed passive lookup preserves the last action result,
its selected text and focus while separately disabling Apply.

The initial rendered check originally expected the offscreen Apply widget to
already have arranged geometry. It now scrolls that control into view, waits for
normal Slate layout, and checks its visible geometry and disabled state. The
workflow test passed before that fixture correction; failed runs were not counted
as final acceptance. Code review separately found and corrected passive refresh
errors overwriting explicit outcomes. The final regression covers that correction.

Native images were viewed: the [initial layout](images/review-panel-v0.6.png),
[scrolled action area](images/review-panel-v0.6-actions.png),
[selected transform text](images/review-panel-v0.6-transform.png),
[mesh review](images/review-panel-v0.6-mesh.png) and
[error guidance](images/review-panel-v0.6-error.png) are readable in the captured
fixture sizes. The three text images are direct native widget captures, not edited
screenshots. Full populated-window captures remain local because they include the
machine's project path. Scrolling is required for longer content; these images do
not establish every tab size, display scale, language or assistive configuration.

Local raw evidence is ignored under `artifacts/review-pytest.xml`,
`review-native-build.log`, `unreal-automation/`, `unreal-rendered/`,
`editor-smoke-v0.6.json`, `mesh-workflows-v0.6.json` and
`saved-launcher-v0.6.json`, plus native PNGs under the sandbox's `Saved/Automation`.
No smoke requested a save or edited another game. This milestone made no provider
calls or new Jev accuracy/cost measurement. Installer, multiple-editor, independent
workflow and broader platform acceptance remain historical or open as identified
below. See [native evidence](UNREAL_VALIDATION.md) and [review use](REVIEW_PANEL.md).

## Historical 0.5 mesh workflow acceptance

The 0.5 alpha adds reviewed mesh replacement and controlled prop copies. Validation
uses only the repository's isolated JevSandbox, public engine meshes and synthetic
fixtures on Windows 11 / Unreal **5.8.2**. It does not establish broad project
compatibility, gameplay correctness, user productivity or production readiness.

The full Python suite passed **1,273 tests** on Python 3.13.2 in **47.64 seconds**.
Locked extras sync, Ruff and wheel/source builds passed. The new cases cover strict
recipes, old-plugin capability refusal, complete mesh state, collision inheritance,
material overrides, actor identities, stale measurements, 20-source/40-check
workflows and malformed readback. Missing observations remain unverifiable.
These are primarily injected/synthetic contract tests; native evidence is separate.

The licensed native build passed on UE **5.8.2**. All **25 `Jev.Editor` suites
passed: 23 clean and two with three warning entries, with zero failed, skipped or
incomplete suites. The final report is timestamped **`2026.09.22-04.28.42`**.
The warnings are the existing empty-bounds and typed-element fixture warnings.
Four new suites exercise replacement, controlled copies, guards and rollback,
including true/false collision inheritance, customized settings, Undo, actor
destruction callbacks and an actual isolated editor-world context switch.
The changed-world case reports `rollback_failed`/unknown without undoing a
different world; the fixture restores its context and explicitly cleans up.

The separate rendered **`Jev.Rendered.ReviewPanel` test passed 1/1**, zero warnings,
at **`2026.09.22-04.29.18`**. Its fresh [panel image](images/review-panel-v0.5.png)
was viewed: the initial controls fit and are legible, with Apply disabled until a
plan is reviewed. Mesh before/after text is covered by native controller tests;
this empty-panel capture does not establish mesh-review usability or accessibility.

Real official-SDK stdio acceptance discovered **40 tools**. The mesh workflow
exercised both replacement policies, explicit material assignments, a controlled
copy, six fresh checks, unchanged source records, different new actor identity,
stale source material/transform rejection and one-shot replay rejection. Four
native viewport captures were inspected: the replacement retained its white
material; mesh defaults restored the checker material; the copy image shows two
separate props. Geometry/readback and image observations do not simulate collision.

Published captures show the [original prop](images/mesh-v0.5-before.png),
[retained-material replacement](images/mesh-v0.5-preserve-slots.png),
[mesh defaults](images/mesh-v0.5-mesh-defaults.png) and
[controlled copy](images/mesh-v0.5-duplicated.png).

The existing blockout, measured-edit and roadmap/reconnect smoke paths also passed.
The encrypted-key launcher independently discovered 40 tools and authenticated the
0.5 sandbox editor; the stored provider key was available and zero provider
requests were made. This release makes no new live Jev accuracy or cost claim.

The live test initially exposed that Unreal's profile setter disables mesh-default
collision inheritance. The implementation now retains that flag explicitly and
refuses incompatible inherited defaults during replacement preview. A separate
fixture issue selected a material equal to the mesh default; the smoke now uses a
distinct inspected engine material. Failures were diagnosed rather than counted
as passes. No smoke requested a map save or edited another game.

Raw local evidence is ignored under `artifacts/mesh-pytest.xml`,
`mesh-native-build.log`, `unreal-automation/`, `unreal-rendered/`,
`mesh-workflows-v0.5.json`, `editor-smoke-v0.5.json`,
`verified-workflows-v0.5.json`, `roadmap-smoke-v0.5.json` and
`saved-launcher-v0.5.json`. Native details are in [Unreal validation](UNREAL_VALIDATION.md).
The two-editor, installer lifecycle and provider benchmark evidence below remains
historical 0.4 evidence; those layers were not repeated for 0.5. Fresh-machine,
representative artist/accessibility and wider engine/platform acceptance remain open.

## Historical 0.4.0a2 client pacing correction

The first 0.4 release's feature and pull-request matrices passed, but a subsequent
Windows Python 3.12 [main run](https://github.com/FahadArfin/Jev_Unreal/actions/runs/35684098069)
failed the real-time request-spacing assertion once. The same commit's release-tag
matrix passed. This intermittent failure was investigated and corrected rather
than hidden by rerunning the failed job.

The client now rechecks its monotonic deadline after an early timer wakeup and
starts the next 50 ms interval after HTTP response cleanup, including failures.
Slow transport time no longer consumes that interval. Calls remain serialized;
cancellation propagates and no request is retried. This deliberately favors
predictable pacing over maximum throughput.

Nine deterministic timing cases exercise early wakeups, slow response/close,
HTTP/transport/body failures and cancellation during waiting/dispatch/body/close.
The **85-test bridge suite passed on installed Windows Python 3.12 and 3.13**.
The full **1,167-test suite passed on Python 3.13.2 in 47.08 seconds**, followed by
successful locked sync, Ruff and wheel/source checks (raw report:
`artifacts/roadmap-pytest-alpha2.xml`). The real 39-tool roadmap/reconnect smoke was rerun successfully
against the existing 0.4 native editor, with zero provider calls and no map save.

This patch changes Python only. The native build, 21-suite, rendered panel,
two-editor, installation and provider evidence below belongs to the unchanged
0.4 native feature milestone; those layers are not presented as newly rerun.

## 0.4 roadmap milestone acceptance

Release **0.4.0a1**, validated on **2026-09-22 UTC**. The nine implemented
foundations in the [roadmap](ROADMAP.md) are bounded alpha features, not completion
of every roadmap item or certification for broad production use.

| Layer | Observed result | Scope |
| --- | --- | --- |
| Python | **1,158 passed**, Python 3.13.2, 50.52 seconds | Contract, protocol, lifecycle, installation, profiles, compatibility and benchmark regressions; primarily synthetic fixtures |
| Dependencies/lint/package | Locked extras sync, Ruff and `uv build` passed | Python wheel/source distributions; no PyPI publication |
| Licensed native builds | Both isolated projects compiled and linked on UE **5.8.2** | Windows 11; compiler 14.51 preference/deprecation warnings remain |
| Native automation | **21/21 suites passed**, 19 clean and two warning-bearing suites | Three warning entries, zero failed/skipped/incomplete suites; includes actual isolated PIE functional fixtures |
| Rendered panel | **1/1 `Jev.Rendered.ReviewPanel` passed**, zero warnings | Real Slate tab, ten discoverable controls, disabled initial apply and native panel PNG; not representative accessibility/usability acceptance |
| Real MCP/editor | **39 tools**, baseline, measured-edit and roadmap/reconnect smoke passed | Native authenticated sandbox operations, four camera captures and fresh verification |
| Two editor connections | Both 39-tool MCP sessions passed isolation checks | Separate projects/tokens/ports; crossed plans rejected, own plans applied once, mismatched project refused |
| Native validation over MCP | Localization rule returned `valid`; material rule returned `not_validated` for the engine cube | Two explicitly approved native rules; no claim about arbitrary project validators |
| Installer lifecycle | Reviewed install, upgrade, native build, live connection and source removal passed | Disposable project on this configured machine; generated files and backups retained, original plugin entry restored |
| Saved Windows launcher | Fresh stdio initialization discovered 39 tools and verified bridge 0.4/project identity | Existing encrypted-key launcher; zero provider requests |
| Live routing benchmark | Jev **8/8**, keyword **7/8**, eight held-out-partition public synthetic cases | One tiny public dataset/run; no direct-agent or completed-workflow comparison |

The native report is timestamped **`2026.09.22-03.35.34`**; the rendered report is
**`2026.09.22-03.31.06`**. [Detailed native evidence](UNREAL_VALIDATION.md) separates
fixture automation, live transport and visual acceptance. The [review panel image](images/review-panel-v0.4.png)
was viewed: all controls fit and their labels are legible. The new
[isometric](images/verified-workflow-v0.4-isometric.png) and
[top](images/verified-workflow-v0.4-top.png) viewport images show the three measured
test actors. This does not establish finished art, gameplay or broad panel usability.

The measured workflow re-exercised exact actor identities, snapshots/diffs,
ground/alignment/distribution/grid edits, material and metadata changes, stale-state
and one-shot rejection, deliberate verification failures and four native camera
captures. The reconnect smoke restarted the Python MCP process, recovered an
`applied` receipt from the still-running editor and independently verified its
actor's label. Neither test used a model or saved a map.

Two simultaneous editors used ports 9845/9846 and distinct profiles. Deliberately
conflicting legacy connection variables could not override the selected profile.
Cross-editor plans changed neither scene; the correct plans each added exactly one
unsaved cube and passed fresh label/instance checks. Native validator receipts
distinguished a valid result from an inapplicable rule; the inspected cube package
stayed clean. Validator side effects in general remain outside a sandbox guarantee.

Review also found and corrected a preflight race: validation start now carries the
authenticated project, session, world and revision to the native queue boundary.
A changed editor rejects the request before callbacks run. Native and synthetic
regressions cover mismatched identities and malformed state. Doctor now reports
the complete 0.4 capability set, rather than declaring an older 0.3 plugin ready.

The [sanitized routing report](benchmarks/2026-09-22-public-synthetic-v1.json)
records the frozen dataset and answer-key hashes. Jev's eight requests averaged
**234.95 ms** and reported **USD 0.00026061** total cost. This requested
`typesafe/jev-1.13`; the returned resolved model identifier was not retained.
The keyword baseline averaged 0.10 ms. These figures demonstrate the harness and
current provider wiring, not game-development savings or independent accuracy.
See [benchmark methods and limits](BENCHMARKS.md).

Raw reports remain ignored locally: `artifacts/roadmap-pytest-release.xml`,
`artifacts/unreal-automation/`, `artifacts/unreal-rendered/`,
`artifacts/editor-smoke-v0.4.json`, `artifacts/verified-workflows-v0.4.json`,
`artifacts/roadmap-smoke-v0.4.json`, `artifacts/connections-smoke-v0.4.json`,
`artifacts/install-acceptance/summary.json` and `artifacts/saved-launcher-v0.4.json`.
Published evidence contains no credentials, private game data or engine binaries.
An already-running Codex MCP connection may require reconnect/restart to discover
the expanded tool catalog; fresh launcher acceptance does not refresh that client.

## Historical 0.3 acceptance

Release **0.3.0a1**, validated on **2026-09-22 UTC** (2026-09-21 US Eastern). This is a tested alpha, not a production-readiness certificate. Synthetic tests, live provider calls, engine tests and visual review establish different things.

## Inspect, edit and verify acceptance

| Layer | 0.3 result | Scope |
| --- | --- | --- |
| Python | **865 passed**, Python 3.13.2, 31.34 seconds | Strict contracts, geometry, instance-aware evidence, bounded snapshots/records, ambiguous outcomes, capability compatibility and real stdio protocol; mostly synthetic fixtures |
| Dependencies/lint/package | Locked sync, Ruff and `uv build` passed | Builds Python wheel/source; no PyPI publication |
| UE native | Build succeeded; **14 suites passed**, one expected empty-bounds navigation warning | UE 5.8.2 Windows; includes partial-failure and no-op Undo/Redo preservation regressions |
| Live MCP/editor | **27 tools**, baseline and verified-edit smoke passed | Real authenticated rendered sandbox, unsaved actor edits and native images |
| Saved client launcher | 27 tools and correct 0.3 sandbox identity | Existing Windows encrypted-key launcher, zero provider calls |
| Live Jev | Three responses, four authored labels matched | Public synthetic helper compatibility; not accuracy or productivity evidence |

Review caught and fixed an important Undo edge case before acceptance: Unreal can
discard a no-op transaction, so rollback must match the exact Jev transaction GUID
before undoing anything. Further regression tests prevent empty apply expectations
from passing, check primitive mesh/label readback, and preserve actionable errors
when the installed native plugin lacks a capability.

The real stdio MCP client discovered **27 tools** against the rendered UE **5.8.2**
sandbox, with bridge **0.3.0**. `scripts/smoke_verified_workflows.py` exercised three
rotated, differently scaled native cubes through the complete workflow:

- Exact actor bounds and live instance identity, retained snapshot, unchanged diff,
  then a changed diff after the first edit.
- Five measured previews/applies: ground, align a bounds edge, distribute equal
  bounds gaps, snap pivots to a grid, and ground again. All generated fresh checks
  passed; preview did not change actor state.
- Old inspection revision and wrong session rejected before preview; each apply
  was one-shot, and retained plan records reflected the observed outcome.
- Batch label/folder changes, an existing material assignment, native readback and
  fresh checks against the same actor instances.
- A deliberately wrong label returned `failed`; a wrong instance or missing actor
  returned `unverifiable`, never a false pass.
- Isometric, top, front and right perspective camera presets followed by four
  **1014 × 479** native captures with matching camera metadata.

The [isometric capture](images/verified-workflow-isometric.png) and
[top capture](images/verified-workflow-top.png) were viewed and show all three
rotated test objects. Front and right captures were also viewed; the right view
occludes objects along the row, as expected from that camera direction. A camera
preset is not a visibility or collision guarantee. Grounding uses the specified
Z plane; there is no terrain trace or simulated contact test.

The baseline `scripts/smoke_editor.py --require-capture` also passed against 0.3,
including auth/origin rejection, project binding, stale plans, mesh placement,
grid/stairs/room layouts and rendered capture. Test actors remain unsaved in the
isolated sandbox. These tests make no provider requests and modify no other game.
Local evidence: `artifacts/editor-smoke-v0.3.json`,
`artifacts/verified-workflows-v0.3.json`, native automation reports and build logs.
The [Unreal evidence](UNREAL_VALIDATION.md) describes native rollback tests and limits.

## Historical 0.2 baseline

The following table and original measurements record the previous release. They
are retained as historical evidence, not substituted for the 0.3 acceptance above.

| Layer | Result | What it establishes |
| --- | --- | --- |
| Python suite | 513 passed locally on Python 3.13.2 in 28.46 seconds | Wire contracts, bounded discovery, strict inputs, credentials, local ranking/grouping, layout geometry, readback, PNG validation and MCP integration; synthetic transports are not live accuracy evidence |
| Dependency and lint checks | `uv sync --locked --all-extras` and Ruff passed | Locked dependency installation and configured static checks |
| Packaging | `uv build` produced wheel and source distribution | Python package can be constructed; not a public PyPI publication |
| PowerShell scripts | Syntax checked; DPAPI and launcher credential flow tested with synthetic values | Local credential handling without plaintext arguments |
| UE C++ build | Passed on installed UE 5.8.2 | Editor module compiles/links; compiler preference warning documented separately |
| Native Unreal automation | 9 passed, zero failures/warnings/skipped/incomplete tests | Scene edits/Undo, stale identities, context, mesh inspection/placement, warnings, capture and framing boundaries |
| Real MCP -> HTTP -> Unreal | Passed, 21 tools discovered | Authenticated sandbox operations, existing mesh placement and all three layouts with verified native readback |
| Rendered viewport and visual review | Framing and fresh native PNG capture passed; four-step staircase viewed | One blockout was visible in the current frame; not broad gameplay, visual or art acceptance |
| Actual saved Codex launcher configuration | Reverified 21-tool initialization/list/status via the official stdio client | Existing Windows launcher loads its encrypted local key and identifies JevSandbox/bridge 0.2.0; this check made zero provider requests |
| External catalog protocol | Real MCP SDK against synthetic local network servers with 200 tools and JSON/SSE responses | Streamable HTTP initialization/listing/search/schema retrieval; no external tool execution or installed Epic catalog coverage claim |
| Live 0.2 semantic smoke | 3 valid provider responses; 4/4 authored labels matched | Actual asset selection, batched diagnostics and one unsupported-action deferral; not production accuracy |

The repository's GitHub Actions workflow runs Python 3.12/3.13 checks on Windows and Ubuntu. Inspect the check associated with the commit you use; cloud CI does not compile proprietary Unreal Engine or run the local editor tests.

The first PR matrix exposed an intermittent Windows Python 3.12 test-server failure: sse-starlette's process-global shutdown state could leak between synthetic Uvicorn server lifecycles. It was reproduced locally and corrected with the upstream test lifecycle reset. Fresh-state and previous-shutdown variants cover both JSON and SSE; 48 consecutive real-socket lifecycles then passed on Python 3.12. Client deadlines and production behavior were unchanged.

## Network/editor evidence

The isolated sandbox reported `5.8.2-56702186+++UE5+Release-5.8`, with its listener at `127.0.0.1:9845`. The historical 0.2 nine-test native automation report is timestamped **`2026.09.22-01.53.08`**.

`scripts/smoke_editor.py` launched the real Python MCP server over stdio, initialized it with the official MCP client, and successfully checked:

1. Missing bearer authentication is rejected (HTTP 401).
2. Browser Origin requests are rejected (HTTP 403).
3. The connected project matches this repository's isolated sandbox.
4. Preview creates no actor.
5. One plan spawns a Cube and Sphere.
6. Another plan changes the Cube's location to `[100,50,60]` centimeters.
7. Reapplying a consumed plan returns `unknown_plan`.
8. Applying a competing plan after that transform returns `stale_plan`.
9. Inspection limits/truncation work, and asset registry queries succeed.
10. Compact context, exact native mesh measurements, existing static-mesh placement and bounded scene warnings work against the editor.
11. Grid, staircase and room previews leave the scene unchanged; application creates 4, 4 and 5 actors respectively, with all three native readback verifications passing.
12. Framing the staircase and capturing the editor viewport produces a **1014 × 479 PNG, 520,449 bytes**, with matching current camera metadata.

Visual review initially exposed stale viewport pixels after framing. Capture now submits pending component updates, draws the target viewport, and waits for rendering commands before readback. After rebuilding, rerunning all nine native suites, and repeating the live smoke, the [reviewed native capture](images/verified-stair-blockout.png) visibly showed the intended four-step staircase. This is narrow visual evidence, not a gameplay or general art-quality verdict.

The Python bridge serializes calls and paces individual HTTP requests, including identity reads, to at most **20 per second per client**, below the native **30-per-second** authenticated limit. No potentially applied operation is automatically retried. Multiple clients can still be rate-limited; this is not a sustained-load benchmark.

Test actors remain unsaved. No project packages were saved and no other game project was modified. Local raw evidence is ignored in `artifacts/editor-smoke-v0.2.json`, `artifacts/editor-viewport.png` and `artifacts/unreal-automation/`. [Native coverage and limitations](UNREAL_VALIDATION.md).

## Live 0.3 semantic smoke

At **2026-09-22 02:35:07 UTC**, the same three-request public-fixture workflow
smoke returned three valid responses from `typesafe/jev-1.13-20260917`, with all
four authored labels matching (three recommendations and one abstention). The
unsupported-action case used the expanded **17-candidate** router. No editor
operation ran. [Sanitized 0.3 report](evaluations/2026-09-22-workflows-v0.3.json).

Reported provider latency was **343.17 ms** for asset selection, **228.67 ms** for
batched diagnostics, and **375.42 ms** for the unsupported route. Usage totaled
**2,368 input tokens, 386 output tokens and USD 0.000099456**, with zero cache hits
and automatic retries. These few synthetic cases establish helper compatibility,
not held-out accuracy, improvement over direct tools or a productivity gain.

## Historical live 0.2 semantic smoke

Run the masked key setup if needed, then the three-request workflow smoke:

```powershell
.\scripts\Set-OpenRouterKey.ps1
.\scripts\Test-Jev.ps1 -WorkflowSmoke
```

Only built-in public synthetic inputs are sent; the cache is disabled, no requests are automatically retried, and no editor operation executes. The underlying `scripts/smoke_semantics.py` uses two requests by default; `--include-route` adds the third. `--validate-only` checks the actual helper request shapes without credentials or network access. Semantic disagreement is reported separately from a wire/helper-contract failure.

The final run was recorded at **2026-09-22 01:55:52 UTC**, with returned model `typesafe/jev-1.13-20260917`. The [sanitized 0.2 report](evaluations/2026-09-22-workflows-v0.2.json) includes fixture, helper-code, script and request hashes.

| Request | Observed result | Provider latency |
| --- | --- | --- |
| Asset shortlist | Chose the wooden door after deterministic exclusion of a no-collision candidate | 360.64 ms |
| Batched diagnostic groups | Classified compiler and shader groups correctly in one request | 189.31 ms |
| Unsupported operation | Deferred an arbitrary-Python/deletion request against the current 11-candidate router | 258.27 ms |

Totals: **3 valid responses, 0 errors, 4/4 raw and gated authored labels matched, 3 recommendations and 1 abstention**. Reported usage was **2,216 input tokens, 329 output tokens, USD 0.000093072**, with zero cache hits and retries. These few obvious cases establish live helper compatibility; they do not establish general routing quality or savings over direct tool use.

## Historical five-candidate routing evaluation

Use the masked local prompt, then run:

```powershell
.\scripts\Set-OpenRouterKey.ps1
.\scripts\Test-Jev.ps1
```

This runs at most 24 public-fixture requests against the **current** router, disables the cache, and writes a new local report. It does not automatically reproduce the historical five-candidate code. Alternatively, set your key in the process environment and use `uv run python scripts/evaluate.py --output artifacts/jev-evaluation.json`. Missing credentials never substitute a simulated result.

The first live run completed at **2026-09-22 01:18:45 UTC** using OpenRouter's returned model `typesafe/jev-1.13-20260917`: 24 sequential requests, zero retries, zero cache hits, and 24 valid responses. It used the initial **five-candidate catalog**. These figures do **not** measure the expanded 11-candidate router, the full installed Epic catalog, or real game-development workflows. The [historical sanitized report](evaluations/2026-09-22-openrouter.json) preserves its original fixture and routing-code hashes.

| Metric | Observed result |
| --- | --- |
| Jev decisions matching authored labels | 22/24 |
| Keyword baseline matching the same labels | 23/24 |
| Recommendations / deferrals | 17 / 7 |
| Correct among accepted recommendations | 16/17 |
| Median / p95 response latency | 212 ms / 318 ms |
| Provider-reported cost for 24 requests | USD 0.000522564 |
| Input / output tokens reported | 12,442 / 1,811 |

One supported transform request was deferred; one unsupported delete request received an actor-inspection recommendation instead of the expected deferral. The latter passed the confidence gate, confirming that confidence is not a correctness guarantee. Neither recommendation executed an editor operation. No thresholds or fixture labels were changed after observing these results.

The keyword baseline did better on this small, easy, non-held-out set. These results verify live wiring and illustrate the need to use Jev selectively; they do not establish value over simple rules or a game-development speedup. A useful next study needs representative real tool catalogs, ambiguous cases, larger held-out labels, baseline coding-agent runs, error rates and total human correction time. The separate connectivity smoke call is excluded from the table.

## Remaining limits

Actual discovery of the installed Epic server's complete tool catalog remains unverified. Catalog tests used synthetic metadata, including real local SDK/network exchanges; discovery never executes advertised tools. A gateway listing is not proof of underlying catalog coverage. See [tool discovery](TOOL_DISCOVERY.md).

No production accuracy, game-development speedup, broad gameplay/visual acceptance, other engine/platform build, production-map performance, sustained load, runtime NPC system, Blender executor, or multiplayer behavior is claimed. Native transforms exclude Blueprint actors and attachment hierarchies. Existing-mesh placement validates the exact path and reviewed UObject identity; it does not hash every byte of mutable asset content. The local bridge is for a trusted workstation, not internet deployment. Review [architecture](ARCHITECTURE.md) and [security](../SECURITY.md).

## Windows credential setup regression

Version `0.1.0a2` fixes credential scripts resolving `Microsoft.PowerShell.Security` through inherited module search paths. All three credential entry points now import the manifest under the running shell's `$PSHOME`, without force-reloading it. Windows PowerShell 5.1 regression checks cover clean and mixed-edition module paths, repeated imports, an already loaded native module, and DPAPI round trips using synthetic values only. The interactive setup script was also exercised with temporary storage and a synthetic masked-input substitute; no real credential file was used for these tests.

If an older prompt failed with duplicate `ObjectSecurity` type-data members, close that failed window and open the updated setup script in a fresh PowerShell process. No machine-wide PowerShell settings need changing. See Microsoft's [module-path inheritance explanation](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/about/about_psmodulepath?view=powershell-7.6).
