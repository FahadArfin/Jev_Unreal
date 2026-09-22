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
| Live Jev provider calls | **Not run: provider key not supplied locally** | Accuracy, real latency, billing and account access are unverified |

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

The keyword baseline gets **23/24** on the authored smoke set. That demonstrates why a model should not be called before every obvious editor operation. This small, easy, non-held-out set checks wiring and failure behavior; it cannot establish Jev's value over simple rules. A useful next study needs representative real tool catalogs, ambiguous cases, larger held-out labels, baseline coding-agent runs, p50/p95 latency, actual cost, abstention/error rates and total human correction time.

## Remaining limits

No live provider accuracy or speedup is claimed. No visual/gameplay acceptance, other engine/platform builds, production-map performance, sustained load, runtime NPC system, Blender executor or multiplayer behavior was tested. Native static mesh transformations exclude Blueprint actors and attachment hierarchies. The local HTTP service is for a trusted workstation, not internet deployment. Review [architecture](ARCHITECTURE.md) and [security](../SECURITY.md).
