"""Evaluation-harness tests use synthetic transport; they are not benchmark results."""

import importlib.util
import json
import sys
from pathlib import Path

import httpx
import pytest

from jev_unreal.config import Settings
from jev_unreal.decision import DecisionClient
from jev_unreal.errors import JevError
from jev_unreal.workflows import CATALOG

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("jev_evaluation_script", ROOT / "scripts/evaluate.py")
evaluation = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = evaluation
SPEC.loader.exec_module(evaluation)


def dataset(cases=None):
    return {
        "schema_version": 1,
        "description": "Synthetic unit test fixture, not benchmark evidence.",
        "cases": cases
        or [
            {
                "id": "assets",
                "group": "assets",
                "goal": "Search existing assets",
                "expected": "unreal_assets",
            }
        ],
    }


def write_dataset(tmp_path, value):
    path = tmp_path / "cases.json"
    path.write_text(json.dumps(value), encoding="utf-8")
    return path


def test_public_fixture_is_bounded_and_has_all_requested_groups():
    data, digest = evaluation.load_cases(ROOT / "examples/evaluation_cases.json")
    assert 16 <= len(data.cases) <= 24
    assert len(digest) == 64
    assert {case.group for case in data.cases} == {
        "status",
        "actors",
        "assets",
        "preview",
        "triage",
        "unsupported",
        "ambiguous",
        "injection",
    }
    assert all(case.expected in evaluation.LABELS for case in data.cases)


@pytest.mark.parametrize("invalid", ["duplicate", "label", "goal", "id", "extra", "too_many"])
def test_invalid_datasets_are_rejected_without_echoing_contents(tmp_path, invalid):
    data = dataset()
    if invalid == "duplicate":
        data["cases"].append(data["cases"][0].copy())
    elif invalid == "label":
        data["cases"][0]["expected"] = "execute_python"
    elif invalid == "goal":
        data["cases"][0]["goal"] = " "
    elif invalid == "id":
        data["cases"][0]["id"] = "invalid id"
    elif invalid == "extra":
        data["cases"][0]["private"] = "sensitive fixture contents"
    else:
        data["cases"] = [{**data["cases"][0], "id": str(i)} for i in range(25)]
    with pytest.raises(ValueError) as caught:
        evaluation.load_cases(write_dataset(tmp_path, data))
    assert "sensitive fixture contents" not in str(caught.value)


async def test_validate_only_uses_actual_routing_schema_without_credentials_or_network(
    tmp_path, monkeypatch
):
    def forbidden(*args, **kwargs):
        pytest.fail("Validation must not instantiate a provider client")

    monkeypatch.setattr(evaluation, "DecisionClient", forbidden)
    monkeypatch.setenv("JEV_PROVIDER", "invalid-provider")
    monkeypatch.setenv("JEV_BRIDGE_TOKEN_FILE", "not-a-real-token-file")
    output = tmp_path / "validation.json"
    args = evaluation.parser().parse_args(["--validate-only", "--output", str(output)])
    result = await evaluation.run(args)
    assert result["mode"] == "validation_only"
    assert result["case_count"] == 24
    assert result["budget"]["planned_provider_requests"] == 0
    assert result["budget"]["total_request_bytes"] > 0
    assert "jev" not in result
    assert json.loads(output.read_text(encoding="utf-8"))["mode"] == "validation_only"


@pytest.mark.parametrize(
    "arguments", [["--max-requests", "23"], ["--max-input-bytes", "1"], ["--max-requests", "25"]]
)
async def test_budget_rejected_before_live_client_creation(arguments, monkeypatch):
    monkeypatch.setattr(evaluation, "DecisionClient", lambda *a, **kw: pytest.fail("No network"))
    with pytest.raises(ValueError):
        await evaluation.run(evaluation.parser().parse_args(["--validate-only", *arguments]))


def test_percentiles_and_empty_metrics_are_defined():
    assert evaluation.percentile([], 0.5) is None
    assert evaluation.percentile([4], 0.95) == 4
    assert evaluation.percentile([0, 10, 20, 30, 40], 0.5) == 20
    assert evaluation.percentile([0, 10, 20, 30, 40], 0.95) == 38
    assert evaluation.fraction(0, 0)["ratio"] is None


