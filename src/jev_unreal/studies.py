"""Preregistered, paired full-workflow evidence; no provider or editor execution.

This module schedules counterbalanced trials and checks local evidence hashes.
It never converts an authored pilot or an importer's attestation into an
independent user study, measured scene state, or a human review.
"""

from __future__ import annotations

import argparse
import itertools
import json
import stat
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Literal

from pydantic import Field, ValidationError, model_validator

from .benchmarks import (
    Cost,
    Digest,
    Identifier,
    Method,
    Seconds,
    StrictModel,
    fingerprint,
    hash_evidence,
    read_json,
)
from .errors import JevError

METHODS = ("direct_agent", "keyword", "jev")
MAX_TRIALS = 4096


class Criterion(StrictModel):
    id: Identifier
    evaluation: Literal["automated", "human"]


class StudyTask(StrictModel):
    id: Identifier
    initial_state_sha256: Digest
    acceptance_criteria_sha256: Digest
    goal_sha256: Digest
    timeout_seconds: Seconds = Field(gt=0)
    criteria: list[Criterion] = Field(min_length=1, max_length=32)

    @model_validator(mode="after")
    def unique_criteria(self):
        if len({item.id for item in self.criteria}) != len(self.criteria):
            raise ValueError("Criterion IDs must be unique within a task.")
        return self


class MethodConfiguration(StrictModel):
    method: Method
    implementation: Identifier
    configuration_sha256: Digest


class StudyProtocol(StrictModel):
    schema_version: Literal[1] = 1
    id: Identifier
    provenance: Literal["authored_synthetic_pilot", "real_project_pilot", "independent_study"]
    seed: int = Field(ge=0, le=2**32 - 1)
    participants: list[Identifier] = Field(min_length=1, max_length=32)
    repetitions: int = Field(ge=1, le=20)
    tasks: list[StudyTask] = Field(min_length=1, max_length=64)
    methods: list[MethodConfiguration] = Field(min_length=3, max_length=3)
    environment_sha256: Digest
    allowed_tools_sha256: Digest
    intervention_policy_sha256: Digest

    @model_validator(mode="after")
    def bounded_unique_protocol(self):
        if len(set(self.participants)) != len(self.participants):
            raise ValueError("Participant IDs must be unique pseudonyms.")
        if len({task.id for task in self.tasks}) != len(self.tasks):
            raise ValueError("Task IDs must be unique.")
        if {method.method for method in self.methods} != set(METHODS):
            raise ValueError("Declare direct_agent, keyword and jev exactly once.")
        if len(self.participants) * self.repetitions * len(self.tasks) * 3 > MAX_TRIALS:
            raise ValueError("The protocol exceeds 4096 planned trials.")
        return self


class Trial(StrictModel):
    trial_id: Identifier
    block_id: Identifier
    participant_id: Identifier
    task_id: Identifier
    repetition: int = Field(ge=1, le=20)
    method: Method
    position_in_block: int = Field(ge=1, le=3)
    sequence: int = Field(ge=1, le=MAX_TRIALS)


class StudyManifest(StrictModel):
    schema_version: Literal[1] = 1
    algorithm: Literal["sha256-balanced-v1"] = "sha256-balanced-v1"
    protocol_sha256: Digest
    protocol: StudyProtocol
    trials: list[Trial] = Field(min_length=3, max_length=MAX_TRIALS)


class CriterionResult(StrictModel):
    criterion_id: Identifier
    status: Literal["passed", "failed", "not_measured"]
    evidence_kind: Literal["automated_observation", "human_attested"]
    evidence_sha256: Digest
    reviewer_id: Identifier | None = None

    @model_validator(mode="after")
    def explicit_human_attestation(self):
        if (self.evidence_kind == "human_attested") != (self.reviewer_id is not None):
            raise ValueError("Only human-attested criteria require a reviewer ID.")
        return self


