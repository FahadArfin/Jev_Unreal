# Validation evidence

Initial validation: **2026-09-22 UTC** (2026-09-21 local US Eastern). This is a tested alpha, not a production-readiness certificate.

| Layer | Result | What it establishes |
| --- | --- | --- |
| Python adversarial suite | 232 tests passed locally on Python 3.13.2 | Provider wire contracts, response validation, cache/concurrency, budgets, circuit breaker, error redaction, project binding, MCP handshake and evaluation harness |
| Ruff | Passed | Configured Python static checks |
| Packaging | Wheel and source distribution built | Python package can be constructed; not a public PyPI publication |
| PowerShell scripts | Syntax checked; DPAPI and launcher credential flow tested with synthetic values | Local credential handling without plaintext arguments |
| UE C++ build | Passed on installed UE 5.8.2 | Editor module compiles/links; compiler preference warning documented separately |
| Native Unreal automation | 3 passed, zero failures/warnings | Native scene edits/Undo, strict inputs, expiry/replay and object/world replacement handling |
| Real MCP -> HTTP -> Unreal | Passed, 9 tools discovered | Actual authenticated editor operations, not mocks |
| Actual saved Codex launcher configuration | MCP initialization/list-tools/status passed | PowerShell launcher starts the server and identifies the sandbox; in-app reconnect still required to expose tools in a new Codex session |
| Jev evaluation fixture validation | 24 cases validated, zero cloud calls | Inputs/harness are valid; no statement about model quality |
| Live Jev provider calls | 24/24 valid responses through OpenRouter; 22/24 matched fixture labels | Real account access, wire compatibility and small-fixture behavior; not production accuracy |

The repository's GitHub Actions workflow runs Python 3.12/3.13 checks on Windows and Ubuntu. Inspect the check associated with the commit you use; cloud CI does not compile proprietary Unreal Engine or run the local editor tests.

## Network/editor evidence

The sandbox reported `5.8.2-56702186+++UE5+Release-5.8`. The OS confirmed a listener at `127.0.0.1:9845` owned by that sandbox editor process.

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

Test actors were left unsaved. No project packages were saved and no other game project was opened. Local raw evidence is ignored in `artifacts/editor-smoke.json` and `artifacts/unreal-automation/`. [Native test details and known engine startup issues](UNREAL_VALIDATION.md).

## Live provider evaluation

Use the masked local prompt, then run:

```powershell
.\scripts\Set-OpenRouterKey.ps1
.\scripts\Test-Jev.ps1
```

This uses the configured OpenRouter account for at most 24 public-fixture requests, disables the local cache, and writes a sanitized local report. Alternatively, set your key in the process environment and use `uv run python scripts/evaluate.py --output artifacts/jev-evaluation.json`. No simulated result is substituted when credentials are missing.

The first live run completed at **2026-09-22 01:18:45 UTC** using OpenRouter's returned model `typesafe/jev-1.13-20260917`: 24 sequential requests, zero retries, zero cache hits, and 24 valid responses. Results are recorded in a [sanitized evaluation report](evaluations/2026-09-22-openrouter.json), with fixture and routing-code hashes.

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

No production accuracy or game-development speedup is claimed. No visual/gameplay acceptance, other engine/platform builds, production-map performance, sustained load, runtime NPC system, Blender executor or multiplayer behavior was tested. Native static mesh transformations exclude Blueprint actors and attachment hierarchies. The local HTTP service is for a trusted workstation, not internet deployment. Review [architecture](ARCHITECTURE.md) and [security](../SECURITY.md).

## Windows credential setup regression

Version `0.1.0a2` fixes credential scripts resolving `Microsoft.PowerShell.Security` through inherited module search paths. All three credential entry points now import the manifest under the running shell's `$PSHOME`, without force-reloading it. Windows PowerShell 5.1 regression checks cover clean and mixed-edition module paths, repeated imports, an already loaded native module, and DPAPI round trips using synthetic values only. The interactive setup script was also exercised with temporary storage and a synthetic masked-input substitute; no real credential file was used for these tests.

If an older prompt failed with duplicate `ObjectSecurity` type-data members, close that failed window and open the updated setup script in a fresh PowerShell process. No machine-wide PowerShell settings need changing. See Microsoft's [module-path inheritance explanation](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/about/about_psmodulepath?view=powershell-7.6).
