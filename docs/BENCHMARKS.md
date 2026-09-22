# Reproducible routing and workflow evidence

Jev_Unreal includes a local benchmark harness for comparing a fixed keyword baseline,
Jev routing, and imported direct-agent observations on the same inputs. Routing selects
the next tool; it does not run an Unreal workflow. Completion, correction time, failures,
tool calls, and total workflow cost require separate human-reviewed evidence.

The bundled dataset is **public synthetic data**, authored for integration testing. Its
`heldout` partition is explicitly separate from train/dev, but is public and was seen
by the implementation authors. It is not an untouched external test set, an industry
benchmark, or evidence that Jev improves game-development efficiency.

## Recorded synthetic run — September 22, 2026 UTC

The [sanitized result summary](benchmarks/2026-09-22-public-synthetic-v1.json) records
one actual local keyword run and one live Jev Decisions run against all eight public
synthetic heldout cases, using the same frozen catalog and answer key:

| Measurement | Keyword baseline | Jev |
| --- | ---: | ---: |
| Correct next-tool or defer outcome | 7 / 8 | 8 / 8 |
| Correct required deferrals | 2 / 2 | 2 / 2 |
| Additional deferrals on answerable cases | 1 | 0 |
| Provider/contract errors | 0 | 0 |
| Mean observed routing latency | 0.101 ms | 234.95 ms |
| Reported provider cost | $0 | $0.00026061 |

Jev made eight sequential uncached requests. The keyword baseline abstained on the
equal-gap spacing case; Jev recommended `unreal_spatial_preview`. Both deferred on the
unsupported arbitrary-execution request and the ambiguous request. Jev's requested
model was `typesafe/jev-1.13`; the score summaries do not retain the provider's returned
model identity, so that field is not proof of an exact served model revision.

These measurements cover one small authored routing run. The local keyword method
was substantially faster in this run. Jev matched one additional expected label,
which does not establish saved development time or a statistically reliable advantage.
No direct-agent comparison or workflow completion, correction time, gameplay quality,
or artist acceptance was measured. The result file contains only hashes, public case
IDs/tool IDs and sanitized score summaries; it contains no prompts, local project
paths, keys, raw provider responses, or private scene data.

## Run the local baseline

From the repository root:

```powershell
uv run python -m jev_unreal.benchmarks validate examples/benchmarks/public-synthetic-v1.dataset.json --answer-key examples/benchmarks/public-synthetic-v1.answers.json
uv run python -m jev_unreal.benchmarks keyword examples/benchmarks/public-synthetic-v1.dataset.json --output artifacts/keyword-run.json
uv run python -m jev_unreal.benchmarks score examples/benchmarks/public-synthetic-v1.dataset.json examples/benchmarks/public-synthetic-v1.answers.json artifacts/keyword-run.json --output artifacts/keyword-score.json
```

Outputs use exclusive creation. Choose a fresh filename for repeated trials; an explicit
`--overwrite` replaces an existing output. Output paths may never replace input files.

The fixed baseline counts shared lowercase alphanumeric tokens between each goal and
each tool description, after removing a documented set of common words. Zero overlap
and ties abstain. It does not use the case IDs, splits, scenario groups, or answer key.
This intentionally simple baseline is reproducible, not a claim about the best possible
keyword system. Tokenization is currently English-oriented.

## Run the synthetic Jev smoke benchmark

With the normal provider key configured locally, the following makes at most **eight**
sequential Decisions calls, without editing Unreal or sending project files:

```powershell
uv run python -m jev_unreal.benchmarks jev examples/benchmarks/public-synthetic-v1.dataset.json --request-budget 8 --output artifacts/jev-run.json
uv run python -m jev_unreal.benchmarks score examples/benchmarks/public-synthetic-v1.dataset.json examples/benchmarks/public-synthetic-v1.answers.json artifacts/jev-run.json --output artifacts/jev-score.json
```

The CLI reads the usual provider environment; it does not discover or decrypt key files.
Use your existing launcher/session that supplies the key. Do not paste a key into a
command, dataset, report, issue, or repository file.

The runner uses the existing `DecisionClient` and `workflows.route` Decisions contract,
including the same uncertainty gate and `__defer__` option. It disables the local cache,
requires a fresh client, sends one goal per request, and never retries. The maximum
live smoke budget is 12 requests. A partition larger than the budget is rejected before
any request. Provider errors remain scored observations; they cannot improve accuracy
by disappearing from the denominator. The existing provider circuit breaker still applies.

For datasets marked `real_project`, cloud inference requires the explicit
`--allow-private-inputs` flag. Only goals and the frozen candidate descriptions are
sent, but those fields may themselves contain private information. Local keyword
scoring does not require the flag.

## Data separation and fingerprints

`examples/benchmarks/public-synthetic-v1.dataset.json` contains two train cases, two dev
cases, eight heldout cases, and a frozen 14-tool candidate catalog. Its separate
`.answers.json` file binds labels to the canonical dataset SHA-256. Inference functions
have no answer-key argument. Only the scorer loads labels. Provider input contains the
goal and candidate descriptions, not partition names, case IDs, scenario groups, or labels.

