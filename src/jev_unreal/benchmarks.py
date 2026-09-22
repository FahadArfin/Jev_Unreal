"""Reproducible routing evaluation and separately evidenced workflow observations.

Inference receives a dataset without labels. Ground truth is loaded only by scoring.
No editor actions are executed, and provider payloads are never written to reports.
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import re
import sys
import time
import unicodedata
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError, field_validator, model_validator

from .config import Settings
from .decision import DecisionClient
from .errors import JevError
from .workflows import Candidate, route

MAX_FILE_BYTES = 2 * 1024 * 1024
Identifier = Annotated[str, Field(min_length=1, max_length=80, pattern=r"^[A-Za-z0-9_.:-]+$")]
ToolId = Annotated[str, Field(min_length=1, max_length=128, pattern=r"^[A-Za-z0-9_.:-]+$")]
Digest = Annotated[str, Field(pattern=r"^[a-f0-9]{64}$")]
Split = Literal["train", "dev", "heldout"]
Method = Literal["keyword", "jev", "direct_agent"]
Seconds = Annotated[float, Field(ge=0, le=604800)]
Cost = Annotated[float, Field(ge=0, le=1_000_000)]
ErrorCode = Literal[
    "missing_api_key",
    "request_limit",
    "circuit_open",
    "rate_limited",
    "provider_error",
    "provider_unavailable",
    "invalid_response",
    "invalid_request",
    "request_too_large",
    "benchmark_contract",
    "agent_error",
]


class StrictModel(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)

    @field_validator("schema_version", mode="before", check_fields=False)
    @classmethod
    def version_is_integer(cls, value):
        if type(value) is not int:
            raise ValueError("Schema version must be an integer.")
        return value

    @field_validator("human_reviewed", mode="before", check_fields=False)
    @classmethod
    def attestation_is_boolean(cls, value):
        if type(value) is not bool:
            raise ValueError("Review attestation must be a boolean.")
        return value


class BenchmarkCase(StrictModel):
    id: Identifier
    split: Split
    group_id: Identifier
    goal: str = Field(min_length=1, max_length=8000)

    @model_validator(mode="after")
    def goal_is_text(self):
        if not self.goal.strip() or any(ord(c) < 32 and c not in "\n\t" for c in self.goal):
            raise ValueError("Goals must contain nonblank text without control characters.")
        self.goal.encode("utf-8")
        return self


class BenchmarkDataset(StrictModel):
    schema_version: Literal[1] = 1
    id: Identifier
    provenance: Literal["public_synthetic", "real_project"]
    catalog: list[Candidate] = Field(min_length=1, max_length=64)
    cases: list[BenchmarkCase] = Field(min_length=1, max_length=256)

    @model_validator(mode="after")
    def unique_inputs_and_splits(self):
        catalog_ids = [candidate.id for candidate in self.catalog]
        if len(set(catalog_ids)) != len(catalog_ids) or "__defer__" in catalog_ids:
            raise ValueError("Catalog IDs must be unique; __defer__ is reserved.")
        ids = [case.id for case in self.cases]
        goals = [
            " ".join(unicodedata.normalize("NFC", c.goal).casefold().split()) for c in self.cases
        ]
        if len(set(ids)) != len(ids) or len(set(goals)) != len(goals):
            raise ValueError("Duplicate case IDs or normalized goals are not permitted.")
        groups: dict[str, str] = {}
        for case in self.cases:
            if case.group_id in groups and groups[case.group_id] != case.split:
                raise ValueError("A scenario group cannot cross train/dev/heldout partitions.")
            groups[case.group_id] = case.split
        return self


class AnswerLabel(StrictModel):
    case_id: Identifier
    accepted_tools: list[ToolId] = Field(default_factory=list, max_length=8)
    expect_defer: bool

    @model_validator(mode="after")
    def exclusive_defer(self):
        if self.expect_defer == bool(self.accepted_tools):
            raise ValueError("Defer labels require no accepted tools; other labels need tools.")
        if (
            len(set(self.accepted_tools)) != len(self.accepted_tools)
            or "__defer__" in self.accepted_tools
        ):
            raise ValueError("Use expect_defer, not a reserved or repeated tool label.")
        return self


class AnswerKey(StrictModel):
    schema_version: Literal[1] = 1
    dataset_sha256: Digest
    labels: list[AnswerLabel] = Field(min_length=1, max_length=256)

    @model_validator(mode="after")
    def unique_labels(self):
        if len({label.case_id for label in self.labels}) != len(self.labels):
            raise ValueError("Answer labels must have unique case IDs.")
        return self


class RoutingObservation(StrictModel):
    case_id: Identifier
    input_sha256: Digest
    outcome: Literal["recommend", "defer", "error"]
    selected: ToolId | None = None
    error_code: ErrorCode | None = None
    latency_ms: float | None = Field(default=None, ge=0, le=604800000)
    cost_usd: Cost | None = None
    cost_source: Literal["provider_reported", "human_reported", "local_no_provider"] | None = None
    cached: bool = False

    @model_validator(mode="after")
    def consistent_outcome(self):
        if (self.outcome == "recommend") != (self.selected is not None):
            raise ValueError("Only recommendations have a selected tool.")
        if (self.outcome == "error") != (self.error_code is not None):
            raise ValueError("Only errors have an error code.")
        if (self.cost_usd is None) != (self.cost_source is None):
            raise ValueError("A cost needs a source; unreported costs remain null.")
        if self.cost_source == "local_no_provider" and self.cost_usd != 0:
            raise ValueError("A local no-provider cost must be zero.")
        return self


class RoutingRun(StrictModel):
    schema_version: Literal[1] = 1
    dataset_sha256: Digest
    catalog_sha256: Digest
    split: Split
    method: Method
    implementation: Identifier
    model: str | None = Field(default=None, min_length=1, max_length=200)
    observations: list[RoutingObservation] = Field(min_length=1, max_length=256)
    human_reviewed: bool = False
    reviewer_id: Identifier | None = None
    evidence_sha256: Digest | None = None

    @model_validator(mode="after")
    def provenance_and_unique_cases(self):
        if len({row.case_id for row in self.observations}) != len(self.observations):
            raise ValueError("A run cannot contain repeated case IDs.")
        if self.method == "direct_agent":
            if not self.human_reviewed or not self.reviewer_id or not self.evidence_sha256:
                raise ValueError(
                    "Direct-agent imports require reviewed evidence and a reviewer ID."
                )
        elif (
            self.human_reviewed or self.reviewer_id is not None or self.evidence_sha256 is not None
        ):
            raise ValueError("Automated routing runs must not claim human-reviewed provenance.")
        return self


class WorkflowObservation(StrictModel):
    case_id: Identifier
    status: Literal["passed", "failed", "unverifiable"]
    completion_seconds: Seconds
    correction_seconds: Seconds
    failure_count: int = Field(ge=0, le=10000)
    tool_call_count: int | None = Field(default=None, ge=0, le=100000)
    total_cost_usd: Cost | None = None
    evidence_sha256: Digest

    @model_validator(mode="after")
    def consistent_times(self):
        if self.correction_seconds > self.completion_seconds:
            raise ValueError("Correction time must be included within total completion time.")
        if self.status == "failed" and self.failure_count < 1:
            raise ValueError("A failed workflow must record at least one failure.")
        return self


class WorkflowOutcomes(StrictModel):
    schema_version: Literal[1] = 1
    dataset_sha256: Digest
    routing_run_sha256: Digest
    human_reviewed: Literal[True]
    reviewer_id: Identifier
    observations: list[WorkflowObservation] = Field(min_length=1, max_length=256)

    @model_validator(mode="after")
    def unique_cases(self):
        if len({row.case_id for row in self.observations}) != len(self.observations):
            raise ValueError("Workflow observations must have unique case IDs.")
        return self


def fingerprint(value: BaseModel | Mapping | Sequence) -> str:
    """Canonical content hash; reproducibility binding, not a digital signature."""
    data = value.model_dump(mode="json") if isinstance(value, BaseModel) else value
    content = json.dumps(
        data, sort_keys=True, ensure_ascii=False, separators=(",", ":"), allow_nan=False
    ).encode("utf-8")
    return hashlib.sha256(content).hexdigest()


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Repeated JSON object key.")
        result[key] = value
    return result


def read_json(path: str | Path) -> dict:
    """Read bounded strict JSON without printing paths, payloads, or parser excerpts."""
    try:
        with Path(path).open("rb") as stream:
            raw = stream.read(MAX_FILE_BYTES + 1)
        if len(raw) > MAX_FILE_BYTES:
            raise ValueError
        data = json.loads(
            raw.decode("utf-8-sig"),
            object_pairs_hook=_unique_object,
            parse_constant=lambda _: (_ for _ in ()).throw(ValueError()),
        )
        if not isinstance(data, dict):
            raise ValueError
        return data
    except (OSError, UnicodeError, ValueError, RecursionError):
        raise JevError(
            "invalid_request", "Benchmark input must be strict JSON under 2 MiB."
        ) from None


def load_dataset(path: str | Path) -> BenchmarkDataset:
    return BenchmarkDataset.model_validate(read_json(path))


def load_answer_key(path: str | Path, dataset: BenchmarkDataset) -> AnswerKey:
    key = AnswerKey.model_validate(read_json(path))
    _validate_key(dataset, key)
    return key


def _validate_key(dataset: BenchmarkDataset, key: AnswerKey):
    if key.dataset_sha256 != fingerprint(dataset):
        raise JevError("invalid_request", "Answer key does not match the dataset fingerprint.")
    if {label.case_id for label in key.labels} != {case.id for case in dataset.cases}:
        raise JevError("invalid_request", "Answer key must label every dataset case exactly once.")
    tools = {candidate.id for candidate in dataset.catalog}
    if any(not set(label.accepted_tools) <= tools for label in key.labels):
        raise JevError("invalid_request", "An answer key tool is absent from the frozen catalog.")


def input_fingerprint(dataset: BenchmarkDataset, case: BenchmarkCase) -> str:
    # Inference never receives case ID, group ID, split, provenance, or answer labels.
    return fingerprint(
        {"goal": case.goal, "catalog": [candidate.model_dump() for candidate in dataset.catalog]}
    )


def _cases(dataset: BenchmarkDataset, split: Split) -> list[BenchmarkCase]:
    if split not in {"train", "dev", "heldout"}:
        raise JevError("invalid_request", "Select train, dev, or heldout.")
    cases = sorted(
        (case for case in dataset.cases if case.split == split), key=lambda case: case.id
    )
    if not cases:
        raise JevError("invalid_request", "The requested partition contains no cases.")
    return cases


def _run(
    dataset: BenchmarkDataset,
    split: Split,
    method: Method,
    observations: list[RoutingObservation],
    model: str | None = None,
) -> RoutingRun:
    return RoutingRun(
        dataset_sha256=fingerprint(dataset),
        catalog_sha256=fingerprint([candidate.model_dump() for candidate in dataset.catalog]),
        split=split,
        method=method,
        implementation="jev-benchmark-v1",
        model=model,
        observations=observations,
    )


def run_keyword(dataset: BenchmarkDataset, split: Split = "heldout") -> RoutingRun:
    """Fixed token-overlap baseline; ties and zero overlap abstain. Never reads labels."""
    stopwords = {
        "a",
        "an",
        "the",
        "to",
        "in",
        "of",
        "and",
        "or",
        "for",
        "with",
        "on",
        "my",
        "is",
        "are",
        "it",
        "this",
        "that",
        "please",
        "unreal",
        "editor",
    }

    def tokens(text: str) -> set[str]:
        return set(re.findall(r"[a-z0-9]+", text.casefold())) - stopwords

    rows = []
    for case in _cases(dataset, split):
        start = time.perf_counter()
        words = tokens(case.goal)
        scored = [
            (len(words & tokens(candidate.description)), candidate.id)
            for candidate in dataset.catalog
        ]
        maximum = max(score for score, _ in scored)
        winners = [tool for score, tool in scored if score == maximum]
        selected = winners[0] if maximum > 0 and len(winners) == 1 else None
        rows.append(
            RoutingObservation(
                case_id=case.id,
                input_sha256=input_fingerprint(dataset, case),
                outcome="recommend" if selected else "defer",
                selected=selected,
                latency_ms=(time.perf_counter() - start) * 1000,
                cost_usd=0.0,
                cost_source="local_no_provider",
            )
        )
    return _run(dataset, split, "keyword", rows)


async def run_jev(
    dataset: BenchmarkDataset,
    client: DecisionClient,
    split: Split = "heldout",
    *,
    request_budget: int = 12,
    allow_private_inputs: bool = False,
) -> RoutingRun:
    """At most 12 sequential Decisions calls; no retries or editor actions."""
    cases = _cases(dataset, split)
    if type(request_budget) is not int or not 1 <= request_budget <= 12:
        raise JevError("invalid_request", "The live smoke request budget must be an integer 1–12.")
    if len(cases) > request_budget:
        raise JevError("request_limit", "Partition exceeds the budget; no requests were sent.")
    if dataset.provenance != "public_synthetic" and allow_private_inputs is not True:
        raise JevError("invalid_request", "Explicitly allow private inputs before cloud inference.")
    if client.settings.cache_seconds != 0:
        raise JevError("invalid_request", "Use a fresh DecisionClient with cache_seconds=0.")
    if client.requests != 0 or not len(cases) <= client.settings.max_requests <= request_budget:
        raise JevError(
            "invalid_request", "Use a fresh client capped at the benchmark request budget."
        )
    model = client.settings.model
    if (
        not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._/:@+-]{0,199}", model)
        or model.startswith("sk-")
        or (client.settings.api_key and client.settings.api_key in model)
    ):
        raise JevError("invalid_request", "Invalid model metadata for a public benchmark report.")
    rows = []
    safe_errors = set(ErrorCode.__args__)
    for case in cases:
        start = time.perf_counter()
        try:
            result = await route(client, case.goal, dataset.catalog)
            selected = result.get("selected")
            outcome = result.get("outcome")
            if (
                result.get("executed") is not False
                or outcome not in {"recommend", "defer"}
                or (outcome == "recommend" and selected not in {c.id for c in dataset.catalog})
                or (outcome == "defer" and selected is not None)
                or result.get("cached") is True
            ):
                raise JevError("benchmark_contract", "Invalid sanitized routing outcome.")
            cost = result.get("usage", {}).get("cost")
            row = RoutingObservation(
                case_id=case.id,
                input_sha256=input_fingerprint(dataset, case),
                outcome=outcome,
                selected=selected,
                latency_ms=(time.perf_counter() - start) * 1000,
                cost_usd=cost,
                cost_source="provider_reported" if cost is not None else None,
            )
        except JevError as exc:
            row = RoutingObservation(
                case_id=case.id,
                input_sha256=input_fingerprint(dataset, case),
                outcome="error",
                error_code=exc.code if exc.code in safe_errors else "benchmark_contract",
                latency_ms=(time.perf_counter() - start) * 1000,
            )
        rows.append(row)
    return _run(dataset, split, "jev", rows, model=model)


def validate_run(dataset: BenchmarkDataset, run: RoutingRun):
    if run.dataset_sha256 != fingerprint(dataset) or run.catalog_sha256 != fingerprint(
        [candidate.model_dump() for candidate in dataset.catalog]
    ):
        raise JevError(
            "invalid_request", "Routing run dataset or catalog fingerprint does not match."
        )
    cases = {case.id: case for case in _cases(dataset, run.split)}
    if {row.case_id for row in run.observations} != set(cases):
        raise JevError("invalid_request", "A run must cover its complete partition exactly once.")
    tools = {candidate.id for candidate in dataset.catalog}
    for row in run.observations:
        if row.input_sha256 != input_fingerprint(dataset, cases[row.case_id]):
            raise JevError("invalid_request", "An observation does not match its inference inputs.")
        if row.selected is not None and row.selected not in tools:
            raise JevError("invalid_request", "An observed tool is absent from the frozen catalog.")
        if run.method == "keyword" and (row.cost_source != "local_no_provider" or row.cached):
            raise JevError("invalid_request", "Keyword runs must report no provider cost or cache.")
        if run.method == "jev" and (
            row.cost_source not in {None, "provider_reported"} or row.cached
        ):
            raise JevError(
                "invalid_request", "Jev benchmark runs must use uncached provider costs."
            )
        if run.method == "direct_agent" and row.cost_source not in {None, "human_reported"}:
            raise JevError("invalid_request", "Direct-agent cost imports require a human source.")


def hash_evidence(path: str | Path) -> str:
    """Hash one explicitly supplied evidence file (1 byte–16 MiB), without reading its text."""
    digest = hashlib.sha256()
    count = 0
    try:
        with Path(path).open("rb") as stream:
            while chunk := stream.read(65536):
                count += len(chunk)
                if count > 16 * 1024 * 1024:
                    raise ValueError
                digest.update(chunk)
        if not count:
            raise ValueError
    except (OSError, ValueError):
        raise JevError(
            "invalid_request", "Evidence must be a nonempty file under 16 MiB."
        ) from None
    return digest.hexdigest()


def _verify_evidence(required: set[str], files: dict[str, str | Path] | None):
    if files is None:
        return
    if set(files) != required:
        raise JevError(
            "invalid_request", "Supply exactly the evidence files referenced by this import."
        )
    if any(hash_evidence(path) != expected for expected, path in files.items()):
        raise JevError("invalid_request", "An evidence file does not match its declared SHA-256.")


def import_direct_agent_run(
    dataset: BenchmarkDataset, data: dict, *, evidence_files: dict[str, str | Path] | None = None
) -> RoutingRun:
    run = RoutingRun.model_validate(data)
    if run.method != "direct_agent":
        raise JevError("invalid_request", "This importer accepts only reviewed direct-agent runs.")
    validate_run(dataset, run)
    _verify_evidence({run.evidence_sha256}, evidence_files)
    return run


def import_workflow_outcomes(
    dataset: BenchmarkDataset,
    run: RoutingRun,
    data: dict,
    *,
    evidence_files: dict[str, str | Path] | None = None,
) -> WorkflowOutcomes:
    validate_run(dataset, run)
    outcomes = WorkflowOutcomes.model_validate(data)
    if outcomes.dataset_sha256 != fingerprint(
        dataset
    ) or outcomes.routing_run_sha256 != fingerprint(run):
        raise JevError(
            "invalid_request", "Workflow outcomes are not bound to this exact routing run."
        )
    if not {row.case_id for row in outcomes.observations} <= {
        row.case_id for row in run.observations
    }:
        raise JevError(
            "invalid_request", "Workflow outcomes contain cases outside the routing run."
        )
    _verify_evidence({row.evidence_sha256 for row in outcomes.observations}, evidence_files)
    return outcomes


def _measured(values: list[float | None]) -> dict:
    known = [value for value in values if value is not None]
    return {
        "reported_count": len(known),
        "missing_count": len(values) - len(known),
        "reported_sum": sum(known) if known else None,
        "mean_of_reported": sum(known) / len(known) if known else None,
    }


def score_routing(
    dataset: BenchmarkDataset,
    key: AnswerKey,
    run: RoutingRun,
    *,
    outcomes: WorkflowOutcomes | None = None,
) -> dict:
    _validate_key(dataset, key)
    validate_run(dataset, run)
    labels = {label.case_id: label for label in key.labels}
    rows = []
    for observation in run.observations:
        label = labels[observation.case_id]
        correct = (observation.outcome == "defer" and label.expect_defer) or (
            observation.outcome == "recommend" and observation.selected in label.accepted_tools
        )
        rows.append(
            {
                "case_id": observation.case_id,
                "correct": correct,
                "outcome": observation.outcome,
                "selected": observation.selected,
                "expected_defer": label.expect_defer,
            }
        )
    total = len(rows)
    recommendations = [row for row in rows if row["outcome"] == "recommend"]
    abstentions = [row for row in rows if row["outcome"] == "defer"]
    expected_defer = sum(row["expected_defer"] for row in rows)
    workflow = {
        "scope": "not_measured",
        "expected_cases": total,
        "observed_cases": 0,
        "passed": 0,
        "failed": 0,
        "unverifiable": 0,
        "missing": total,
        "completion_rate_over_all_cases": None,
        "note": "Routing correctness is not task completion or efficiency evidence.",
    }
    if outcomes is not None:
        # Revalidate bindings even if the caller bypassed the import helper.
        outcomes = import_workflow_outcomes(dataset, run, outcomes.model_dump())
        observations = outcomes.observations
        passed = sum(row.status == "passed" for row in observations)
        workflow = {
            "scope": "human_attested_import",
            "expected_cases": total,
            "observed_cases": len(observations),
            "passed": passed,
            "failed": sum(row.status == "failed" for row in observations),
            "unverifiable": sum(row.status == "unverifiable" for row in observations),
            "missing": total - len(observations),
            "completion_rate_over_all_cases": passed / total,
            "completion_seconds": _measured(
                [row.completion_seconds for row in observations]
                + [None] * (total - len(observations))
            ),
            "correction_seconds": _measured(
                [row.correction_seconds for row in observations]
                + [None] * (total - len(observations))
            ),
            "failure_events": sum(row.failure_count for row in observations),
            "total_workflow_cost_usd": _measured(
                [row.total_cost_usd for row in observations] + [None] * (total - len(observations))
            ),
            "tool_calls": _measured(
                [row.tool_call_count for row in observations] + [None] * (total - len(observations))
            ),
            "note": "Human attestations are imported, not independently verified behavior. "
            "Optional file-hash validation checks artifact identity only. Workflow cost includes "
            "routing; do not add it twice.",
        }
    return {
        "schema_version": 1,
        "dataset_id": dataset.id,
        "dataset_sha256": fingerprint(dataset),
        "answer_key_sha256": fingerprint(key),
        "routing_run_sha256": fingerprint(run),
        "provenance": dataset.provenance,
        "split": run.split,
        "method": run.method,
        "implementation": run.implementation,
        "model": run.model,
        "routing": {
            "cases": total,
            "correct": sum(row["correct"] for row in rows),
            "accuracy_over_all_cases": sum(row["correct"] for row in rows) / total,
            "recommendations": len(recommendations),
            "coverage": len(recommendations) / total,
            "recommendation_accuracy": (
                sum(row["correct"] for row in recommendations) / len(recommendations)
                if recommendations
                else None
            ),
            "abstentions": len(abstentions),
            "correct_abstentions": sum(row["correct"] for row in abstentions),
            "expected_defer_cases": expected_defer,
            "recommendations_when_defer_expected": sum(
                row["expected_defer"] for row in recommendations
            ),
            "errors": sum(row["outcome"] == "error" for row in rows),
            "latency_ms": _measured([row.latency_ms for row in run.observations]),
            "cost_usd": _measured([row.cost_usd for row in run.observations]),
            "rows": rows,
        },
        "workflow": workflow,
        "limitations": [
            "Partition labels and hashes do not prove an external, untouched holdout.",
            "No editor workflow runs in this benchmark. Completion requires separate evidence.",
            "Missing costs are unknown; failed calls may still incur unreported charges.",
            "Confidence is neither permission nor a security boundary.",
        ],
    }


def compare_runs(
    dataset: BenchmarkDataset,
    key: AnswerKey,
    runs: list[RoutingRun],
    *,
    outcomes: dict[str, WorkflowOutcomes] | None = None,
) -> dict:
    if not 2 <= len(runs) <= 12 or len({fingerprint(run) for run in runs}) != len(runs):
        raise JevError("invalid_request", "Compare 2–12 distinct complete routing runs.")
    if len({run.split for run in runs}) != 1:
        raise JevError("invalid_request", "Compared runs must use the same partition and catalog.")
    if outcomes and not set(outcomes) <= {fingerprint(run) for run in runs}:
        raise JevError("invalid_request", "A workflow outcome has no matching comparison run.")
    reports = [
        score_routing(dataset, key, run, outcomes=(outcomes or {}).get(fingerprint(run)))
        for run in runs
    ]
    return {
        "dataset_sha256": fingerprint(dataset),
        "split": runs[0].split,
        "reports": reports,
        "comparison_scope": "routing_and_separately_imported_workflow_evidence",
        "efficiency_conclusion": None,
        "note": "No speedup is inferred. Match project state, tools, prompts, hardware, operator "
        "intervention, and repetitions before interpreting workflow differences.",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate")
    validate.add_argument("dataset")
    validate.add_argument("--answer-key")
    for name in ("keyword", "jev"):
        command = commands.add_parser(name)
        command.add_argument("dataset")
        command.add_argument("--split", choices=("train", "dev", "heldout"), default="heldout")
        command.add_argument("--output", required=True)
        command.add_argument("--overwrite", action="store_true")
        if name == "jev":
            command.add_argument("--request-budget", type=int, default=12)
            command.add_argument("--allow-private-inputs", action="store_true")
    score = commands.add_parser("score")
    score.add_argument("dataset")
    score.add_argument("answer_key")
    score.add_argument("run")
    score.add_argument("--outcomes")
    score.add_argument("--output", required=True)
    score.add_argument("--overwrite", action="store_true")
    args = parser.parse_args(argv)
    try:
        dataset = load_dataset(args.dataset)
        if args.command == "validate":
            if args.answer_key:
                load_answer_key(args.answer_key, dataset)
            print(
                json.dumps(
                    {
                        "valid": True,
                        "dataset_sha256": fingerprint(dataset),
                        "cases_by_split": {
                            split: sum(c.split == split for c in dataset.cases)
                            for split in ("train", "dev", "heldout")
                        },
                    }
                )
            )
            return 0
        output = Path(args.output)
        inputs = {
            Path(getattr(args, key)).resolve()
            for key in ("dataset", "answer_key", "run", "outcomes")
            if getattr(args, key, None)
        }
        if output.resolve() in inputs or (output.exists() and not args.overwrite):
            raise JevError(
                "invalid_request", "Choose a new output path or explicitly overwrite it."
            )
        if args.command == "keyword":
            result = run_keyword(dataset, args.split).model_dump()
        elif args.command == "jev":
            from dataclasses import replace

            async def live():
                client = DecisionClient(
                    replace(Settings.from_env(), cache_seconds=0, max_requests=args.request_budget)
                )
                try:
                    return await run_jev(
                        dataset,
                        client,
                        args.split,
                        request_budget=args.request_budget,
                        allow_private_inputs=args.allow_private_inputs,
                    )
                finally:
                    await client.close()

            result = asyncio.run(live()).model_dump()
        else:
            key = load_answer_key(args.answer_key, dataset)
            run = RoutingRun.model_validate(read_json(args.run))
            outcomes = (
                import_workflow_outcomes(dataset, run, read_json(args.outcomes))
                if args.outcomes
                else None
            )
            result = score_routing(dataset, key, run, outcomes=outcomes)
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("w" if args.overwrite else "x", encoding="utf-8") as stream:
            stream.write(json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False) + "\n")
        print(json.dumps({"ok": True, "scope": "routing_only_no_editor_execution"}))
        return 0
    except (JevError, ValidationError, OSError, UnicodeError, ValueError, RecursionError):
        print(
            json.dumps({"ok": False, "error": "invalid_or_unavailable_benchmark_input"}),
            file=sys.stderr,
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
