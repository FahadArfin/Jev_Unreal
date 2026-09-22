"""Transport, cost bounds, privacy, and recovery tests with an isolated provider."""

import asyncio
import json
import time
from types import SimpleNamespace

import httpx
import pytest

from jev_unreal import decision
from jev_unreal.errors import JevError


@pytest.mark.parametrize(
    ("provider", "model", "endpoint"),
    [
        ("openrouter", "typesafe/jev-1.13", "https://openrouter.ai/api/alpha/decisions"),
        ("typesafe", "jev-1.13.0", "https://api.typesafe.ai/v1/systemone"),
    ],
)
async def test_provider_wire_contract(
    provider, model, endpoint, decision_questions, decision_response, decision_client_factory
):
    seen = []

    def handler(request):
        seen.append(request)
        return httpx.Response(200, json={**decision_response, "model": model})

    client = decision_client_factory(handler, provider=provider, model=model)
    result = await client.decide({"request": "Find a chair"}, decision_questions)
    assert len(seen) == 1
    request = seen[0]
    assert request.method == "POST"
    assert str(request.url) == endpoint
    assert request.headers["authorization"] == "Bearer synthetic-test-key"
    assert json.loads(request.content) == {
        "model": model,
        "state": {"request": "Find a chair"},
        "questions": decision_questions,
    }
    assert result["model"] == model
    assert result["provider"] == provider
    assert result["latency_ms"] >= 0
    assert not result["cached"]
    assert client.metrics()["requests_sent"] == 1


async def test_missing_key_never_attempts_network(decision_questions, decision_client_factory):
    def handler(request):
        pytest.fail("An unconfigured client must not send a provider request")

    client = decision_client_factory(handler, api_key="")
    with pytest.raises(JevError) as caught:
        await client.decide("state", decision_questions)
    assert caught.value.code == "missing_api_key"
    assert client.metrics()["requests_sent"] == 0
    assert client.metrics()["configured"] is False


async def test_invalid_input_never_spends_request_budget(
    decision_client_factory, decision_questions
):
    def handler(request):
        pytest.fail("Invalid input must not reach the provider")

    client = decision_client_factory(handler)
    with pytest.raises(JevError):
        await client.decide({"x": float("nan")}, decision_questions)
    with pytest.raises(JevError) as caught:
        await client.decide("x" * 70000, decision_questions)
    assert caught.value.code == "request_too_large"
    assert client.metrics()["requests_sent"] == 0


async def test_concurrent_identical_calls_are_coalesced_and_results_are_detached(
    decision_client_factory, decision_questions, decision_response
):
    entered = asyncio.Event()
    release = asyncio.Event()
    seen = []

    async def handler(request):
        seen.append(request)
        entered.set()
        await release.wait()
        return httpx.Response(200, json=decision_response)

    client = decision_client_factory(handler)
    jobs = [
        asyncio.create_task(client.decide({"request": "chair"}, decision_questions))
        for _ in range(8)
    ]
    await asyncio.wait_for(entered.wait(), timeout=2)
    release.set()
    results = await asyncio.wait_for(asyncio.gather(*jobs), timeout=2)
    assert len(seen) == 1
    assert sum(result["cached"] for result in results) == 7
    results[0]["answers"]["route"]["choice"] = "corrupted-by-caller"
    assert all(result["answers"]["route"]["choice"] == "list_assets" for result in results[1:])
    cached = await client.decide({"request": "chair"}, decision_questions)
    assert cached["answers"]["route"]["choice"] == "list_assets"


async def test_cache_key_ignores_json_object_order_but_not_state_or_question(
    decision_client_factory, decision_questions
):
    client = decision_client_factory()
    await client.decide({"a": 1, "b": 2}, decision_questions)
    assert (await client.decide({"b": 2, "a": 1}, decision_questions))["cached"]
    assert not (await client.decide({"a": 2, "b": 2}, decision_questions))["cached"]
    changed = {"route": {**decision_questions["route"], "instructions": "Find a different object"}}
    assert not (await client.decide({"a": 1, "b": 2}, changed))["cached"]
    assert client.metrics()["requests_sent"] == 3


async def test_cache_expiration_and_disabled_cache(
    monkeypatch, decision_client_factory, decision_questions
):
    clock = [100.0]
    monkeypatch.setattr(
        decision,
        "time",
        SimpleNamespace(monotonic=lambda: clock[0], perf_counter=time.perf_counter),
    )
    client = decision_client_factory(cache_seconds=10)
    await client.decide("same", decision_questions)
    clock[0] += 9
    assert (await client.decide("same", decision_questions))["cached"]
    clock[0] += 2
    assert not (await client.decide("same", decision_questions))["cached"]
    no_cache = decision_client_factory(cache_seconds=0)
    await no_cache.decide("same", decision_questions)
    assert not (await no_cache.decide("same", decision_questions))["cached"]


async def test_cache_has_bounded_capacity(decision_client_factory, decision_questions):
    client = decision_client_factory(max_requests=200)
    for index in range(129):
        await client.decide(str(index), decision_questions)
    assert (await client.decide("128", decision_questions))["cached"]
    assert not (await client.decide("0", decision_questions))["cached"]