class StudyObservation(StrictModel):
    trial_id: Identifier
    attempt_id: Identifier
    method: Method
    initial_state_sha256: Digest
    acceptance_criteria_sha256: Digest
    configuration_sha256: Digest
    environment_sha256: Digest
    allowed_tools_sha256: Digest
    intervention_policy_sha256: Digest
    status: Literal["passed", "failed", "unverifiable", "timed_out", "cancelled"]
    elapsed_seconds: Seconds
    correction_seconds: Seconds
    failure_count: int = Field(ge=0, le=100000)
    tool_call_count: int = Field(ge=0, le=1000000)
    total_cost_usd: Cost | None = None
    cost_source: Literal["provider_metered", "human_reported", "local_no_provider"] | None = None
    cost_evidence_sha256: Digest | None = None
    trace_sha256: Digest
    evidence_kind: Literal["automated_observation", "human_attested"]
    reviewer_id: Identifier | None = None
    criteria: list[CriterionResult] = Field(min_length=1, max_length=32)

    @model_validator(mode="after")
    def consistent_measurements(self):
        if self.correction_seconds > self.elapsed_seconds:
            raise ValueError("Correction time must be included in total elapsed time.")
        reported = self.total_cost_usd is not None
        if reported != (self.cost_source is not None) or reported != (
            self.cost_evidence_sha256 is not None
        ):
            raise ValueError("Reported total cost needs a source and evidence hash.")
        if self.cost_source == "local_no_provider" and self.total_cost_usd != 0:
            raise ValueError("No-provider cost must be zero.")
        if (self.evidence_kind == "human_attested") != (self.reviewer_id is not None):
            raise ValueError("Human-attested observations need an explicit reviewer ID.")
        if len({item.criterion_id for item in self.criteria}) != len(self.criteria):
            raise ValueError("Criterion observations must be unique.")
        if self.status == "passed" and any(item.status != "passed" for item in self.criteria):
            raise ValueError("A passed workflow must pass every predeclared criterion.")
        if self.status == "failed" and self.failure_count == 0:
            raise ValueError("Failed workflows must retain at least one failure event.")
        return self


class StudyObservations(StrictModel):
    schema_version: Literal[1] = 1
    manifest_sha256: Digest
    observations: list[StudyObservation] = Field(max_length=MAX_TRIALS)


def _key(seed: int | str, *parts: object) -> str:
    return fingerprint([seed, *parts])


def create_manifest(protocol: dict) -> dict:
    """Freeze all three methods per task/repetition with counterbalanced order.

    All six method permutations occur once per six consecutive paired blocks.
    Their order, and task/participant order, use portable SHA-256 sorting.
    """
    parsed = StudyProtocol.model_validate(protocol)
    digest = fingerprint(parsed)
    blocks = [
        (participant, task.id, repetition)
        for participant in parsed.participants
        for repetition in range(1, parsed.repetitions + 1)
        for task in parsed.tasks
    ]
    blocks.sort(key=lambda item: _key(parsed.seed, "block", *item))
    permutations = list(itertools.permutations(METHODS))
    trials = []
    for index, (participant, task, repetition) in enumerate(blocks):
        cycle = index // 6
        orders = sorted(permutations, key=lambda item: _key(parsed.seed, "order", cycle, *item))
        block_id = "block-" + _key(digest, participant, task, repetition)[:24]
        for position, method in enumerate(orders[index % 6], 1):
            trials.append(
                Trial(
                    trial_id="trial-" + _key(digest, block_id, method)[:24],
                    block_id=block_id,
                    participant_id=participant,
                    task_id=task,
                    repetition=repetition,
                    method=method,
                    position_in_block=position,
                    sequence=len(trials) + 1,
                )
            )
    return StudyManifest(protocol=parsed, protocol_sha256=digest, trials=trials).model_dump()


def _manifest(data: dict) -> StudyManifest:
    parsed = StudyManifest.model_validate(data)
    if create_manifest(parsed.protocol.model_dump()) != parsed.model_dump():
        raise JevError("study_contract", "The manifest differs from its frozen trial schedule.")
    return parsed


