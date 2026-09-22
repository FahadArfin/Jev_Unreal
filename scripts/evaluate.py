"""Evaluate routing with a real provider, or validate requests without running inference.

Examples:
    uv run python scripts/evaluate.py --validate-only
    uv run python scripts/evaluate.py --output artifacts/jev-evaluation.json

Credentials are read only from the normal provider environment variables. No Unreal
connection is needed: routing proposes a tool but never executes an editor command.
"""

import argparse
import asyncio
import hashlib
import json
import re
import sys
import time
from collections import Counter
from dataclasses import replace
from datetime import UTC, datetime
from pathlib import Path
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError, model_validator

from jev_unreal.config import Settings
from jev_unreal.decision import DecisionClient
from jev_unreal.errors import JevError
from jev_unreal.schema import request_body
from jev_unreal.workflows import CATALOG, route

ROOT = Path(__file__).resolve().parents[1]
LABELS = frozenset(CATALOG) | {"__defer__"}
LIMITATION = (
    "Small, manually authored smoke set, not a production benchmark or evidence of "
    "game-development efficiency. No held-out evaluation, threshold tuning, or "
    "statistical significance claim. The heuristic baseline is a simple keyword rule."
)
BASELINE_RULES = {
    "unsupported": r"\b(write|generate|implement|delete|package|upload|export)\b",
    "jev_triage": r"\b(log|triage|diagnose|diagnostic)\b",
    "unreal_preview": r"\b(preview|proposal|blockout)\b",
    "unreal_status": r"\b(status|session|connection|connected|identity|currently has open)\b",
    "unreal_actors": r"\b(actors?|transforms?|locations?|rotations?|scales?)\b",
    "unreal_assets": r"\b(assets?|registry|materials?|meshes|content browser)\b",
}


