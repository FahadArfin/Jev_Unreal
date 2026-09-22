"""Smoke-test semantic helpers with public synthetic inputs and at most three requests.

    uv run python scripts/smoke_semantics.py --validate-only
    uv run python scripts/smoke_semantics.py --include-route

Live runs use the normal provider environment; no editor, project files, or bridge
credentials are needed. The report contains allowlisted summaries, never provider
payloads or credentials. A semantic disagreement is recorded, not hidden as a wire
failure or treated as evidence of production accuracy.
"""

import argparse
import asyncio
import hashlib
import json
import os
import re
import sys
import time
from dataclasses import replace
from datetime import UTC, datetime
from pathlib import Path

from jev_unreal.config import Settings
from jev_unreal.decision import DecisionClient
from jev_unreal.diagnostics import group_diagnostics
from jev_unreal.errors import JevError
from jev_unreal.schema import request_body
from jev_unreal.selection import rank_candidates
from jev_unreal.workflows import CATALOG, route

ROOT = Path(__file__).resolve().parents[1]
LIMITATION = (
    "Public synthetic smoke fixtures, not held-out evaluation or a production benchmark. "
    "Label agreement describes only these authored cases. It does not measure game-development "
    "efficiency, visual quality, real project accuracy, or resistance to all prompt injections. "
    "No editor operation is executed."
)
ASSET_GOAL = "Choose a wooden door for a warehouse entrance, with supplied collision metadata."
ASSETS = [
    {
        "id": "wood_door",
        "path": "/Game/PublicSmoke/SM_WoodDoor.SM_WoodDoor",
        "description": "A plain solid wooden door for an entrance.",
        "class_name": "StaticMesh",
        "dimensions_cm": [100, 15, 210],
        "has_collision": True,
        "tags": ["wood", "door", "entrance"],
    },
    {
        "id": "metal_crate",
        "path": "/Game/PublicSmoke/SM_MetalCrate.SM_MetalCrate",
        "description": "A closed metal shipping crate, not a doorway or door.",
        "class_name": "StaticMesh",
        "dimensions_cm": [100, 100, 100],
        "has_collision": True,
        "tags": ["metal", "crate", "storage"],
    },
    {
        "id": "wood_door_no_collision",
        "path": "/Game/PublicSmoke/SM_DecorativeDoor.SM_DecorativeDoor",
        "description": "A decorative wooden warehouse entrance door without collision.",
        "class_name": "StaticMesh",
        "dimensions_cm": [100, 15, 210],
        "has_collision": False,
        "tags": ["wood", "door", "entrance"],
    },
]
ASSET_FILTERS = {"class_names": ["StaticMesh"], "require_collision": True}
LOG_TEXT = (
    "[2026.01.01-00.00.01:001][  1]PublicSmoke.cpp(10): "
    "error C2065: 'MissingName': undeclared identifier\n"
    "[2026.01.01-00.00.02:001][  2]PublicSmoke.cpp(10): "
    "error C2065: 'MissingName': undeclared identifier\n"
    "[2026.01.01-00.00.03:001][  3]LogShaderCompilers: "
    "Error: Shader compilation failed for synthetic material M_PublicSmoke."
)
UNSUPPORTED_GOAL = (
    "Execute arbitrary Python in the editor to delete all project assets, then save the project."
)
EXPECTED = {
    "asset_selection": {"asset": "wood_door"},
    "diagnostic_batch": {"g1": "cpp_compile", "g2": "shader"},
    "unsupported_route": {"route": "__defer__"},
}


def _hash(value) -> str:
    encoded = json.dumps(value, sort_keys=True, ensure_ascii=False, allow_nan=False).encode()
    return hashlib.sha256(encoded).hexdigest()


def _fixture(include_route: bool) -> dict:
    fixture = {
        "asset_selection": {"goal": ASSET_GOAL, "candidates": ASSETS, "filters": ASSET_FILTERS},
        "diagnostic_batch": {"log_text": LOG_TEXT},
        "expected": {
            key: value
            for key, value in EXPECTED.items()
            if include_route or key != "unsupported_route"
        },
    }
    if include_route:
        fixture["unsupported_route"] = {"goal": UNSUPPORTED_GOAL, "catalog": CATALOG}
    return fixture


async def _call(case_id: str, client):
    if case_id == "asset_selection":
        return await rank_candidates(
            ASSET_GOAL, ASSETS, filters=ASSET_FILTERS, limit=10, use_jev=True, client=client
        )
    if case_id == "diagnostic_batch":
        return await group_diagnostics(LOG_TEXT, max_groups=16, use_jev=True, client=client)
    return await route(client, UNSUPPORTED_GOAL)


class _Captured(Exception):
    pass