def _required_evidence(protocol: StudyProtocol, observations: list[StudyObservation]) -> set[str]:
    required = {
        protocol.environment_sha256,
        protocol.allowed_tools_sha256,
        protocol.intervention_policy_sha256,
        *(method.configuration_sha256 for method in protocol.methods),
    }
    for task in protocol.tasks:
        required.update(
            (task.initial_state_sha256, task.acceptance_criteria_sha256, task.goal_sha256)
        )
    for row in observations:
        required.add(row.trace_sha256)
        required.update(item.evidence_sha256 for item in row.criteria)
        if row.cost_evidence_sha256:
            required.add(row.cost_evidence_sha256)
    return required


def _verify_evidence(required: set[str], evidence_files: dict[str, str | Path]) -> None:
    if not isinstance(evidence_files, dict) or set(evidence_files) != required:
        raise JevError(
            "study_contract", "Supply exactly the local artifacts referenced by the study."
        )
    total = 0
    try:
        for digest, path in evidence_files.items():
            metadata = Path(path).stat()
            if not stat.S_ISREG(metadata.st_mode):
                raise JevError("study_contract", "Study evidence must be regular local files.")
            size = metadata.st_size
            total += size
            if size > 16 * 1024 * 1024 or total > 512 * 1024 * 1024:
                raise JevError("study_contract", "Study evidence exceeds its bounded byte budget.")
            if hash_evidence(path) != digest:
                raise JevError(
                    "study_contract", "A local evidence file changed or has the wrong hash."
                )
    except (OSError, TypeError, ValueError):
        raise JevError(
            "study_contract", "A local evidence artifact could not be checked."
        ) from None


def _validate_rows(manifest: StudyManifest, rows: list[StudyObservation]) -> None:
    trials = {trial.trial_id: trial for trial in manifest.trials}
    tasks = {task.id: task for task in manifest.protocol.tasks}
    configs = {method.method: method.configuration_sha256 for method in manifest.protocol.methods}
    if len({row.trial_id for row in rows}) != len(rows):
        raise JevError(
            "study_contract", "Duplicate trial outcomes cannot replace earlier failures."
        )
    if len({row.attempt_id for row in rows}) != len(rows):
        raise JevError("study_contract", "Each planned trial requires a unique attempt ID.")
    for row in rows:
        trial = trials.get(row.trial_id)
        if trial is None or trial.method != row.method:
            raise JevError(
                "study_contract", "An observation is outside the registered method/trial."
            )
        task = tasks[trial.task_id]
        expected = {
            "initial_state_sha256": task.initial_state_sha256,
            "acceptance_criteria_sha256": task.acceptance_criteria_sha256,
            "configuration_sha256": configs[trial.method],
            "environment_sha256": manifest.protocol.environment_sha256,
            "allowed_tools_sha256": manifest.protocol.allowed_tools_sha256,
            "intervention_policy_sha256": manifest.protocol.intervention_policy_sha256,
        }
        if any(getattr(row, name) != digest for name, digest in expected.items()):
            raise JevError(
                "study_contract",
                "Observed state, policy or configuration differs from registration.",
            )
        criteria = {item.id: item for item in task.criteria}
        if {item.criterion_id for item in row.criteria} != set(criteria):
            raise JevError(
                "study_contract", "Every predeclared acceptance criterion must be reported."
            )
        for item in row.criteria:
            if (
                criteria[item.criterion_id].evaluation == "human"
                and item.status != "not_measured"
                and item.evidence_kind != "human_attested"
            ):
                raise JevError(
                    "study_contract", "An automated result cannot claim human acceptance."
                )
        if row.status == "passed" and row.elapsed_seconds > task.timeout_seconds:
            raise JevError("study_contract", "A workflow past its registered deadline cannot pass.")