class EvaluationCase(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    id: str = Field(pattern=r"^[a-z0-9_]{1,64}$")
    group: Literal[
        "status", "actors", "assets", "preview", "triage", "unsupported", "ambiguous", "injection"
    ]
    goal: str = Field(min_length=1, max_length=8000)
    expected: str

    @model_validator(mode="after")
    def validate_case(self):
        if self.expected not in LABELS or not self.goal.strip():
            raise ValueError("Unknown expected label or empty goal")
        return self


class EvaluationSet(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    schema_version: Literal[1]
    description: str = Field(min_length=1, max_length=2000)
    cases: list[EvaluationCase] = Field(min_length=1, max_length=24)

    @model_validator(mode="after")
    def unique_ids(self):
        if len({case.id for case in self.cases}) != len(self.cases):
            raise ValueError("Case IDs must be unique")
        return self


def load_cases(path: Path) -> tuple[EvaluationSet, str]:
    try:
        content = path.read_bytes()
        if len(content) > 256 * 1024:
            raise ValueError("Dataset exceeds 256 KiB")
        dataset = EvaluationSet.model_validate_json(content)
    except (OSError, ValueError, ValidationError):
        # Pydantic's detailed error can include the submitted goal. Keep output bounded.
        raise ValueError("Evaluation data is unreadable or violates the case schema.") from None
    return dataset, hashlib.sha256(content).hexdigest()


class _RequestCaptured(Exception):
    pass


class RequestInspector:
    """Stop after real request validation; never fabricate a decision or call a provider."""

    def __init__(self, model: str):
        self.model = model
        self.input_bytes = 0

    async def decide(self, state, questions):
        body = request_body(state, questions, self.model)
        self.input_bytes = len(
            json.dumps(body, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode()
        )
        raise _RequestCaptured


async def validate_requests(dataset: EvaluationSet, model: str) -> list[int]:
    sizes = []
    for case in dataset.cases:
        inspector = RequestInspector(model)
        try:
            await route(inspector, case.goal)
        except _RequestCaptured:
            sizes.append(inspector.input_bytes)
        else:
            raise ValueError("Routing did not reach the request validator.")
    return sizes


def heuristic_route(goal: str) -> str:
    """Fixed precedence, no learned parameters and no provider: intentionally basic."""
    for label, pattern in BASELINE_RULES.items():
        if re.search(pattern, goal, re.IGNORECASE):
            return "__defer__" if label == "unsupported" else label
    return "__defer__"


def fraction(correct: int, total: int) -> dict:
    return {"correct": correct, "total": total, "ratio": correct / total if total else None}


def percentile(values: list[float], probability: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * probability
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return round(ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower), 3)


def baseline_report(dataset: EvaluationSet) -> dict:
    rows = [
        {"id": case.id, "expected": case.expected, "choice": heuristic_route(case.goal)}
        for case in dataset.cases
    ]
    return {
        "name": "Fixed keyword matching with first-match precedence",
        "rules_in_order": [
            {"label": label, "regex": rule} for label, rule in BASELINE_RULES.items()
        ],
        "caveat": "A transparent minimal baseline; not an optimized production router.",
        "provider_requests": 0,
        "correctness": fraction(sum(row["choice"] == row["expected"] for row in rows), len(rows)),
        "deferred": sum(row["choice"] == "__defer__" for row in rows),
        "cases": rows,
    }


async def evaluate(dataset: EvaluationSet, settings: Settings) -> dict:
    """Exactly one route invocation per case. Client has no retries; cache is disabled."""
    if not settings.api_key:
        raise JevError(
            "missing_api_key", "Set the provider API key locally before live evaluation."
        )
    client = DecisionClient(replace(settings, cache_seconds=0))
    rows = []
    try:
        for case in dataset.cases:
            start = time.perf_counter()
            row = {"id": case.id, "group": case.group, "expected": case.expected}
            before = client.requests
            try:
                result = await route(client, case.goal)
            except JevError as exc:
                row.update(outcome="error", error_code=exc.code)
            else:
                answer = result["answers"]["route"]
                row.update(
                    model=result["model"],
                    outcome=result["outcome"],
                    reason=result["reason"],
                    selected=result["selected"],
                    raw_choice=answer["choice"],
                    confidence=answer.get("confidence"),
                    probabilities=answer.get("probabilities"),
                    raw_choice_correct=answer["choice"] == case.expected,
                    final_choice_correct=(result["selected"] or "__defer__") == case.expected,
                    usage=result["usage"],
                )
            row["latency_ms"] = round((time.perf_counter() - start) * 1000, 3)
            row["provider_requests"] = client.requests - before
            rows.append(row)
        metrics = client.metrics()
    finally:
        await client.close()

    successful = [row for row in rows if row["outcome"] != "error"]
    accepted = [row for row in successful if row["outcome"] == "recommend"]
    latency = [row["latency_ms"] for row in successful]
    usage = {}
    for field in ("input_tokens", "output_tokens", "cost"):
        values = [row["usage"][field] for row in successful if field in row["usage"]]
        usage[field] = {
            "reported_total": sum(values) if values else None,
            "responses_reporting": len(values),
            "complete_for_all_cases": len(values) == len(rows),
        }
    return {
        "requested_model": settings.model,
        "provider": settings.provider,
        "models_returned": sorted({row["model"] for row in successful}),
        "provider_requests": metrics["requests_sent"],
        "cache_hits": metrics["cache_hits"],
        "cache_seconds": 0,
        "automatic_retries": 0,
        "case_count": len(rows),
        "successful_responses": len(successful),
        "accepted": len(accepted),
        "deferred": sum(row["outcome"] == "defer" for row in successful),
        "errors": len(rows) - len(successful),
        "accepted_coverage": len(accepted) / len(rows),
        "correctness_among_accepted": fraction(
            sum(row["final_choice_correct"] for row in accepted), len(accepted)
        ),
        "raw_choice_correctness": fraction(
            sum(row["raw_choice_correct"] for row in successful), len(successful)
        ),
        "gated_correctness": fraction(
            sum(row["final_choice_correct"] for row in successful), len(successful)
        ),
        "completed_correctly_among_all_cases": fraction(
            sum(row["final_choice_correct"] for row in successful), len(rows)
        ),
        "recommendations_on_expected_defer": sum(
            row["expected"] == "__defer__" for row in accepted
        ),
        "successful_response_latency_ms": {
            "p50": percentile(latency, 0.5),
            "p95": percentile(latency, 0.95),
            "percentile_method": "Linear interpolation at (n - 1) * quantile",
            "excludes_errors": True,
        },
        "usage": usage,
        "usage_note": (
            "Only provider-reported usage is summed. Missing cost is unknown, not zero. "
            "Failed calls may be billed without returning usable usage. "
            "Cost is provider-reported USD."
        ),
        "cases": rows,
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--cases", type=Path, default=ROOT / "examples/evaluation_cases.json")
    result.add_argument(
        "--validate-only", action="store_true", help="Validate inputs; no inference"
    )
    result.add_argument("--max-requests", type=int, default=24, help="Live request cap, 1 to 24")
    result.add_argument(
        "--max-input-bytes",
        type=int,
        default=24 * 65536,
        help="Total serialized request-byte budget before any provider call",
    )
    result.add_argument("--output", type=Path, help="Live default: artifacts/jev-evaluation.json")
    return result


async def run(args: argparse.Namespace) -> dict:
    dataset, digest = load_cases(args.cases)
    if not 1 <= args.max_requests <= 24 or len(dataset.cases) > args.max_requests:
        raise ValueError("Request budget must be 1 to 24 and cover every case.")
    if args.max_input_bytes < 1:
        raise ValueError("Input-byte budget must be positive.")
    # Validation-only must work without credentials, token files, or editor configuration.
    settings = Settings() if args.validate_only else Settings.from_env()
    settings = replace(settings, max_requests=min(settings.max_requests, args.max_requests))
    if len(dataset.cases) > settings.max_requests:
        raise ValueError("JEV_MAX_REQUESTS is lower than the number of evaluation cases.")
    sizes = await validate_requests(dataset, settings.model)
    if sum(sizes) > args.max_input_bytes:
        raise ValueError("Dataset exceeds the total request-byte budget.")
    routing_code = await asyncio.to_thread(Path(sys.modules[route.__module__].__file__).read_bytes)
    report = {
        "schema_version": 1,
        "created_at": datetime.now(UTC).isoformat(),
        "mode": "validation_only" if args.validate_only else "live_provider",
        "limitation": LIMITATION,
        "dataset_sha256": digest,
        "routing_code_sha256": hashlib.sha256(routing_code).hexdigest(),
        "case_count": len(dataset.cases),
        "groups": dict(Counter(case.group for case in dataset.cases)),
        "budget": {
            "max_provider_requests": settings.max_requests,
            "planned_provider_requests": 0 if args.validate_only else len(dataset.cases),
            "max_total_request_bytes": args.max_input_bytes,
            "total_request_bytes": sum(sizes),
            "token_budget_note": (
                "Byte counts are not token estimates; actual tokens appear in usage."
            ),
        },
        "baseline": baseline_report(dataset),
    }
    if not args.validate_only:
        report["jev"] = await evaluate(dataset, settings)
    output = args.output or (None if args.validate_only else ROOT / "artifacts/jev-evaluation.json")
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return report


def main() -> int:
    args = parser().parse_args()
    try:
        report = asyncio.run(run(args))
    except (ValueError, OSError, JevError) as exc:
        code = exc.code if isinstance(exc, JevError) else "evaluation_configuration"
        # OS diagnostics can contain private paths. Error codes suffice for unattended runs.
        print(json.dumps({"ok": False, "error_code": code}), file=sys.stderr)
        return 2
    summary = {key: value for key, value in report.items() if key not in {"baseline", "jev"}}
    summary["baseline_correctness"] = report["baseline"]["correctness"]
    if "jev" in report:
        summary["jev"] = {key: value for key, value in report["jev"].items() if key != "cases"}
    print(json.dumps(summary, indent=2, allow_nan=False))
    return 1 if report.get("jev", {}).get("errors", 0) else 0


if __name__ == "__main__":
    raise SystemExit(main())