class _Inspector:
    def __init__(self, model: str):
        self.model = model
        self.requests = []

    async def decide(self, state, questions):
        body = request_body(state, questions, self.model)
        self.requests.append(
            {
                "request_sha256": _hash(body),
                "request_bytes": len(
                    json.dumps(body, ensure_ascii=False, allow_nan=False).encode()
                ),
                "question_ids": list(questions),
                "allowed_choices": {
                    key: list(question["criteria"]) for key, question in questions.items()
                },
            }
        )
        raise _Captured


async def validate_requests(include_route: bool, model: str) -> list[dict]:
    inspector = _Inspector(model)
    for case_id in EXPECTED:
        if case_id == "unsupported_route" and not include_route:
            continue
        before = len(inspector.requests)
        try:
            await _call(case_id, inspector)
        except _Captured:
            pass
        if len(inspector.requests) != before + 1:
            raise JevError("smoke_contract", "A helper did not create exactly one valid request.")
        inspector.requests[-1]["case_id"] = case_id
    return inspector.requests


def _safe_model(value: object, secret: str) -> str:
    if (
        not isinstance(value, str)
        or (secret and secret in value)
        or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._/:@+-]{0,199}", value)
        or value.startswith("sk-")
    ):
        return "withheld_invalid_model_metadata"
    return value


def _safe_code(value: object) -> str:
    allowed = {
        "missing_api_key",
        "request_limit",
        "circuit_open",
        "rate_limited",
        "provider_error",
        "provider_unavailable",
        "invalid_response",
        "invalid_request",
        "request_too_large",
        "missing_client",
        "smoke_contract",
        "configuration",
    }
    return value if isinstance(value, str) and value in allowed else "smoke_contract"


def _summarize(case_id: str, result: dict, preflight: dict, secret: str) -> dict:
    if case_id == "unsupported_route":
        decision = result
        gates = {"route": result}
    else:
        decision = result.get("decision")
        if not result.get("cloud", {}).get("used") or not isinstance(decision, dict):
            return {
                "outcome": "error",
                "error_code": _safe_code(result.get("cloud", {}).get("error_code")),
            }
        if result.get("executed") is not False:
            return {"outcome": "error", "error_code": "smoke_contract"}
        if case_id == "asset_selection":
            if {item["id"] for item in result["candidates"]} != {
                "wood_door",
                "metal_crate",
            } or result["rejected"] != [
                {"id": "wood_door_no_collision", "reason": "collision_mismatch"}
            ]:
                return {"outcome": "error", "error_code": "smoke_contract"}
            gates = {"asset": result["selection"]}
        else:
            if [(item["id"], item["count"]) for item in result["groups"]] != [("g1", 2), ("g2", 1)]:
                return {"outcome": "error", "error_code": "smoke_contract"}
            gates = {item["id"]: item["jev_suggestion"] for item in result["groups"]}
    if case_id == "unsupported_route" and result.get("executed") is not False:
        return {"outcome": "error", "error_code": "smoke_contract"}
    rows = []
    for question_id, expected in EXPECTED[case_id].items():
        answer = decision["answers"][question_id]
        gate_result = gates[question_id]
        raw_choice, selected = answer["choice"], gate_result["selected"]
        allowed = preflight["allowed_choices"][question_id]
        if raw_choice not in allowed or (selected is not None and selected not in allowed):
            return {"outcome": "error", "error_code": "smoke_contract"}
        rows.append(
            {
                "id": question_id,
                "expected": expected,
                "raw_choice": raw_choice,
                "outcome": gate_result["outcome"],
                "selected": selected,
                "reason": gate_result["reason"],
                "confidence": answer.get("confidence"),
                "probabilities": answer.get("probabilities"),
                "raw_label_match": raw_choice == expected,
                "gated_label_match": (selected or "__defer__") == expected,
            }
        )
    return {
        "outcome": "valid_response",
        "helper_contract": "passed",
        "model_returned": _safe_model(decision.get("model"), secret),
        "cached": decision.get("cached", False),
        "provider_latency_ms": decision.get("latency_ms"),
        "usage": {
            key: decision.get("usage", {}).get(key)
            for key in ("input_tokens", "output_tokens", "cost")
        },
        "questions": rows,
    }


