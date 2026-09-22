# Jev_Unreal

[![Python checks](https://github.com/FahadArfin/Jev_Unreal/actions/workflows/ci.yml/badge.svg)](https://github.com/FahadArfin/Jev_Unreal/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**Find the right tools, inspect Unreal, build measured layouts, and verify the results. Jev helps when a semantic choice is useful.**

A coding agent can ask Jev to choose a tool or classify diagnostics, while deterministic code validates and executes bounded Unreal editor operations. Independent community project inspired by [cnrveysel/JevUnreal](https://github.com/cnrveysel/JevUnreal).

**Status: 0.2 alpha.** Python MCP server + source-built Unreal editor plugin. Initial target: Windows and Unreal 5.8.2. Python tests run on Windows/Linux; Linux/macOS Unreal builds are not certified. See [validation evidence](docs/VALIDATION.md) and [release notes](CHANGELOG.md).

## What works

- Jev **Choice, Score and Noul** through OpenRouter's Decisions API or TypeSafe directly.
- Batched questions, strict response validation, bounded in-memory cache, request limits, timeouts and a circuit breaker.
- Tool recommendation with explicit deferral for unsuitable choices or missing uncertainty data.
- Discover explicitly configured local MCP catalogs, search compact descriptions, and retrieve exact schemas on demand. Optional Jev shortlist selection never executes another server's tools.
- Local diagnostic grouping, repetition counts and line evidence; optional batched Jev classification.
- Asset candidate filtering by known class, dimensions and collision, with optional Jev selection.
- Compact editor context: selection, dirty packages, play state, actor metadata and project identity.
- Exact static mesh bounds, materials, LOD and collision inspection; bounded scene validation warnings.
- Native viewport framing and PNG capture delivered as MCP image content for visual review.
- Preview/apply primitive blockouts, existing static mesh placement and supported native actor transforms, with native Undo.
- Measured grid, staircase and room recipes, with automatic transform readback checks after apply.
- Authenticated loopback bridge, project binding, short-lived single-use plans and stale editor-state rejection.
- A sample project, adversarial tests, a real MCP/editor smoke test and a provider evaluation harness.

Scene tools need **no model key**. Jev never executes an editor command, generates arbitrary code, or automatically receives project files. Only explicit decision arguments go to the configured cloud provider.

## Is Jev useful here?

Yes, where there are many small semantic choices: select a relevant tool, classify a log or prioritize candidates. Batching judgments over shared context may reduce requests. When the correct operation is known, call it directly; a model adds cost and latency to a deterministic decision.

Jev does not replace a coding/reasoning model, Unreal APIs, visual review or build/play tests. Typed output can still be wrong. We do not claim a measured game-development speedup or production-scale reliability. [Research and primary sources](docs/RESEARCH.md) explain the opportunities and limitations.

## Quick start on Windows

Requirements: licensed UE 5.8, its supported Visual Studio C++ toolchain, Python 3.12+, [uv](https://docs.astral.sh/uv/getting-started/installation/) and Git. Engine binaries are not included.

```powershell
git clone https://github.com/FahadArfin/Jev_Unreal.git
cd Jev_Unreal
uv sync --locked --all-extras
```

If scripts are disabled, open a process-scoped shell with `powershell -NoProfile -ExecutionPolicy Bypass`, then run these commands **in that shell**:

```powershell
.\scripts\Initialize-Local.ps1
.\scripts\Build-Unreal.ps1 -EngineRoot 'C:\Program Files\UE_5.8'
.\scripts\Launch-Unreal.ps1 -EngineRoot 'C:\Program Files\UE_5.8'
uv run jev-unreal status
uv run jev-unreal doctor
```

Initialization creates a random token under `%LOCALAPPDATA%\JevUnreal`, sets this shell's environment, and targets the isolated `examples/JevSandbox` project. Keep the editor open. The bridge token is separate from your provider key. Port **9845** is fixed in this alpha; do not start two bridge editors concurrently.

Optional real Jev access:

```powershell
.\scripts\Set-OpenRouterKey.ps1
```

This masked prompt stores your key with Windows user-scoped DPAPI encryption. The MCP launcher decrypts it only into its process environment. Never put keys in source control, chat, command arguments or an Unreal project. Elsewhere, set `OPENROUTER_API_KEY` in the server environment. For direct TypeSafe, set `JEV_PROVIDER=typesafe`, `TYPESAFE_API_KEY` and optionally `JEV_MODEL=jev-1.13.0`.

If an older version fails with duplicate `ObjectSecurity`/`AuditToString` type-data errors, update to `0.1.0a2` or later and reopen the setup script in a fresh PowerShell window. The corrected scripts load the running shell's native security module directly.

## Connect an MCP client

For this trusted Codex project:

```powershell
.\scripts\Configure-Codex.ps1
```

It creates an ignored `.codex/config.toml` with absolute paths and **no credentials**, preserving any existing config. Restart the MCP connection and verify `jev_status` identifies your intended project. [Official Codex MCP configuration](https://developers.openai.com/codex/mcp).

For another MCP client, use STDIO and substitute your paths:

```json
{
  "mcpServers": {
    "jev_unreal": {
      "command": "powershell.exe",
      "args": [
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
        "C:/path/to/Jev_Unreal/scripts/Start-Mcp.ps1",
        "-BridgeTokenFile", "C:/Users/YOU/AppData/Local/JevUnreal/bridge.token",
        "-ExpectedProject", "C:/path/to/Jev_Unreal/examples/JevSandbox/JevSandbox.uproject"
      ]
    }
  }
}
```

Portable server command: `uv --directory /path/to/Jev_Unreal run --frozen jev-unreal serve`. Set `JEV_BRIDGE_TOKEN_FILE`, `JEV_EXPECTED_PROJECT`, and optional provider credentials in that process environment. Python portability does not certify Unreal builds on that platform.

## Tools

| Tool | Purpose | Cloud |
| --- | --- | --- |
| `jev_status` | Provider counters and editor identity | No |
| `jev_decide` | 1-32 typed questions over explicit shared state | Yes |
| `jev_route` | Recommend a built-in or supplied candidate tool; may defer | Yes |
| `jev_triage` | Classify a supplied diagnostic excerpt | Yes |
| `jev_catalog_refresh` | Discover configured local MCP schemas; atomic cached snapshot | No |
| `jev_catalog_search` | Find relevant tools/actions with compact local search results | No |
| `jev_catalog_get` | Retrieve an exact versioned external tool schema | No |
| `jev_catalog_rerank` | Recommend one explicitly shortlisted discovered tool | Yes |
| `jev_rank_assets` | Hard-filter and shortlist supplied asset metadata | Opt-in |
| `jev_diagnostics` | Group logs with counts, categories, redaction and evidence | Opt-in |
| `unreal_status` | Engine/project/session/world identity | No |
| `unreal_context` | Selection, play state, dirty packages and relevant actors together | No |
| `unreal_actors` | Bounded actor metadata and transforms | No |
| `unreal_assets` | Search `/Game` asset metadata | No |
| `unreal_asset_details` | Inspect one exact mesh path, bounds, LODs, materials and collision | No |
| `unreal_validate` | Inspect loaded actors for structural warnings | No |
| `unreal_frame` | Frame explicit actors in the current editor viewport | No |
| `unreal_capture` | Return the rendered editor viewport as an MCP image | No |
| `unreal_preview` | Validate a batch and return a 120-second plan | No |
| `unreal_layout_preview` | Preview a measured grid, staircase or room | No |
| `unreal_apply` | Apply once in an Undo transaction; verify native transform readback | No |

Example `unreal_preview` arguments:

```json
{
  "operations": [
    {"op": "spawn_primitive", "shape": "Cube", "label": "Cover_Block",
     "location": [0, 0, 100], "scale": [3, 1, 2]}
  ]
}
```

Review normalized operations, then pass the returned `plan_id` to `unreal_apply`. Positions are centimeters; rotation is `[pitch,yaw,roll]` in degrees. Maximum 20 operations per plan. Edits during Play/Simulate and stale/reused plans are rejected. Changes remain unsaved until you save in Unreal. After a timeout, inspect the scene before deciding what to do; do not blindly retry.

For a measured staircase, call `unreal_layout_preview` with:

```json
{"layout":{"kind":"stairs","steps":8,"rise_cm":18,"tread_depth_cm":30,"width_cm":150,"label_prefix":"Entrance","origin":[0,0,0]}}
```

Apply the returned plan, inspect its `verification`, frame the resulting actor paths, and capture the viewport. The result verifies identities and transforms; it does not certify gameplay traversal, collision behavior or appearance. [Practical workflows and examples](docs/WORKFLOWS.md).

![Four-step blockout captured through the native Unreal viewport tool](docs/images/verified-stair-blockout.png)

Actual UE 5.8.2 sandbox output from the MCP smoke test: four measured steps, applied, checked, framed and captured. This is an operation check, not a finished environment.

Read `jev://layouts` for recipes and `jev://catalog` for built-in routing descriptions. MCP clients that support prompts can use `blockout_workflow` and `diagnostic_workflow` for the complete inspection/verification sequence.

External discovery is opt-in: set `JEV_CATALOG_FILE` to a local JSON configuration, or pass `-CatalogFile` to the Windows launcher. It lists metadata from explicitly named loopback Streamable HTTP servers. It never launches a server or proxies arbitrary execution. [Discovery setup, limits and Epic gateway compatibility](docs/TOOL_DISCOVERY.md).

For your own project, copy `Plugins/JevEditor` into its `Plugins` directory, enable it, compile, and launch with `JEV_BRIDGE_TOKEN` in the editor environment. Set the MCP server's `JEV_EXPECTED_PROJECT` to that absolute `.uproject`. Test scripts belong only in the sandbox.

## Validation and development

```powershell
uv run ruff check .
uv run pytest
uv build
.\scripts\Launch-Unreal.ps1 -AutomationTests
uv run python scripts/smoke_editor.py
uv run python scripts/smoke_editor.py --require-capture
uv run python scripts/smoke_catalog.py
uv run python scripts/evaluate.py --validate-only
```

The editor smoke test needs the running sandbox and intentionally creates unsaved test actors. `--require-capture` needs a rendered editor viewport; headless NullRHI cannot supply one. The catalog smoke needs `JEV_CATALOG_FILE`. Native automation runs a separate sandbox editor; do not run it concurrently with a bridge test. [Validation details](docs/VALIDATION.md) distinguish the test layers.

For a real evaluation, set the provider key in the process environment and run `uv run python scripts/evaluate.py --output artifacts/jev-evaluation.json`. It spends at most 24 provider requests on public fixtures, disables caching, uses no retries and compares a simple keyword baseline. This small authored smoke set cannot establish production accuracy or coding-agent savings.

With a saved Windows key, `.\scripts\Test-Jev.ps1 -WorkflowSmoke` checks asset selection, batched diagnostics and unsupported-action deferral using at most three provider requests. Its [sanitized report](docs/evaluations/2026-09-22-workflows-v0.2.json) records the fixture and code hashes, usage and individual outcomes.

[`.env.example`](.env.example) lists variables; the server does **not** automatically load `.env`. `JEV_MAX_REQUESTS` defaults to 100 attempted provider calls per server process; it is not a dollar budget. OpenRouter uses `typesafe/jev-1.13` at `/api/alpha/decisions`; alpha contracts may change.

## Scope and roadmap

Version 0.2 concentrates on discovery, inspection, explicit decision assistance and verified editor workflows. It does not include runtime NPC Blueprint nodes, arbitrary Blueprint graph generation, code execution, asset deletion/import, packaging automation, remote/multiuser hosting, or a Blender executor. Existing Unreal MCP tools remain useful alongside this server; discovery helps find their advertised capabilities without duplicating them.

Next: representative held-out workflow evaluations, additional measured editor operations, multiple-editor support, broader platform validation and a Blender adapter. No game-development speedup has been established. The model-independent discovery and selection layers can be reused for Blender.

See [architecture](docs/ARCHITECTURE.md), [research](docs/RESEARCH.md), [provenance](docs/PROVENANCE.md), [contributing](CONTRIBUTING.md), and [security](SECURITY.md). Original code is MIT. Unreal and provider services have separate licenses and terms.