The schemas reject unknown fields, duplicate IDs, reserved refusal tool IDs, repeated
normalized goals, and a scenario group appearing in multiple partitions. The answer
key must label every case exactly once, using only catalog tools or an explicit defer
label. Multiple accepted tools are supported when the evaluation author has justified
equivalent next steps. Case input hashes bind the goal and catalog; run hashes bind all
observations, timing and provenance. Changed inputs require new hashes.

Hashes establish reproducible content identity, not signatures, secrecy, or proof that
labels were never seen. Exact-duplicate and declared-group checks do not detect all
semantic paraphrases. For a credible new study, have a separate evaluator author and
hold the answer key, assign related scenarios to one partition, freeze prompts and
thresholds on train/dev, then evaluate heldout once. Do not tune against published
heldout results and continue calling them untouched.

## Import real direct-agent observations

The harness does not pretend to run an agent. Run the actual direct-agent baseline
separately with the same goal, catalog, initial project state, allowed tools and policies.
Preserve its trace and have a person review the selected next tool. Import a `RoutingRun`
with method `direct_agent`, the dataset/catalog/input hashes, implementation/model ID,
per-case observation, `human_reviewed: true`, reviewer ID, and evidence SHA-256.

```python
from jev_unreal.benchmarks import (
    hash_evidence, import_direct_agent_run, load_dataset, read_json,
)

dataset = load_dataset("examples/benchmarks/public-synthetic-v1.dataset.json")
digest = hash_evidence("local-evidence/reviewed-agent-trace.json")
run = import_direct_agent_run(
    dataset,
    read_json("local-evidence/direct-agent-run.json"),
    evidence_files={digest: "local-evidence/reviewed-agent-trace.json"},
)
```

`evidence_files` is optional; when supplied, every referenced evidence hash must have
an exact matching local file. File hashing streams at most 16 MiB per explicitly supplied
file and never stores its contents or paths in the imported run. Without those files,
the import validates the attestation and hash format only. Even a matching trace hash
does not independently prove correct behavior or a human review; reports call these
**human-attested imports**. Keep sensitive traces local and publish only reviewed summaries.

The JSON schemas in `examples/benchmarks/` describe the import shape. Timing and cost
must come from actual observations. Do not populate fields with convenient estimates.
Unreported latency/cost is `null`; direct-agent costs use `cost_source: "human_reported"`.
A refusal is `outcome: "defer", selected: null`; a failed call is an error, not a refusal.

## Import workflow completion evidence separately

After actually executing each workflow, review its native verification, functional test,
or visual acceptance evidence. Import `WorkflowOutcomes` bound to the exact routing-run
SHA-256. Each observed case records:

- `status`: passed, failed, or unverifiable against the predeclared acceptance criteria.
- `completion_seconds`: observed total elapsed time, including corrections.
- `correction_seconds`: observed intervention/correction time, no greater than total time.
- `failure_count` and optional observed `tool_call_count`.
- `total_cost_usd`: actual total workflow cost, including routing, or null if unavailable.
- `evidence_sha256`: hash of the reviewed evidence artifact.

```python
from jev_unreal.benchmarks import import_workflow_outcomes, score_routing, load_answer_key

outcomes = import_workflow_outcomes(dataset, run, read_json("local-evidence/outcomes.json"))
key = load_answer_key("examples/benchmarks/public-synthetic-v1.answers.json", dataset)
report = score_routing(dataset, key, run, outcomes=outcomes)
```

Partial workflow evidence is allowed, but missing cases remain in the completion-rate
denominator and are reported separately. A good routing score never becomes workflow
completion. Unknown outcomes are not passes. Imported elapsed times and costs are
attested observations; the scorer does not manufacture them.

## Compare fairly and interpret cautiously

Use `compare_runs(dataset, key, [keyword_run, jev_run, direct_run], outcomes={run_sha: ...})`
to compare complete runs from one partition and catalog. The optional outcomes mapping
is keyed by `fingerprint(run)`. Reports separate total routing accuracy, recommendation
coverage, recommendation accuracy, abstentions, correct abstentions, recommendations
when defer was expected, errors, reported latency and reported cost. They separately
summarize workflow completion, corrections, failure events and tool calls.

Missing costs remain unknown, even on failed provider requests. Summed reported costs
are not a complete bill when coverage is incomplete. Do not add routing cost to a
workflow total that already includes it. Inference reports never retain raw responses,
probability distributions, API keys, prompts, or editor scene data.

The comparison deliberately emits no speedup conclusion. Credible efficiency evidence
requires repeated real tasks with matched project revisions, actor state, model versions,
prompts, hardware, tools, acceptance criteria and intervention policy. Record failures
as well as successes and report uncertainty. These helpers collect comparable evidence;
they cannot make an alpha plugin production-ready by themselves.