def _summary(trials: list[Trial], by_id: dict[str, StudyObservation]) -> dict:
    rows = [by_id[trial.trial_id] for trial in trials if trial.trial_id in by_id]
    counts = Counter(row.status for row in rows)
    costs = [row.total_cost_usd for row in rows if row.total_cost_usd is not None]
    return {
        "planned": len(trials),
        "observed": len(rows),
        "missing": len(trials) - len(rows),
        **{
            status: counts[status]
            for status in ("passed", "failed", "unverifiable", "timed_out", "cancelled")
        },
        "completion_rate": counts["passed"] / len(trials),
        "observed_elapsed_seconds_total": sum(row.elapsed_seconds for row in rows),
        "observed_elapsed_seconds_mean": statistics.mean(row.elapsed_seconds for row in rows)
        if rows
        else None,
        "observed_correction_seconds_total": sum(row.correction_seconds for row in rows),
        "observed_failure_events": sum(row.failure_count for row in rows),
        "observed_tool_calls": sum(row.tool_call_count for row in rows),
        "reported_total_cost_usd": sum(costs) if costs else None,
        "cost_observations": len(costs),
        "cost_missing": len(trials) - len(costs),
        "total_cost_complete": len(costs) == len(trials),
        "evidence_kinds": dict(Counter(row.evidence_kind for row in rows)),
        "criterion_evidence_kinds": dict(
            Counter(item.evidence_kind for row in rows for item in row.criteria)
        ),
    }


def _paired_metric(values: dict[str, list[float]], seed: str) -> dict:
    flat = [value for group in values.values() for value in group]
    participants = sorted(values)
    result = {
        "paired_observations": len(flat),
        "participants": len(participants),
        "mean_delta": statistics.mean(flat) if flat else None,
        "participant_cluster_bootstrap_95": None,
    }
    if len(participants) >= 2:
        means = []
        for replicate in range(1000):
            sample = []
            for draw in range(len(participants)):
                choice = int(_key(seed, replicate, draw), 16) % len(participants)
                sample.extend(values[participants[choice]])
            means.append(statistics.mean(sample))
        means.sort()
        result["participant_cluster_bootstrap_95"] = [means[24], means[974]]
    return result


def _comparisons(trials: list[Trial], by_id: dict[str, StudyObservation], seed: str) -> list[dict]:
    blocks = defaultdict(dict)
    for trial in trials:
        blocks[trial.block_id][trial.method] = trial
    comparisons = []
    for left, right in itertools.combinations(METHODS, 2):
        measured = {
            metric: defaultdict(list)
            for metric in ("elapsed_seconds", "correction_seconds", "total_cost_usd")
        }
        paired = both_passed = left_only = right_only = 0
        for block in blocks.values():
            ltrial, rtrial = block[left], block[right]
            lrow, rrow = by_id.get(ltrial.trial_id), by_id.get(rtrial.trial_id)
            if lrow is None or rrow is None:
                continue
            paired += 1
            both_passed += lrow.status == rrow.status == "passed"
            left_only += lrow.status == "passed" and rrow.status != "passed"
            right_only += rrow.status == "passed" and lrow.status != "passed"
            for metric, values in measured.items():
                lvalue, rvalue = getattr(lrow, metric), getattr(rrow, metric)
                if lvalue is not None and rvalue is not None:
                    values[ltrial.participant_id].append(rvalue - lvalue)
        comparisons.append(
            {
                "left": left,
                "right": right,
                "delta_direction": "right_minus_left",
                "planned_pairs": len(blocks),
                "observed_pairs": paired,
                "missing_pairs": len(blocks) - paired,
                "both_passed": both_passed,
                "left_only_passed": left_only,
                "right_only_passed": right_only,
                "metrics": {
                    metric: _paired_metric(values, _key(seed, left, right, metric))
                    for metric, values in measured.items()
                },
            }
        )
    return comparisons