def test_heuristic_baseline_is_transparent_and_deterministic():
    data = evaluation.EvaluationSet.model_validate(dataset())
    result = evaluation.baseline_report(data)
    assert result["provider_requests"] == 0
    assert result["correctness"] == {"correct": 1, "total": 1, "ratio": 1.0}
    assert result["rules_in_order"]
    assert evaluation.heuristic_route("Please write C++ code") == "__defer__"
    assert evaluation.heuristic_route("Preview moving an actor") == "unreal_preview"
    assert evaluation.heuristic_route("No clear request") == "__defer__"


async def test_live_evaluation_refuses_missing_key(monkeypatch):
    monkeypatch.setattr(evaluation, "DecisionClient", lambda *a, **kw: pytest.fail("No network"))
    with pytest.raises(JevError) as caught:
        await evaluation.evaluate(evaluation.EvaluationSet.model_validate(dataset()), Settings())
    assert caught.value.code == "missing_api_key"


async def test_metrics_separate_acceptance_deferral_error_cost_and_raw_accuracy(monkeypatch):
    data = evaluation.EvaluationSet.model_validate(
        dataset(
            [
                {
                    "id": "assets",
                    "group": "assets",
                    "goal": "Find assets",
                    "expected": "unreal_assets",
                },
                {"id": "ambiguous", "group": "ambiguous", "goal": "Maybe", "expected": "__defer__"},
                {"id": "status", "group": "status", "goal": "Status", "expected": "unreal_status"},
                {
                    "id": "error",
                    "group": "actors",
                    "goal": "Inspect actors",
                    "expected": "unreal_actors",
                },
            ]
        )
    )
    choices = iter(
        [("unreal_assets", 0.9, 0.001), ("unreal_actors", 0.9, None), ("unreal_status", 0.1, 0.002)]
    )
    calls = []

    def handler(request):
        calls.append(request)
        if len(calls) == 4:
            return httpx.Response(500, text="private provider diagnostic")
        choice, confidence, cost = next(choices)
        probabilities = {label: 0 for label in evaluation.LABELS}
        probabilities[choice] = 0.9
        probabilities["__defer__"] = 0.1
        usage = {"input_tokens": 100, "output_tokens": 10}
        if cost is not None:
            usage["cost"] = cost
        return httpx.Response(
            200,
            json={
                "model": "typesafe/jev-1.13",
                "answers": {
                    "route": {
                        "type": "choice",
                        "choice": choice,
                        "confidence": confidence,
                        "probabilities": probabilities,
                    }
                },
                "usage": usage,
            },
        )

    def make_client(settings):
        assert settings.cache_seconds == 0
        return DecisionClient(settings, transport=httpx.MockTransport(handler))

    monkeypatch.setattr(evaluation, "DecisionClient", make_client)
    result = await evaluation.evaluate(data, Settings(api_key="synthetic-test-key", max_requests=4))
    assert len(calls) == result["provider_requests"] == 4
    assert result["accepted"] == 2
    assert result["deferred"] == 1
    assert result["errors"] == 1
    assert result["correctness_among_accepted"]["ratio"] == 0.5
    assert result["raw_choice_correctness"]["correct"] == 2
    assert result["raw_choice_correctness"]["total"] == 3
    assert result["recommendations_on_expected_defer"] == 1
    assert result["usage"]["input_tokens"]["reported_total"] == 300
    assert result["usage"]["cost"]["reported_total"] == pytest.approx(0.003)
    assert result["usage"]["cost"]["responses_reporting"] == 2
    assert not result["usage"]["cost"]["complete_for_all_cases"]
    assert result["cases"][-1]["error_code"] == "provider_error"
    assert "synthetic-test-key" not in json.dumps(result)
    assert "private provider diagnostic" not in json.dumps(result)
    assert all("goal" not in row for row in result["cases"])
    assert set(CATALOG).issubset(json.loads(calls[0].content)["questions"]["route"]["criteria"])
