# Real Codex agent benchmark adapter

`python -m jev_unreal.codex_benchmark` runs fresh ephemeral agent threads through
an installed and authenticated Codex CLI's experimental app-server API. Unlike
the synthetic paired demo, the agent chooses and calls actual Unreal read tools.
See the [official app-server documentation](https://learn.chatgpt.com/docs/codex-app-server)
and [paired evidence contract](PAIRED_BENCHMARKS.md).

## Bounded initial workload

The adapter measures the local dimensions and LOD counts of the public engine
Cube and Sphere assets. Each arm receives the same three dynamic tools:
`unreal_actors`, `unreal_actor_details`, and `unreal_asset_details`. The on arm
receives one optional Jev recommendation before the same task. The off arm makes
no Jev request. Tool choice remains the agent's responsibility.

The adapter does not yet measure edits, ambiguous scene intent, failure recovery,
gameplay, Blender or another MCP implementation. These two straightforward
inspection cases validate the real-agent measurement path; they cannot establish
general development productivity or statistically reliable superiority.

## Run

Use an isolated sandbox and a current installed Codex CLI. Configure the exact
project and bridge token using the normal profile file. Do not put keys in the
command line or report files.

```powershell
$env:JEV_PROFILES_FILE = 'C:\private\jev-profiles.json'
$env:JEV_PROFILE = 'sandbox'
uv run python -m jev_unreal.codex_benchmark --off-only `
  --output artifacts/agent-off-smoke
```

This executes one real agent trial and costs whatever your Codex account charges,
but makes zero Jev/OpenRouter requests. It exits nonzero if verification fails.
The report explicitly marks it as an off-only smoke, not a classifier comparison.

To run the paired comparison after provider authentication succeeds:

```powershell
uv run python -m jev_unreal.codex_benchmark --repetitions 1 `
  --output artifacts/agent-paired
```

On Windows, `scripts/Benchmark-Agent.ps1` loads the current user's saved DPAPI
credential without putting it in arguments or output:

```powershell
.\scripts\Benchmark-Agent.ps1 -ProfilesFile .local\profiles.json -Profile sandbox `
  -OutputDirectory artifacts\agent-paired -Repetitions 1
```

Use `-OffOnly` for one actual-agent trial without loading a provider credential.
The wrapper prefers the saved credential; if absent it uses the process key.
The direct Python module requires a securely supplied process key and does not
decrypt Windows credentials itself. One repetition is four agent trials and at most two Jev
requests. Repetitions are bounded at three, native calls at four per trial, each
trial at 120 seconds and the full paired run at 600 seconds. Authentication failure
blocks subsequent on-arm trials. An existing output directory is refused.

## Isolation and evidence

Threads use an empty temporary directory, explicit read-only sandbox and no
approval prompts. The installed CLI model, reasoning effort and service tier are
recorded; the adapter does not select a model on the user's behalf. Native tool
calls are forwarded serially through a separate authenticated Jev MCP process.
Shell, code execution, apps, plugins, other MCP servers, web, image tools and
subagents are disabled in thread configuration. Unexpected tool/item events
invalidate the run, and unknown dynamic tool requests are never dispatched.
These controls bound this adapter, not arbitrary future Codex APIs.

Independent native reads freeze expected evidence before trials, check state
before/after preparation and asset reads, and verify both answer and unchanged
project/session/world/revision afterward. Numeric JSON representations such as
`100` and `100.0` compare consistently. No model judgment decides whether its own
answer passed. Cache usage is recorded, not assumed cold.

Private output contains protocol hashes, aggregate usage, sanitized observations
and verified outcomes. The adapter does not persist raw model transcripts or
provider payloads. Codex's own host logging policy remains outside this adapter.
Reported input tokens include cached input; cached tokens are not an extra charge
to add to that total. Agent cost and exact served model revision remain unknown.
Provider cost is included only when reported for a fresh provider request.

An actual local off-only smoke on 2026-09-29 passed independent native validation:
one task tool call, no unexpected tool events, no Jev request. The CLI reported
17,235 input tokens (8,320 cached) and 87 output tokens. This is evidence that the
adapter works against the installed host, not an efficiency result.