async def test_budget_prevents_extra_calls_but_allows_cached_result(
    decision_client_factory, decision_questions
):
    client = decision_client_factory(max_requests=1)
    await client.decide("first", decision_questions)
    assert (await client.decide("first", decision_questions))["cached"]
    with pytest.raises(JevError) as caught:
        await client.decide("second", decision_questions)
    assert caught.value.code == "request_limit"
    assert client.metrics()["requests_sent"] == 1


@pytest.mark.parametrize("status", [301, 307, 308, 401, 403, 429, 500, 529])
async def test_status_failures_never_echo_provider_text_or_follow_redirects(
    status, decision_questions, decision_client_factory, caplog
):
    seen = []
    secret = "synthetic-test-key"

    def handler(request):
        seen.append(request)
        return httpx.Response(
            status,
            headers={"Location": f"https://untrusted.invalid/collect?token={secret}"},
            text=f"Provider diagnostic echoed Authorization: Bearer {secret}",
        )

    client = decision_client_factory(handler)
    with pytest.raises(JevError) as caught:
        await client.decide("private scene state", decision_questions)
    assert caught.value.code == ("rate_limited" if status == 429 else "provider_error")
    assert str(status) in str(caught.value)
    assert len(seen) == 1
    assert secret not in json.dumps(caught.value.as_dict())
    assert secret not in caplog.text
    assert "private scene state" not in caplog.text
    assert secret not in repr(client.settings)
    assert secret not in json.dumps(client.metrics())


@pytest.mark.parametrize(
    "failure", [httpx.ConnectError, httpx.ReadTimeout, httpx.RemoteProtocolError]
)
async def test_network_failures_are_redacted(failure, decision_questions, decision_client_factory):
    def handler(request):
        raise failure("synthetic-test-key private scene state", request=request)

    client = decision_client_factory(handler)
    with pytest.raises(JevError) as caught:
        await client.decide("private scene state", decision_questions)
    assert caught.value.code == "provider_unavailable"
    assert "synthetic-test-key" not in str(caught.value)
    assert "private scene state" not in str(caught.value)
    assert client.metrics()["requests_sent"] == 1


async def test_three_failures_open_circuit_then_recovery_is_possible(
    monkeypatch, decision_questions, decision_response, decision_client_factory
):
    clock = [100.0]
    monkeypatch.setattr(
        decision,
        "time",
        SimpleNamespace(monotonic=lambda: clock[0], perf_counter=time.perf_counter),
    )
    seen = []

    def handler(request):
        seen.append(request)
        return (
            httpx.Response(500) if len(seen) <= 3 else httpx.Response(200, json=decision_response)
        )

    client = decision_client_factory(handler)
    for _ in range(3):
        with pytest.raises(JevError) as caught:
            await client.decide("state", decision_questions)
        assert caught.value.code == "provider_error"
    with pytest.raises(JevError) as caught:
        await client.decide("state", decision_questions)
    assert caught.value.code == "circuit_open"
    assert len(seen) == 3
    clock[0] += 31
    assert (await client.decide("state", decision_questions))["answers"]["route"][
        "choice"
    ] == "list_assets"
    assert len(seen) == 4


async def test_success_resets_consecutive_failure_count(
    decision_questions, decision_response, decision_client_factory
):
    statuses = iter([500, 500, 200, 500, 500, 200])

    def handler(request):
        status = next(statuses)
        return httpx.Response(status, json=decision_response if status == 200 else {})

    client = decision_client_factory(handler, cache_seconds=0)
    for index in range(6):
        if index in (2, 5):
            assert await client.decide("state", decision_questions)
        else:
            with pytest.raises(JevError) as caught:
                await client.decide("state", decision_questions)
            assert caught.value.code == "provider_error"


@pytest.mark.parametrize("body", [b"not JSON", b"[]", b"null", b'{"model":"jev","answers":{}}'])
async def test_malformed_provider_response_is_not_cached(
    body, decision_questions, decision_client_factory
):
    client = decision_client_factory(lambda request: httpx.Response(200, content=body))
    for _ in range(2):
        with pytest.raises(JevError) as caught:
            await client.decide("state", decision_questions)
        assert caught.value.code == "invalid_response"
    assert client.metrics()["requests_sent"] == 2
    assert client.metrics()["cache_hits"] == 0


async def test_streamed_response_limit_stops_reading_and_closes_stream(
    decision_questions, decision_client_factory
):
    class LargeStream(httpx.AsyncByteStream):
        def __init__(self):
            self.chunks_read = 0
            self.closed = False

        async def __aiter__(self):
            for _ in range(100):
                self.chunks_read += 1
                yield b"x" * 65536

        async def aclose(self):
            self.closed = True

    stream = LargeStream()
    client = decision_client_factory(lambda request: httpx.Response(200, stream=stream))
    with pytest.raises(JevError) as caught:
        await client.decide("state", decision_questions)
    assert caught.value.code == "invalid_response"
    assert "256 KiB" in str(caught.value)
    assert stream.chunks_read < 100
    assert stream.closed


async def test_missing_uncertainty_is_not_fabricated_by_transport(
    decision_questions, decision_response, decision_client_factory
):
    answer = decision_response["answers"]["route"]
    del answer["confidence"]
    del answer["probabilities"]
    client = decision_client_factory(lambda request: httpx.Response(200, json=decision_response))
    result = await client.decide("state", decision_questions)
    assert result["answers"]["route"] == {"type": "choice", "choice": "list_assets"}
