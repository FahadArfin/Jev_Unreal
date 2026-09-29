# Routing off versus routing on

This two-arm runner tests whether adding one Jev routing consultation helps the
**same agent complete the same workflow**. It complements the routing-label
benchmark and three-arm evidence study in [BENCHMARKS.md](BENCHMARKS.md). It does
not compare Jev_Unreal with another Unreal MCP implementation.

The public demo executes fake tools locally. It uses no model, provider, editor,
API key, or paid requests, and cannot establish an efficiency improvement.
Real comparisons require actual collected observations and independent workflow
verification; provider fallback alone is not evidence of classifier benefit.

## Run the offline example

From the repository root:

```powershell
uv run python -m jev_unreal.paired_benchmarks schedule `
  --protocol examples/benchmarks/paired-routing-demo.protocol.json `
  --output artifacts/paired-demo/schedule.json

uv run python -m jev_unreal.paired_benchmarks demo `
  --protocol examples/benchmarks/paired-routing-demo.protocol.json `
  --output artifacts/paired-demo/observations.json

uv run python -m jev_unreal.paired_benchmarks report `
  --protocol examples/benchmarks/paired-routing-demo.protocol.json `
  --observations artifacts/paired-demo/observations.json `
  --output artifacts/paired-demo/report.json
```

This executes four serial trials: off/on, then on/off, for a synthetic cube-bounds
workflow. The runner calls preparation, the ON routing hook, bounded task tools,
and an independent answer-checking hook. The report must mark
`valid_classifier_comparison: false` because its origin is synthetic.

To prepare a saved-observation file for a real frozen protocol:

```powershell
uv run python -m jev_unreal.paired_benchmarks template `
  --protocol artifacts/my-pilot/protocol.json `
  --output artifacts/my-pilot/observations.json
```

The template includes every scheduled trial as `missing`; fill each completed
trial with its actual measured observations, then use `report`. Missing trials
remain in success-rate denominators. The report command validates JSON shape,
protocol binding, identities, routing-arm contracts, and required evidence. It
does not execute a provider or editor. Inputs are strict JSON limited to 2 MiB;
unknown fields, duplicate object keys, and nonfinite measurements are rejected.

## Freeze a fair protocol

Choose a small owned sandbox, a fixed task catalog, and objective acceptance
criteria before collecting observations. Start with a few paired tasks, then
repeat only if the result warrants the cost. Include ambiguous routing, ordinary
inspection, a bounded edit, and an intentional recoverable failure as distinct
tasks when progressing beyond the read-only pilot.

Declare these fields in the protocol:

| Field | Meaning |
| --- | --- |
| `model_id` | Actual model identifier reported by the agent adapter; do not infer the served revision |
| `agent_configuration_sha256` | Hash of identical agent prompt/settings except the arm's routing intervention |
| `environment_sha256` | Hash of engine/plugin/server versions and relevant execution environment |
| `project_id` | Exact owned sandbox project identity, not just its display name |
| `catalog_sha256` | Hash of the exact visible tool descriptions **and schemas** |
| `allowed_tools` | Task tool allowlist shared by both arms |
| `tasks[].initial_state_sha256` | Hash of measured restored state, not an assumed scene label |
| `tasks[].acceptance_sha256` | Hash of the frozen verification contract |
| `cache_policy` | Cold, warm, or record-only; measure cache usage separately |
| `repetitions`, `seed` | Deterministic paired order, alternating the first arm across tasks/repetitions |
| `max_tool_calls` | Per-trial task-call budget, including failed calls |
| `trial_timeout_seconds`, `total_timeout_seconds` | Preparation + task + verification deadlines |

The maximum protocol has 256 trials, 64 tools per trial, a five-minute trial
deadline, and a one-hour total deadline. Smaller defaults are intentional.
Exactly one routing hook runs for each started ON task. OFF tasks skip it. There
is no automatic rerun of failed trials and no provider retry in the driver
contract. Provider authentication failure blocks subsequent ON trials while OFF
trials can continue; all scheduled trials remain visible in the report.
The rejected request is `provider_failed`; later ON trials are `blocked` with
no routing attempt or task execution. Unknown request counts on interrupted
attempts remain unknown.

For retries after a fix, freeze a new protocol/run and retain the original
failure. Do not replace failed rows with the best later attempt.

## Connect a real agent adapter

`run_paired(protocol, factory)` is an async Python API. The factory receives a
`PairedTrial` and returns a fresh `TrialDriver`. The adapter is trusted **local
code**, not a module path or executable command accepted from benchmark JSON or
from the authenticated editor bridge. No dynamic adapter loader or arbitrary
execution endpoint is exposed.

Implement these hooks using an existing bounded agent SDK and authenticated
Jev_Unreal tools:

1. `prepare(task) -> TrialIdentity`: establish a fresh agent context, restore the
   sandbox to the task's initial state, measure its identity, confirm actual model
   and visible catalog/configuration, and return a unique `fresh_context_id`.
2. `route(task) -> RouteObservation`: make at most one Decisions request, retries
   disabled, using the frozen candidates. Return `recommend`, `defer`, `error`, or
   `authentication_failed`. Map HTTP 401/403 explicitly to authentication failure.
   Return only normalized telemetry and a candidate tool ID, never raw payloads.
3. `execute(task, context, recommendation) -> DriverResult`: run the same fresh
   agent loop in each arm, allowing the ON recommendation to advise its choice.
   Every task tool call must use `await context.call_tool(name, arguments)`.
   Await all work; do not start detached tasks or make hidden tool/provider calls.
   Return a hash of the resulting answer and **task-interval** agent telemetry.
4. `call_tool(name, arguments)`: dispatch an allowed tool through the normal
   project/authentication/preview/permission boundaries. The harness serializes
   calls and counts failures; it recognizes native `ok: false` and MCP `isError`
   replies, including structured content and JSON text content. For another
   transport, return `ToolReply(succeeded=False, value=reply)` on failure; the
   context unwraps the value for the agent while retaining only the success flag,
   duration and JSON byte count. It does not authorize editor mutations. Confidence
   or a recommendation never grants permission.
5. `verify(task, answer_sha256) -> Verification`: independently inspect acceptance
   against actual engine state or captured evidence. Confirm the same project and
   frozen acceptance digest; return passed/failed and a sanitized evidence hash.
   Do not accept the agent's self-reported success as verification.

```python
from jev_unreal.paired_benchmarks import PairedProtocol, run_paired, report

