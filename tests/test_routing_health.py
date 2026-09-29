"""Synthetic provider tests: cloud consent, truthful health and preserved request bounds."""

import json

import httpx
import pytest

from jev_unreal.errors import JevError
from jev_unreal.health import provider_health
from jev_unreal.routing import selective_route
from jev_unreal.workflows import Candidate


def candidates():
    return [
        Candidate(id="unreal_actors", description="Inspect actors"),
        Candidate(id="unreal_assets", description="Find assets"),
    ]


def answer(request):
    body = json.loads(request.content)
    questions = body["questions"]
    answers = {}
    for key, question in questions.items():
        if question["type"] == "noul":
            answers[key] = {"type": "noul", "noul": 1}
        else:
            ids = list(question["criteria"])
            answers[key] = {
                "type": "choice", "choice": ids[0], "confidence": 0.95,
                "probabilities": {item: 1 if item == ids[0] else 0 for item in ids},
            }
    return httpx.Response(200, json={"model": body["model"], "answers": answers})


async def test_health_presence_is_not_authentication_and_default_never_calls_network(
    decision_client_factory,
):
    def no_network(request):
        pytest.fail("Health observation must remain local")

    for key in ("", "synthetic-test-key"):
        client = decision_client_factory(no_network, api_key=key)
        health = await provider_health(client)
        assert health["key_present"] is bool(key)
        assert health["authentication"] == {"status": "unknown", "observed_age_seconds": None}
        assert health["last_request"] is None
        assert health["probe"] == {"requested": False, "request_sent": False, "succeeded": None}
        assert client.requests == 0


async def test_probe_is_fresh_synthetic_redacted_and_respects_total_budget(decision_client_factory):
    seen = []

    def handler(request):
        seen.append(json.loads(request.content))
        return answer(request)

    client = decision_client_factory(handler, max_requests=2)
    first = await provider_health(client, probe=True)
    second = await provider_health(client, probe=True)
    refused = await provider_health(client, probe=True)
    assert len(seen) == 2
    assert seen[0] == seen[1]
    assert seen[0]["state"] == {"purpose": "Jev Unreal provider health check", "sample": "blue"}
    assert first["probe"]["succeeded"] and second["probe"]["request_sent"]
    assert second["authentication"]["status"] == "authenticated"
    assert second["last_request"]["outcome"] == "success"
    assert refused["probe"]["error"]["code"] == "request_limit"
    assert refused["probe"]["request_sent"] is False
    assert client.cache_hits == 0
    assert "synthetic-test-key" not in json.dumps(second)


async def test_probe_without_key_does_not_attempt_transport(decision_client_factory):
    client = decision_client_factory(api_key="")
    health = await provider_health(client, probe=True)
    assert health["probe"]["error"]["code"] == "missing_api_key"
    assert health["probe"]["request_sent"] is False
    assert client.requests == 0


async def test_cached_decision_does_not_erase_a_later_auth_rejection(
    decision_client_factory, decision_questions,
):
    calls = 0

    def handler(request):
        nonlocal calls
        calls += 1
        return answer(request) if calls in (1, 3) else httpx.Response(401, text="secret")

    client = decision_client_factory(handler)
    await client.decide("scene", decision_questions)
    rejected = await provider_health(client, probe=True)
    assert rejected["authentication"]["status"] == "rejected"
    assert rejected["last_request"]["http_status"] == 401
    assert rejected["probe"]["request_sent"] is True
    assert "secret" not in json.dumps(rejected)
    assert (await client.decide("scene", decision_questions))["cached"]
    assert (await provider_health(client))["authentication"]["status"] == "rejected"
    recovered = await provider_health(client, probe=True)
    assert recovered["authentication"]["status"] == "authenticated"
    assert recovered["last_request"]["error_code"] is None


@pytest.mark.parametrize("failure", [403, 429, 503, "network", "malformed"])
async def test_availability_failures_do_not_claim_key_rejection(failure, decision_client_factory):
    def handler(request):
        if failure == "network":
            raise httpx.ConnectError("synthetic-test-key", request=request)
        if failure == "malformed":
            return httpx.Response(200, text="malformed-secret-response")
        return httpx.Response(failure)

    client = decision_client_factory(handler)
    health = await provider_health(client, probe=True)
    assert health["probe"]["succeeded"] is False
    assert health["probe"]["request_sent"] is True
    assert health["authentication"]["status"] == "unknown"
    assert health["last_request"]["outcome"] == "failed"
    assert "secret" not in json.dumps(health)