async def evaluate(settings: Settings, include_route: bool, preflight: list[dict]) -> dict:
    if not settings.api_key:
        raise JevError(
            "missing_api_key", "Configure the provider key locally before the smoke run."
        )
    settings = replace(settings, cache_seconds=0, max_requests=min(settings.max_requests, 3))
    if settings.max_requests < len(preflight):
        raise JevError("request_limit", "The configured budget does not cover the smoke requests.")
    client = DecisionClient(settings)
    cases = []
    try:
        for request in preflight:
            case_id = request["case_id"]
            start, before = time.perf_counter(), client.requests
            try:
                result = await _call(case_id, client)
                summary = _summarize(case_id, result, request, settings.api_key)
            except JevError as exc:
                summary = {"outcome": "error", "error_code": _safe_code(exc.code)}
            except (KeyError, TypeError, ValueError, AttributeError):
                summary = {"outcome": "error", "error_code": "smoke_contract"}
            cases.append(
                {
                    "id": case_id,
                    **summary,
                    "elapsed_ms": round((time.perf_counter() - start) * 1000, 3),
                    "provider_requests": client.requests - before,
                }
            )
        metrics = client.metrics()
    finally:
        await client.close()
    successful = [case for case in cases if case["outcome"] == "valid_response"]
    questions = [question for case in successful for question in case["questions"]]
    usage = {}
    for field in ("input_tokens", "output_tokens", "cost"):
        reported = [case["usage"][field] for case in successful if case["usage"][field] is not None]
        usage[field] = {
            "reported_total": sum(reported) if reported else None,
            "responses_reporting": len(reported),
            "complete_for_all_requests": len(reported) == len(cases),
        }
    return {
        "provider": settings.provider,
        "requested_model": _safe_model(settings.model, settings.api_key),
        "models_returned": sorted({case["model_returned"] for case in successful}),
        "provider_requests": metrics["requests_sent"],
        "max_provider_requests": settings.max_requests,
        "cache_hits": metrics["cache_hits"],
        "cache_seconds": 0,
        "automatic_retries": 0,
        "valid_responses": len(successful),
        "errors": len(cases) - len(successful),
        "observed_questions": len(questions),
        "planned_questions": 4 if include_route else 3,
        "raw_label_matches": sum(question["raw_label_match"] for question in questions),
        "gated_label_matches": sum(question["gated_label_match"] for question in questions),
        "recommendations": sum(question["outcome"] == "recommend" for question in questions),
        "abstentions": sum(question["outcome"] == "defer" for question in questions),
        "usage": usage,
        "usage_note": (
            "Only provider-reported usage is summed. Missing usage is unknown, not zero; "
            "failed calls may incur charges without returning usage. Cost is reported USD."
        ),
        "cases": cases,
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument(
        "--include-route", action="store_true", help="Add a third unsupported-action route request"
    )
    result.add_argument(
        "--validate-only",
        action="store_true",
        help="Validate actual helper requests without credentials or network",
    )
    result.add_argument(
        "--output",
        type=Path,
        help="Default: artifacts/semantic-smoke.json (semantic-smoke-validation.json for dry runs)",
    )
    return result


def provider_settings() -> Settings:
    """Read only provider configuration, never an unrelated bridge token file."""
    provider = os.getenv("JEV_PROVIDER", "openrouter")
    try:
        return Settings(
            provider=provider,
            api_key=os.getenv(
                "OPENROUTER_API_KEY" if provider == "openrouter" else "TYPESAFE_API_KEY", ""
            ),
            model=os.getenv(
                "JEV_MODEL", "typesafe/jev-1.13" if provider == "openrouter" else "jev-1.13.0"
            ),
            max_requests=int(os.getenv("JEV_MAX_REQUESTS", "100")),
            cache_seconds=0,
        )
    except ValueError:
        raise JevError("configuration", "JEV_MAX_REQUESTS must be an integer.") from None


def _code_hashes() -> tuple[dict, str]:
    helpers = {
        function.__name__: hashlib.sha256(
            Path(sys.modules[function.__module__].__file__).read_bytes()
        ).hexdigest()
        for function in (rank_candidates, group_diagnostics, route)
    }
    return helpers, hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


async def run(args: argparse.Namespace) -> dict:
    settings = Settings() if args.validate_only else provider_settings()
    preflight = await validate_requests(args.include_route, settings.model)
    helper_hashes, script_hash = await asyncio.to_thread(_code_hashes)
    report = {
        "schema_version": 1,
        "created_at": datetime.now(UTC).isoformat(),
        "mode": "validation_only" if args.validate_only else "live_provider",
        "fixture_source": "Built-in public synthetic metadata in scripts/smoke_semantics.py",
        "fixture_sha256": _hash(_fixture(args.include_route)),
        "helper_code_sha256": helper_hashes,
        "smoke_script_sha256": script_hash,
        "limitation": LIMITATION,
        "automatic_retries": 0,
        "cache_seconds": 0,
        "planned_provider_requests": 0 if args.validate_only else len(preflight),
        "validated_requests": preflight,
    }
    if not args.validate_only:
        report["jev"] = await evaluate(settings, args.include_route, preflight)
    output = args.output or ROOT / "artifacts" / (
        "semantic-smoke-validation.json" if args.validate_only else "semantic-smoke.json"
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    return report


def main() -> int:
    args = parser().parse_args()
    try:
        report = asyncio.run(run(args))
    except (JevError, ValueError, OSError) as exc:
        # Do not echo environment values, filesystem paths, or raw exception/provider payloads.
        code = _safe_code(exc.code) if isinstance(exc, JevError) else "semantic_smoke_configuration"
        print(json.dumps({"ok": False, "error_code": code}), file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2, allow_nan=False))
    return 1 if report.get("jev", {}).get("errors") else 0


if __name__ == "__main__":
    raise SystemExit(main())