def score_study(
    manifest: dict, observations: list[dict], *, evidence_files: dict[str, str | Path]
) -> dict:
    """Verify actual local artifacts and summarize all registered trials, including missing ones."""
    parsed = _manifest(manifest)
    bundle = StudyObservations(manifest_sha256=fingerprint(parsed), observations=observations)
    rows = bundle.observations
    _validate_rows(parsed, rows)
    required = _required_evidence(parsed.protocol, rows)
    _verify_evidence(required, evidence_files)
    by_id = {row.trial_id: row for row in rows}
    digest = fingerprint(parsed)
    return {
        "schema_version": 1,
        "manifest_sha256": digest,
        "observations_sha256": fingerprint(bundle),
        "study_id": parsed.protocol.id,
        "provenance_declared": parsed.protocol.provenance,
        "independence_verified": False,
        "human_review_verified": False,
        "evidence_hashes_checked": len(required),
        "trial_schedule_verified": True,
        "methods": {
            method: _summary([trial for trial in parsed.trials if trial.method == method], by_id)
            for method in METHODS
        },
        "paired_comparisons": _comparisons(parsed.trials, by_id, digest),
        "per_task": {
            task.id: {
                "methods": {
                    method: _summary(
                        [
                            trial
                            for trial in parsed.trials
                            if trial.task_id == task.id and trial.method == method
                        ],
                        by_id,
                    )
                    for method in METHODS
                },
                "paired_comparisons": _comparisons(
                    [trial for trial in parsed.trials if trial.task_id == task.id],
                    by_id,
                    _key(digest, task.id),
                ),
            }
            for task in parsed.protocol.tasks
        },
        "limitations": [
            "Hashes verify supplied file contents, not truthful measurement, a restored live "
            "scene, human review or independence.",
            "Elapsed time includes corrections and all observed outcomes, including failures; "
            "missing trials stay in completion denominators.",
            "Paired deltas use only measured pairs; missing costs are never zero "
            "or a complete bill.",
            "Intervals resample participants in 1000 deterministic bootstrap draws; "
            "fewer than two participants has no interval.",
            "Repeated authored tasks, carryover and non-independent participants can "
            "invalidate generalization; no speedup conclusion is emitted.",
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    plan = commands.add_parser("plan", help="Freeze a paired trial manifest")
    plan.add_argument("protocol")
    plan.add_argument("--output", required=True)
    score = commands.add_parser("score", help="Verify local evidence and score registered outcomes")
    score.add_argument("manifest")
    score.add_argument("observations", nargs="+")
    score.add_argument("--evidence-map", required=True)
    score.add_argument("--output", required=True)
    schema = commands.add_parser("schema")
    schema.add_argument("kind", choices=["protocol", "manifest", "observations"])
    args = parser.parse_args(argv)
    try:
        if args.command == "schema":
            model = {
                "protocol": StudyProtocol,
                "manifest": StudyManifest,
                "observations": StudyObservations,
            }[args.kind]
            print(json.dumps(model.model_json_schema(), indent=2))
            return 0
        if args.command == "plan":
            result = create_manifest(read_json(args.protocol))
        else:
            if len(args.observations) > 16:
                raise JevError("study_contract", "Import at most 16 bounded observation files.")
            manifest = _manifest(read_json(args.manifest))
            observations = []
            for path in args.observations:
                bundle = StudyObservations.model_validate(read_json(path))
                if bundle.manifest_sha256 != fingerprint(manifest):
                    raise JevError(
                        "study_contract", "An observation file belongs to another manifest."
                    )
                observations.extend(row.model_dump() for row in bundle.observations)
            result = score_study(
                manifest.model_dump(), observations, evidence_files=read_json(args.evidence_map)
            )
        # Exclusive output creation cannot replace a protocol, trace, or previous report.
        with Path(args.output).open("x", encoding="utf-8") as handle:
            json.dump(result, handle, indent=2, allow_nan=False)
            handle.write("\n")
        print(json.dumps({"status": "written", "sha256": fingerprint(result)}))
        return 0
    except (JevError, ValidationError, OSError):
        print(
            json.dumps(
                {
                    "error": "study_contract",
                    "message": "Study inputs, evidence or exclusive output failed validation; "
                    "no payloads printed.",
                }
            ),
            file=sys.stderr,
        )
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