# protocol = PairedProtocol.model_validate(your_frozen_protocol)
# YourFreshAgentDriver implements the five hooks above.
# run = await run_paired(protocol, lambda trial: YourFreshAgentDriver(protocol, trial))
# result = report(protocol, run)
```

No real model-specific agent adapter is bundled. The runnable synthetic adapter
shows orchestration, and the template/report CLI supports actual saved trials
without Python programming. A real agent experiment still requires an adapter
or independently collected observations; importing a file is not executing an
experiment.

The timeout mechanism is cooperative asyncio cancellation. Adapters must not
block the event loop or suppress cancellation, must release their resources in
`finally`, and must await cancellation of model/tool requests. A local adapter
can bypass Python instrumentation, so this is measurement infrastructure rather
than a security sandbox. Configure token/currency limits in the actual agent and
provider SDKs; the runner cannot enforce a monetary limit from missing telemetry.

## Interpret the report

- Success rates use **all planned trials**, including missing, timeout, blocked,
  verification, provider, and tool failures. Failure codes are grouped per arm.
- `task_ms` includes routing plus agent/tool work. `setup_ms` and
  `verification_ms` are separate and must be included for end-to-end comparisons.
  Agent process startup outside `prepare` is unmeasured and must be disclosed.
- Task tools and routing attempts are separate counts. Verification/preparation
  work is separate from the task tool-call budget and must be bounded by the
  adapter and the shared deadline. A routing attempt is not proof of a successful
  provider request.
  `task_calls_including_routing` and the corresponding paired delta include the
  routing consultation so its extra call is visible. `started` distinguishes
  executed trials from scheduled rows recorded as blocked or missing.
- Input tokens include cached input as a subset; do not add cached tokens again.
  Never put whole-chat or setup-inclusive token totals in task telemetry fields.
- Agent costs and routing-provider costs are separate, with `provider_reported`
  versus `estimated` provenance retained and broken down in reports. `null` means
  unknown. A known subtotal is not a complete bill. HTTP failure does not imply
  zero billed cost. Result bytes are measured JSON bytes, **not token counts**.
- Matched verified-success pairs show ON minus OFF deltas. Negative time or
  tool-call deltas favor ON for those pairs, but failures are excluded from these
  deltas. Always present the all-trial success rates alongside them.
- `collection_complete` distinguishes a fully attempted schedule from partial
  collection. Missing, blocked, budget-skipped and cancelled trials prevent full
  comparison eligibility. `all_trial_identities_valid` requires measured matching
  identities and unique fresh contexts for every trial, including failed trials.
- `comparison_eligibility` lists explicit exclusion reasons. Authentication
  failures, synthetic evidence and mismatched/unmeasured identities invalidate
  the classifier-comparison flag. Ordinary workflow verification failures remain
  eligible observations and stay in denominators: a complete comparison need not
  contain successful pairs. Compare success/failure rates even when matched
  timing deltas are unavailable. Eligibility does not establish statistical
  significance, practical benefit, independent audit, or production readiness.

Only sanitized hashes, counts, durations, IDs, and normalized outcomes belong in
saved observations. Do not commit keys, bearer tokens, real game scene content,
raw model/provider payloads, or the ignored local benchmark artifacts.