async def test_probe_obeys_existing_circuit_without_retry(decision_client_factory):
    client = decision_client_factory(lambda request: httpx.Response(503))
    for _ in range(3):
        await provider_health(client, probe=True)
    stopped = await provider_health(client, probe=True)
    assert stopped["probe"]["request_sent"] is False
    assert stopped["probe"]["error"]["code"] == "circuit_open"
    assert client.requests == 3


async def test_explicit_single_and_ambiguous_local_routes_never_use_provider(
    decision_client_factory,
):
    def no_network(request):
        pytest.fail("Local routing must not consult the provider")

    client = decision_client_factory(no_network)
    explicit = await selective_route(
        client, "Find actors", candidates(), explicit_tool="unreal_assets", allow_cloud=True,
    )
    assert explicit["selected"] == "unreal_assets"  # Caller ID wins over lexical clues.
    assert explicit["reason"] == "explicit_candidate"
    single = await selective_route(client, "Task", candidates()[:1], allow_cloud=True)
    assert single["selected"] == "unreal_actors" and single["reason"] == "single_candidate"
    ambiguous = await selective_route(client, "Call unreal_actors", candidates())
    assert ambiguous["outcome"] == "defer" and ambiguous["selected"] is None
    assert ambiguous["reason"] == "ambiguous_cloud_disabled"
    for result in (explicit, single, ambiguous):
        assert result["executed"] is False
        assert result["routing"]["cloud_requested"] is False
        assert result["routing"]["provider_request_sent"] is False
        assert "confidence" not in result
    assert client.requests == 0


async def test_ambiguous_opt_in_routes_once_and_accounts_for_cache(decision_client_factory):
    client = decision_client_factory(answer)
    fresh = await selective_route(client, "Task", candidates(), allow_cloud=True)
    cached = await selective_route(client, "Task", candidates(), allow_cloud=True)
    assert fresh["selected"] == "unreal_actors" and fresh["executed"] is False
    assert fresh["routing"]["provider_request_sent"] is True
    assert fresh["routing"]["cache_hit"] is False
    assert cached["routing"]["cloud_requested"] is True
    assert cached["routing"]["cache_hit"] is True
    assert cached["routing"]["provider_request_sent"] is False
    assert client.requests == 1


async def test_cloud_failure_defers_without_silent_local_guess_or_retry(decision_client_factory):
    client = decision_client_factory(lambda request: httpx.Response(401), max_requests=1)
    failed = await selective_route(client, "unreal_actors", candidates(), allow_cloud=True)
    limited = await selective_route(client, "unreal_actors", candidates(), allow_cloud=True)
    assert failed["outcome"] == "defer" and failed["selected"] is None
    assert failed["reason"] == "provider_failed"
    assert failed["error"]["code"] == "provider_error"
    assert failed["routing"]["provider_request_sent"] is True
    assert limited["routing"]["provider_request_sent"] is False
    assert limited["error"]["code"] == "request_limit"
    assert client.requests == 1


async def test_missing_uncertainty_still_defers_in_selective_mode(decision_client_factory):
    client = decision_client_factory(
        lambda request: httpx.Response(200, json={
            "model": "typesafe/jev-1.13",
            "answers": {"route": {"type": "choice", "choice": "unreal_actors"}},
        })
    )
    result = await selective_route(client, "Task", candidates(), allow_cloud=True)
    assert result["outcome"] == "defer" and result["reason"] == "missing_uncertainty"
    assert result["executed"] is False


@pytest.mark.parametrize("arguments", [
    {"explicit_tool": "unknown"},
    {"explicit_tool": "UNREAL_ACTORS"},
    {"candidates": []},
    {"candidates": [Candidate(id="__defer__", description="Reserved")]},
    {"candidates": [candidates()[0], candidates()[0]]},
    {"goal": " "},
    {"goal": "x" * 12001},
    {"allow_cloud": "yes"},
])
async def test_invalid_routing_fails_before_network(arguments, decision_client_factory):
    client = decision_client_factory()
    with pytest.raises(JevError):
        await selective_route(client, **{"goal": "Task", "candidates": candidates(), **arguments})
    assert client.requests == 0
