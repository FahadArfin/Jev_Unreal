"""Smoke harness validation uses synthetic HTTP transport, not live benchmark evidence."""

import importlib.util
import json
import sys
from pathlib import Path

import httpx
import pytest

from jev_unreal.config import Settings
from jev_unreal.decision import DecisionClient
from jev_unreal.errors import JevError

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "jev_semantic_smoke", ROOT / "scripts/smoke_semantics.py"
)
smoke = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = smoke
SPEC.loader.exec_module(smoke)


def response_for(request, choices=None, model="synthetic-model", include_cost=True):
    body = json.loads(request.content)
    answers = {}
    for name, question in body["questions"].items():
        choice = (choices or {}).get(
            name,
            {"asset": "wood_door", "g1": "cpp_compile", "g2": "shader", "route": "__defer__"}[name],
        )
        answers[name] = {
            "type": "choice",
            "choice": choice,
            "confidence": 0.95,
            "probabilities": {key: float(key == choice) for key in question["criteria"]},
        }
    usage = {"input_tokens": 100, "output_tokens": 20}
    if include_cost:
        usage["cost"] = 0.00001
    return httpx.Response(
        200,
        json={
            "model": model,
            "answers": answers,
            "usage": usage,
            "private_debug_payload": "synthetic-secret-debug-must-not-appear",
        },
    )


async def test_validation_only_needs_no_environment_credentials_editor_or_client(
    tmp_path, monkeypatch
):
    monkeypatch.setenv("JEV_PROVIDER", "invalid")
    monkeypatch.setenv("JEV_BRIDGE_TOKEN_FILE", "nonexistent-secret-file")
    monkeypatch.setattr(smoke, "DecisionClient", lambda *a, **k: pytest.fail("No provider client"))
    path = tmp_path / "smoke.json"
    args = smoke.parser().parse_args(["--validate-only", "--include-route", "--output", str(path)])
    report = await smoke.run(args)
    assert report["mode"] == "validation_only"
    assert report["planned_provider_requests"] == 0
    assert len(report["validated_requests"]) == 3
    assert [len(item["question_ids"]) for item in report["validated_requests"]] == [1, 2, 1]
    assert all(item["request_bytes"] <= 65536 for item in report["validated_requests"])
    assert "jev" not in report
    assert json.loads(path.read_text(encoding="utf-8")) == report
    assert len(report["fixture_sha256"]) == 64


def test_provider_settings_does_not_read_unrelated_bridge_credentials(monkeypatch):
    monkeypatch.setenv("JEV_PROVIDER", "openrouter")
    monkeypatch.setenv("OPENROUTER_API_KEY", "synthetic-local-key")
    monkeypatch.setenv("JEV_BRIDGE_TOKEN_FILE", "nonexistent-secret-file")
    monkeypatch.setenv("JEV_BRIDGE_URL", "invalid-unrelated-url")
    settings = smoke.provider_settings()
    assert settings.api_key == "synthetic-local-key"
    assert settings.bridge_token == ""
    assert settings.cache_seconds == 0


async def test_real_client_contract_and_wrong_semantics_remain_separate(monkeypatch):
    requests = []

    def handler(request):
        requests.append(request)
        return response_for(
            request,
            choices={"asset": "metal_crate", "g2": "__defer__", "route": "unreal_actors"},
            include_cost=len(requests) != 2,
        )

    def client(settings):
        assert settings.max_requests == 3
        assert settings.cache_seconds == 0
        return DecisionClient(settings, transport=httpx.MockTransport(handler))

    monkeypatch.setattr(smoke, "DecisionClient", client)
    preflight = await smoke.validate_requests(True, "synthetic-model")
    report = await smoke.evaluate(Settings(api_key="synthetic-local-key"), True, preflight)
    assert len(requests) == report["provider_requests"] == 3
    assert report["valid_responses"] == 3
    assert report["errors"] == 0
    assert report["observed_questions"] == 4
    assert report["raw_label_matches"] == report["gated_label_matches"] == 1
    assert report["abstentions"] == 1
    assert report["usage"]["cost"]["responses_reporting"] == 2
    assert report["usage"]["cost"]["complete_for_all_requests"] is False
    assert report["cache_hits"] == report["automatic_retries"] == 0
    serialized = json.dumps(report)
    assert "synthetic-local-key" not in serialized
    assert "synthetic-secret-debug" not in serialized
    assert all(case["provider_requests"] == 1 for case in report["cases"])
    sent_asset = json.loads(requests[0].content)
    assert {item["id"] for item in sent_asset["state"]["candidates"]} == {
        "wood_door",
        "metal_crate",
    }


async def test_provider_failure_preserves_partial_results_and_safe_error_codes(monkeypatch):
    calls = []

    def handler(request):
        calls.append(request)
        if len(calls) == 1:
            return httpx.Response(500, text="private-provider-secret")
        return response_for(request, include_cost=False)

    monkeypatch.setattr(
        smoke,
        "DecisionClient",
        lambda settings: DecisionClient(settings, transport=httpx.MockTransport(handler)),
    )
    report = await smoke.evaluate(
        Settings(api_key="synthetic-local-key"),
        False,
        await smoke.validate_requests(False, "synthetic-model"),
    )
    assert report["errors"] == 1
    assert report["valid_responses"] == 1
    assert report["provider_requests"] == 2
    assert report["cases"][0]["error_code"] == "provider_error"
    assert report["usage"]["cost"]["reported_total"] is None
    assert "private-provider-secret" not in json.dumps(report)


async def test_malformed_provider_choice_is_a_contract_failure(monkeypatch):
    def handler(request):
        return response_for(request, choices={"asset": "unknown", "g1": "unknown", "g2": "unknown"})

    monkeypatch.setattr(
        smoke,
        "DecisionClient",
        lambda settings: DecisionClient(settings, transport=httpx.MockTransport(handler)),
    )
    report = await smoke.evaluate(
        Settings(api_key="synthetic-local-key"),
        False,
        await smoke.validate_requests(False, "synthetic-model"),
    )
    assert report["errors"] == 2
    assert report["observed_questions"] == 0
    assert {case["error_code"] for case in report["cases"]} == {"invalid_response"}


@pytest.mark.parametrize(
    "settings,code",
    [
        (Settings(), "missing_api_key"),
        (Settings(api_key="synthetic-local-key", max_requests=1), "request_limit"),
    ],
)
async def test_missing_key_or_too_small_budget_fails_before_client_creation(
    settings, code, monkeypatch
):
    monkeypatch.setattr(smoke, "DecisionClient", lambda *a, **k: pytest.fail("No provider client"))
    preflight = await smoke.validate_requests(False, settings.model)
    with pytest.raises(JevError) as caught:
        await smoke.evaluate(settings, False, preflight)
    assert caught.value.code == code


@pytest.mark.parametrize(
    "value", ["synthetic-local-key", "unexpected\nprivate text", "sk-or-v1-secret", "x" * 201]
)
def test_model_metadata_is_bounded_and_secret_safe(value):
    assert smoke._safe_model(value, "synthetic-local-key") == "withheld_invalid_model_metadata"
