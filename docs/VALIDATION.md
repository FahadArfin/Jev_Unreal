# Validation evidence

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
