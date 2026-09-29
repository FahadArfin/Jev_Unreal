"""Serial routing-off/on workflow runner and conservative saved-observation reports.

Drivers are trusted local Python adapters, never executable bridge inputs. The
bundled demo is entirely synthetic and performs no provider or Unreal requests.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import statistics
import time
from collections import Counter
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Literal, Protocol

from pydantic import Field, ValidationError, model_validator

from .benchmarks import Digest, Identifier, StrictModel, ToolId, fingerprint, read_json
from .errors import JevError

Arm = Literal["routing_off", "routing_on"]
MAX_TRIALS = 256


class PairedTask(StrictModel):
    id: Identifier
    goal: str = Field(min_length=1, max_length=8000)
    initial_state_sha256: Digest
    acceptance_sha256: Digest


class PairedProtocol(StrictModel):
    schema_version: Literal[1] = 1
    id: Identifier
    provenance: Literal["synthetic_demo", "authored_pilot", "independent_study"]
    model_id: str = Field(min_length=1, max_length=160)
    agent_configuration_sha256: Digest
    environment_sha256: Digest
    project_id: str = Field(min_length=1, max_length=1024)
    catalog_sha256: Digest
    allowed_tools: list[ToolId] = Field(min_length=1, max_length=128)
    tasks: list[PairedTask] = Field(min_length=1, max_length=64)
    repetitions: int = Field(default=2, ge=1, le=20)
    seed: int = Field(default=0, ge=0, le=2**32 - 1)
    max_tool_calls: int = Field(default=8, ge=1, le=64)
    trial_timeout_seconds: float = Field(default=60.0, gt=0, le=300)
    total_timeout_seconds: float = Field(default=600.0, gt=0, le=3600)
    cache_policy: Literal["cold", "warm", "record_only"] = "record_only"

    @model_validator(mode="after")
    def unique_and_bounded(self):
        if len({task.id for task in self.tasks}) != len(self.tasks):
            raise ValueError("Task IDs must be unique.")
        if len(set(self.allowed_tools)) != len(self.allowed_tools):
            raise ValueError("Allowed tool IDs must be unique.")
        if len(self.tasks) * self.repetitions * 2 > MAX_TRIALS:
            raise ValueError("Protocol exceeds 256 trials.")
        return self


class PairedTrial(StrictModel):
    id: Identifier
    task_id: Identifier
    repetition: int = Field(ge=1, le=20)
    arm: Arm
    sequence: int = Field(ge=1, le=MAX_TRIALS)


def schedule(protocol: PairedProtocol) -> list[PairedTrial]:
    """Alternate first arm across blocks and repetitions; deterministic seed parity."""
    trials = []
    for repetition in range(1, protocol.repetitions + 1):
        for index, task in enumerate(protocol.tasks):
            arms = ["routing_off", "routing_on"]
            if (protocol.seed + repetition - 1 + index) % 2:
                arms.reverse()
            for arm in arms:
                trials.append(
                    PairedTrial(
                        id=f"trial-{len(trials) + 1:04d}",
                        task_id=task.id,
                        repetition=repetition,
                        arm=arm,
                        sequence=len(trials) + 1,
                    )
                )
    return trials


class TrialIdentity(StrictModel):
    """Observed before each trial, after restoring state and clearing agent history."""

    model_id: str = Field(min_length=1, max_length=160)
    agent_configuration_sha256: Digest
    environment_sha256: Digest
    project_id: str = Field(min_length=1, max_length=1024)
    catalog_sha256: Digest
    initial_state_sha256: Digest
    fresh_context_id: Identifier
    cache_policy: Literal["cold", "warm", "record_only"]


class Usage(StrictModel):
    """Task interval only; missing telemetry stays unknown, never becomes zero."""

    agent_input_tokens: int | None = Field(default=None, ge=0)
    agent_cached_input_tokens: int | None = Field(default=None, ge=0)
    agent_output_tokens: int | None = Field(default=None, ge=0)
    agent_cost_usd: float | None = Field(default=None, ge=0)
    agent_cost_source: Literal["provider_reported", "estimated"] | None = None

    @model_validator(mode="after")
    def consistent_usage(self):
        if (self.agent_cost_usd is None) != (self.agent_cost_source is None):
            raise ValueError("Agent cost requires its provenance, and vice versa.")
        if (
            self.agent_cached_input_tokens is not None
            and self.agent_input_tokens is not None
            and self.agent_cached_input_tokens > self.agent_input_tokens
        ):
            raise ValueError("Cached input is a subset of total input tokens.")
        return self


class RouteObservation(StrictModel):
    outcome: Literal["recommend", "defer", "authentication_failed", "error"]
    selected_tool: ToolId | None = None
    cached: bool | None = None
    provider_requests: int | None = Field(default=None, ge=0, le=1)
    provider_cost_usd: float | None = Field(default=None, ge=0)
    provider_cost_source: Literal["provider_reported", "estimated"] | None = None

    @model_validator(mode="after")
    def consistent_route(self):
        if (self.outcome == "recommend") != (self.selected_tool is not None):
            raise ValueError("Only recommendations name a selected tool.")
        if (self.provider_cost_usd is None) != (self.provider_cost_source is None):
            raise ValueError("Provider cost requires provenance, and vice versa.")
        return self


class DriverResult(StrictModel):
    answer_sha256: Digest
    usage: Usage = Field(default_factory=Usage)


class Verification(StrictModel):
    passed: bool
    evidence_sha256: Digest
    acceptance_sha256: Digest
    project_id: str = Field(min_length=1, max_length=1024)


class ToolMeasurement(StrictModel):
    tool: ToolId
    elapsed_ms: float = Field(ge=0)
    succeeded: bool
    result_bytes: int | None = Field(default=None, ge=0)


class PairedObservation(StrictModel):
    trial_id: Identifier
    status: Literal[
        "passed",
        "verification_failed",
        "driver_error",
        "timed_out",
        "cancelled",
        "identity_mismatch",
        "budget_exhausted",
        "provider_failed",
        "blocked",
        "missing",
    ]
    failure_code: (
        Literal[
            "authentication_failed",
            "provider_error",
            "driver_error",
            "timed_out",
            "cancelled",
            "identity_mismatch",
            "tool_budget",
            "tool_not_allowed",
            "verification_failed",
            "total_budget",
            "missing",
            "concurrent_tool_calls",
        ]
        | None
    ) = None
    identity: TrialIdentity | None = None
    setup_ms: float | None = Field(default=None, ge=0)
    task_ms: float | None = Field(default=None, ge=0)
    verification_ms: float | None = Field(default=None, ge=0)
    route_ms: float | None = Field(default=None, ge=0)
    route: RouteObservation | None = None
    tools: list[ToolMeasurement] = Field(default_factory=list, max_length=64)
    usage: Usage = Field(default_factory=Usage)
    verification: Verification | None = None
    answer_sha256: Digest | None = None

    @model_validator(mode="after")
    def pass_requires_evidence(self):
        if self.status == "passed":
            if (
                self.verification is None
                or not self.verification.passed
                or self.identity is None
                or self.answer_sha256 is None
                or self.task_ms is None
                or self.failure_code is not None
            ):
                raise ValueError("Passed trials require identity, timing, answer and verification.")
        elif self.failure_code is None:
            raise ValueError("Non-passing trials require an explicit failure code.")
        return self


class PairedRun(StrictModel):
    schema_version: Literal[1] = 1
    protocol_sha256: Digest
    evidence_origin: Literal["local_driver", "imported_observations", "synthetic_demo"]
    observations: list[PairedObservation] = Field(default_factory=list, max_length=MAX_TRIALS)


class TrialDriver(Protocol):
    """Trusted adapter: prepare restores identical state and uses a fresh agent.

    route performs at most one Decisions request, with retries disabled. execute
    must send every task tool call through context.call_tool. verify independently
    measures acceptance. No arbitrary commands are supplied by the protocol.
    """

    async def prepare(self, task: PairedTask) -> TrialIdentity: ...
    async def route(self, task: PairedTask) -> RouteObservation: ...
    async def call_tool(self, name: str, arguments: dict[str, Any]) -> Any: ...
    async def execute(
        self,
        task: PairedTask,
        context: TrialContext,
        recommendation: RouteObservation | None,
    ) -> DriverResult: ...
    async def verify(self, task: PairedTask, answer_sha256: str) -> Verification: ...


class TrialContractError(Exception):
    def __init__(self, code: str):
        self.code = code
        super().__init__(code)


@dataclass(frozen=True)
class ToolReply:
    """Explicit success for adapters whose tool transport has a custom envelope."""

    succeeded: bool
    value: Any


def _error_reply(value: Any) -> bool:
    """Recognize standard native/MCP replies without retaining raw reply content."""
    if isinstance(value, Mapping):
        if value.get("ok") is False or value.get("isError") is True:
            return True
        structured, content = value.get("structuredContent"), value.get("content")
    else:
        if getattr(value, "isError", False) is True:
            return True
        structured, content = (
            getattr(value, "structuredContent", None),
            getattr(value, "content", None),
        )
    if isinstance(structured, Mapping) and (
        structured.get("ok") is False or structured.get("isError") is True
    ):
        return True
    if isinstance(content, list):
        for item in content:
            text = item.get("text") if isinstance(item, Mapping) else getattr(item, "text", None)
            if isinstance(text, str) and len(text) <= 2 * 1024 * 1024:
                try:
                    parsed = json.loads(text)
                except (ValueError, RecursionError):
                    continue
                if isinstance(parsed, Mapping) and (
                    parsed.get("ok") is False or parsed.get("isError") is True
                ):
                    return True
    return False


class TrialContext:
    """All adapter task tools pass this local allowlist and serial call budget."""

    def __init__(self, protocol: PairedProtocol, driver: TrialDriver):
        self._protocol = protocol
        self._driver = driver
        self._in_flight = False
        self.violation: str | None = None
        self.measurements: list[ToolMeasurement] = []

    async def call_tool(self, name: str, arguments: dict[str, Any]) -> Any:
        code = None
        if self._in_flight:
            code = "concurrent_tool_calls"
        elif name not in self._protocol.allowed_tools:
            code = "tool_not_allowed"
        elif len(self.measurements) >= self._protocol.max_tool_calls:
            code = "tool_budget"
        if code:
            self.violation = code
            raise TrialContractError(code)
        measurement = ToolMeasurement(tool=name, elapsed_ms=0, succeeded=False)
        self.measurements.append(measurement)
        self._in_flight = True
        started = time.perf_counter()
        try:
            result = await self._driver.call_tool(name, arguments)
            explicit = result.succeeded if isinstance(result, ToolReply) else True
            if isinstance(result, ToolReply):
                result = result.value
            measurement.succeeded = explicit and not _error_reply(result)
            try:
                measurement.result_bytes = len(json.dumps(result, allow_nan=False).encode("utf-8"))
            except (TypeError, ValueError):
                pass
            return result
        finally:
            measurement.elapsed_ms = (time.perf_counter() - started) * 1000
            self._in_flight = False


def _identity_matches(protocol: PairedProtocol, task: PairedTask, identity: TrialIdentity) -> bool:
    fields = (
        "model_id",
        "agent_configuration_sha256",
        "environment_sha256",
        "project_id",
        "catalog_sha256",
        "cache_policy",
    )
    return all(getattr(identity, key) == getattr(protocol, key) for key in fields) and (
        identity.initial_state_sha256 == task.initial_state_sha256
    )


async def run_paired(
    protocol: PairedProtocol,
    factory: Callable[[PairedTrial], TrialDriver],
    *,
    evidence_origin: Literal["local_driver", "synthetic_demo"] = "local_driver",
) -> PairedRun:
    """Run serial bounded trials; retain failures and stop ON trials after auth failure.

    Timeouts require cancellation-cooperative adapters. Cancellation stops the run
    and preserves completed observations; remaining trials are explicitly missing.
    """
    observations = []
    task_map = {task.id: task for task in protocol.tasks}
    seen_contexts: set[str] = set()
    deadline = time.perf_counter() + protocol.total_timeout_seconds
    auth_failed = False
    cancelled = False
    for trial in schedule(protocol):
        task = task_map[trial.task_id]
        row = PairedObservation(trial_id=trial.id, status="missing", failure_code="missing")
        observations.append(row)
        if cancelled:
            continue
        if auth_failed and trial.arm == "routing_on":
            row.status, row.failure_code = "blocked", "authentication_failed"
            continue
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            row.status, row.failure_code = "budget_exhausted", "total_budget"
            continue
        context = None
        phase = "setup"
        started = time.perf_counter()
        try:
            async with asyncio.timeout(min(protocol.trial_timeout_seconds, remaining)):
                driver = factory(trial)
                context = TrialContext(protocol, driver)
                identity = await driver.prepare(task)
                row.identity = identity
                row.setup_ms = (time.perf_counter() - started) * 1000
                if not _identity_matches(protocol, task, identity) or (
                    identity.fresh_context_id in seen_contexts
                ):
                    raise TrialContractError("identity_mismatch")
                seen_contexts.add(identity.fresh_context_id)
                phase, started = "task", time.perf_counter()
                if trial.arm == "routing_on":
                    route_started = time.perf_counter()
                    try:
                        row.route = await driver.route(task)
                    finally:
                        row.route_ms = (time.perf_counter() - route_started) * 1000
                    if row.route.outcome in {"authentication_failed", "error"}:
                        auth_failed = row.route.outcome == "authentication_failed"
                        row.status = "provider_failed"
                        row.failure_code = (
                            "authentication_failed" if auth_failed else "provider_error"
                        )
                        continue
                    if row.route.selected_tool not in [None, *protocol.allowed_tools]:
                        raise TrialContractError("tool_not_allowed")
                result = await driver.execute(task, context, row.route)
                row.answer_sha256, row.usage = result.answer_sha256, result.usage
                if context.violation:
                    raise TrialContractError(context.violation)
                row.task_ms = (time.perf_counter() - started) * 1000
                phase, started = "verification", time.perf_counter()
                row.verification = await driver.verify(task, result.answer_sha256)
                if row.verification.project_id != protocol.project_id:
                    raise TrialContractError("identity_mismatch")
                if row.verification.acceptance_sha256 != task.acceptance_sha256:
                    raise TrialContractError("identity_mismatch")
                row.verification_ms = (time.perf_counter() - started) * 1000
                if not row.verification.passed:
                    row.status, row.failure_code = "verification_failed", "verification_failed"
                else:
                    row.status, row.failure_code = "passed", None
        except TimeoutError:
            row.status, row.failure_code = "timed_out", "timed_out"
        except asyncio.CancelledError:
            row.status, row.failure_code = "cancelled", "cancelled"
            cancelled = True
        except TrialContractError as error:
            row.status = (
                "identity_mismatch" if error.code == "identity_mismatch" else "driver_error"
            )
            row.failure_code = error.code
        except Exception:
            # Adapter exceptions can contain keys or private provider payloads.
            row.status, row.failure_code = "driver_error", "driver_error"
        finally:
            setattr(row, f"{phase}_ms", (time.perf_counter() - started) * 1000)
            if context:
                row.tools = context.measurements
    return PairedRun(
        protocol_sha256=fingerprint(protocol),
        evidence_origin=evidence_origin,
        observations=observations,
    )


def report(protocol: PairedProtocol, run: PairedRun) -> dict:
    """Validate imported rows too; missing and failed trials stay in denominators."""
    if run.protocol_sha256 != fingerprint(protocol):
        raise ValueError("Run does not match the frozen protocol.")
    trials = schedule(protocol)
    by_id = {trial.id: trial for trial in trials}
    tasks = {task.id: task for task in protocol.tasks}
    rows = {row.trial_id: row for row in run.observations}
    if len(rows) != len(run.observations) or not set(rows).issubset(by_id):
        raise ValueError("Unknown or duplicate trial observations.")
    seen_contexts = set()
    for row in rows.values():
        # Revalidate models because runners update counters while measuring.
        PairedObservation.model_validate(row.model_dump())
        trial, task = by_id[row.trial_id], tasks[by_id[row.trial_id].task_id]
        if trial.arm == "routing_off" and (row.route is not None or row.route_ms is not None):
            raise ValueError("Routing-off trials cannot contain routing observations.")
        if len(row.tools) > protocol.max_tool_calls or any(
            call.tool not in protocol.allowed_tools for call in row.tools
        ):
            raise ValueError("Observed tools violate the frozen catalog or budget.")
        if row.status == "passed":
            if not _identity_matches(protocol, task, row.identity):
                raise ValueError("Passed trial identity does not match the protocol.")
            if row.identity.fresh_context_id in seen_contexts:
                raise ValueError("Passed trials reused an agent context.")
            seen_contexts.add(row.identity.fresh_context_id)
            if (
                row.verification.acceptance_sha256 != task.acceptance_sha256
                or row.verification.project_id != protocol.project_id
            ):
                raise ValueError("Passed verification does not match project and acceptance.")
            if trial.arm == "routing_on" and (
                row.route is None
                or row.route.outcome not in {"recommend", "defer"}
                or row.route.selected_tool not in [None, *protocol.allowed_tools]
            ):
                raise ValueError("Routing-on success requires a successful routing decision.")
    summaries = {}
    for arm in ("routing_off", "routing_on"):
        planned = [trial for trial in trials if trial.arm == arm]
        observed = [rows[trial.id] for trial in planned if trial.id in rows]
        passed = [row for row in observed if row.status == "passed"]
        summaries[arm] = {
            "planned": len(planned),
            "observed": len(observed),
            "started": sum(r.setup_ms is not None for r in observed),
            "passed": len(passed),
            "not_passed": len(planned) - len(passed),
            "failure_codes": dict(Counter(r.failure_code for r in observed if r.failure_code)),
            "missing": len(planned) - len(observed) + sum(r.status == "missing" for r in observed),
            "success_rate": len(passed) / len(planned),
            "task_time_ms": _metric([r.task_ms for r in observed], len(planned)),
            "tool_calls_recorded": sum(len(r.tools) for r in observed),
            "task_calls_including_routing": sum(
                len(r.tools) + int(r.route_ms is not None) for r in observed
            ),
            "tool_failures_recorded": sum(not c.succeeded for r in observed for c in r.tools),
            "setup_time_ms": _metric([r.setup_ms for r in observed], len(planned)),
            "verification_time_ms": _metric([r.verification_ms for r in observed], len(planned)),
            "agent_input_tokens": _metric(
                [r.usage.agent_input_tokens for r in observed], len(planned)
            ),
            "agent_cached_input_tokens": _metric(
                [r.usage.agent_cached_input_tokens for r in observed], len(planned)
            ),
            "agent_output_tokens": _metric(
                [r.usage.agent_output_tokens for r in observed], len(planned)
            ),
            "agent_cost_usd": _metric([r.usage.agent_cost_usd for r in observed], len(planned)),
            "agent_cost_by_source": {
                source: _metric(
                    [
                        r.usage.agent_cost_usd if r.usage.agent_cost_source == source else None
                        for r in observed
                    ],
                    len(planned),
                )
                for source in ("provider_reported", "estimated")
            },
            "routing_attempts_recorded": sum(r.route_ms is not None for r in observed),
            "successful_routes": sum(
                r.route is not None and r.route.outcome in {"recommend", "defer"} for r in observed
            ),
            "route_cache_hits": sum(
                r.route is not None and r.route.cached is True for r in observed
            ),
            "route_time_ms": _metric([r.route_ms for r in observed], len(planned)),
            "provider_requests": _metric(
                [r.route.provider_requests if r.route else None for r in observed], len(planned)
            ),
            "provider_cost_usd": _metric(
                [r.route.provider_cost_usd if r.route else None for r in observed], len(planned)
            ),
            "provider_cost_by_source": {
                source: _metric(
                    [
                        r.route.provider_cost_usd
                        if r.route and r.route.provider_cost_source == source
                        else None
                        for r in observed
                    ],
                    len(planned),
                )
                for source in ("provider_reported", "estimated")
            },
        }
    deltas = []
    for off_trial in (t for t in trials if t.arm == "routing_off"):
        on_trial = next(
            t
            for t in trials
            if t.task_id == off_trial.task_id
            and t.repetition == off_trial.repetition
            and t.arm == "routing_on"
        )
        off, on = rows.get(off_trial.id), rows.get(on_trial.id)
        if off and on and off.status == on.status == "passed":
            deltas.append(
                {
                    "task_id": off_trial.task_id,
                    "repetition": off_trial.repetition,
                    "task_ms_on_minus_off": on.task_ms - off.task_ms,
                    "tool_calls_on_minus_off": len(on.tools) - len(off.tools),
                    "task_calls_including_routing_on_minus_off": (
                        len(on.tools) + int(on.route_ms is not None) - len(off.tools)
                    ),
                }
            )
    auth_failed = any(row.failure_code == "authentication_failed" for row in rows.values())
    finished = [
        row
        for row in rows.values()
        if row.setup_ms is not None
        and row.status
        not in {
            "missing",
            "blocked",
            "budget_exhausted",
            "cancelled",
        }
    ]
    collection_complete = len(finished) == len(trials)
    identities = [row.identity for row in rows.values() if row.identity is not None]
    identities_valid = (
        len(identities) == len(trials)
        and len({identity.fresh_context_id for identity in identities}) == len(trials)
        and all(
            row.identity is not None
            and row.status != "identity_mismatch"
            and _identity_matches(protocol, tasks[by_id[row.trial_id].task_id], row.identity)
            and (
                row.verification is None
                or (
                    row.verification.project_id == protocol.project_id
                    and row.verification.acceptance_sha256
                    == tasks[by_id[row.trial_id].task_id].acceptance_sha256
                )
            )
            for row in rows.values()
        )
    )
    exclusion_reasons = []
    if not collection_complete:
        exclusion_reasons.append("incomplete_collection")
    if not identities_valid:
        exclusion_reasons.append("invalid_or_unmeasured_trial_identity")
    if auth_failed:
        exclusion_reasons.append("authentication_failed")
    if run.evidence_origin == "synthetic_demo" or protocol.provenance == "synthetic_demo":
        exclusion_reasons.append("synthetic_demo")
    return {
        "schema_version": 1,
        "schedule_algorithm": "alternating-pairs-v1",
        "protocol_sha256": fingerprint(protocol),
        "evidence_origin": run.evidence_origin,
        "provenance": protocol.provenance,
        "planned_trials": len(trials),
        "arms": summaries,
        "authentication_failed": auth_failed,
        "collection_complete": collection_complete,
        "finished_trials": len(finished),
        "all_trial_identities_valid": identities_valid,
        "comparison_eligibility": {
            "eligible": not exclusion_reasons,
            "exclusion_reasons": exclusion_reasons,
            "interpretation": (
                "Compare all-trial success/failure rates; timing deltas cover matched successes."
                if not exclusion_reasons
                else "Descriptive partial or invalid evidence; do not infer a classifier advantage."
            ),
        },
        "valid_classifier_comparison": not exclusion_reasons,
        "matched_verified_pairs": len(deltas),
        "matched_verified_deltas": deltas,
        "limitations": [
            "Driver/imported observations are not independently audited evidence.",
            "Task timing includes routing and agent/tool work; setup/verification are separate.",
            "Matched-success deltas exclude failures; use all-trial success rates alongside them.",
            "Unknown telemetry and missing trials are not zero cost; no total bill is inferred.",
            "No statistical significance or advantage over a different Unreal MCP is established.",
        ],
    }


def _metric(values: list[float | int | None], planned: int) -> dict:
    known = [value for value in values if value is not None]
    return {
        "known_count": len(known),
        "unknown_count": planned - len(known),
        "known_sum": sum(known) if known else None,
        "median": statistics.median(known) if known else None,
    }


class SyntheticDriver:
    """Public deterministic unit-workflow demo; not an agent or engine benchmark."""

    def __init__(self, protocol: PairedProtocol, trial: PairedTrial):
        self.protocol, self.trial = protocol, trial

    async def prepare(self, task: PairedTask) -> TrialIdentity:
        values = self.protocol.model_dump(
            include={
                "model_id",
                "agent_configuration_sha256",
                "environment_sha256",
                "project_id",
                "catalog_sha256",
                "cache_policy",
            }
        )
        return TrialIdentity(
            **values, initial_state_sha256=task.initial_state_sha256, fresh_context_id=self.trial.id
        )

    async def route(self, task: PairedTask) -> RouteObservation:
        return RouteObservation(
            outcome="recommend",
            selected_tool=self.protocol.allowed_tools[0],
            cached=False,
            provider_requests=0,
        )

    async def call_tool(self, name: str, arguments: dict[str, Any]) -> Any:
        return {"bounds_size_cm": [100, 100, 100]}

    async def execute(self, task, context, recommendation) -> DriverResult:
        result = await context.call_tool(self.protocol.allowed_tools[0], {})
        return DriverResult(answer_sha256=fingerprint(result))

    async def verify(self, task: PairedTask, answer_sha256: str) -> Verification:
        evidence = {"bounds_size_cm": [100, 100, 100]}
        return Verification(
            passed=answer_sha256 == fingerprint(evidence),
            evidence_sha256=fingerprint(evidence),
            acceptance_sha256=task.acceptance_sha256,
            project_id=self.protocol.project_id,
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["schedule", "template", "report", "demo"])
    parser.add_argument("--protocol", required=True, type=Path)
    parser.add_argument("--observations", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        protocol = PairedProtocol.model_validate(read_json(args.protocol))
        if args.command == "schedule":
            result = {
                "protocol_sha256": fingerprint(protocol),
                "trials": [trial.model_dump() for trial in schedule(protocol)],
            }
        elif args.command == "template":
            result = PairedRun(
                protocol_sha256=fingerprint(protocol),
                evidence_origin="imported_observations",
                observations=[
                    PairedObservation(
                        trial_id=trial.id,
                        status="missing",
                        failure_code="missing",
                    )
                    for trial in schedule(protocol)
                ],
            ).model_dump(mode="json")
        elif args.command == "report":
            if args.observations is None:
                raise ValueError("Report requires --observations.")
            run = PairedRun.model_validate(read_json(args.observations))
            result = report(protocol, run)
        else:
            if protocol.provenance != "synthetic_demo":
                raise ValueError("Demo only accepts a synthetic_demo protocol.")
            run = asyncio.run(
                run_paired(
                    protocol,
                    lambda trial: SyntheticDriver(protocol, trial),
                    evidence_origin="synthetic_demo",
                )
            )
            result = run.model_dump(mode="json")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8"
        )
    except (ValueError, ValidationError, OSError, JevError):
        # Never include input excerpts, file paths or adapter exception payloads.
        print("Paired benchmark failed: invalid input, contract, or output path.")
        return 2
    print(f"Paired benchmark {args.command} complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
