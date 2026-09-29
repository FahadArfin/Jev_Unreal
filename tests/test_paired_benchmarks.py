"""Contract and runner tests use fake tools only; no provider or Unreal access."""

import asyncio
import json
from pathlib import Path

import pytest
from pydantic import ValidationError

from jev_unreal import paired_benchmarks as paired
from jev_unreal.benchmarks import fingerprint


@pytest.fixture
def protocol():
    digest = fingerprint({"synthetic": True})
    return paired.PairedProtocol(
        id="test-pair",
        provenance="authored_pilot",
        model_id="fake-agent-v1",
        agent_configuration_sha256=digest,
        environment_sha256=digest,
        project_id="owned-test-sandbox",
        catalog_sha256=digest,
        allowed_tools=["unreal_actor_details"],
        tasks=[
            paired.PairedTask(
                id="bounds",
                goal="Measure the synthetic cube bounds.",
                initial_state_sha256=digest,
                acceptance_sha256=digest,
            )
        ],
        repetitions=2,
    )


async def run(protocol, driver=paired.SyntheticDriver):
    return await paired.run_paired(protocol, lambda trial: driver(protocol, trial))


def test_counterbalanced_schedule_is_stable_and_bounded(protocol):
    trials = paired.schedule(protocol)
    assert [t.arm for t in trials] == ["routing_off", "routing_on", "routing_on", "routing_off"]
    assert paired.schedule(protocol) == trials
    other = protocol.model_copy(update={"seed": 1})
    assert paired.schedule(other)[0].arm == "routing_on"
    for update in (
        {"tasks": protocol.tasks * 2},
        {"allowed_tools": ["a", "a"]},
        {"repetitions": True},
        {"trial_timeout_seconds": float("inf")},
    ):
        with pytest.raises(ValidationError):
            paired.PairedProtocol.model_validate(protocol.model_dump() | update)
    many = [
        task.model_copy(update={"id": f"task-{i}"}) for i, task in enumerate(protocol.tasks * 64)
    ]
    with pytest.raises(ValidationError):
        paired.PairedProtocol.model_validate(
            protocol.model_dump()
            | {
                "tasks": [task.model_dump() for task in many],
                "repetitions": 3,
            }
        )


async def test_runner_executes_both_arms_and_reports_unknown_cost(protocol):
    calls = []

    class Driver(paired.SyntheticDriver):
        async def route(self, task):
            calls.append((self.trial.id, "route"))
            return await super().route(task)

        async def call_tool(self, name, arguments):
            calls.append((self.trial.id, name))
            return await super().call_tool(name, arguments)

    result = await run(protocol, Driver)
    report = paired.report(protocol, result)
    assert all(row.status == "passed" for row in result.observations)
    assert report["matched_verified_pairs"] == 2
    assert report["arms"]["routing_on"]["routing_attempts_recorded"] == 2
    assert report["arms"]["routing_off"]["routing_attempts_recorded"] == 0
    assert report["arms"]["routing_on"]["task_calls_including_routing"] == 4
    assert report["matched_verified_deltas"][0]["task_calls_including_routing_on_minus_off"] == 1
    assert report["arms"]["routing_on"]["provider_cost_usd"] == {
        "known_count": 0,
        "unknown_count": 2,
        "known_sum": None,
        "median": None,
    }
    assert calls == [
        ("trial-0001", "unreal_actor_details"),
        ("trial-0002", "route"),
        ("trial-0002", "unreal_actor_details"),
        ("trial-0003", "route"),
        ("trial-0003", "unreal_actor_details"),
        ("trial-0004", "unreal_actor_details"),
    ]
    assert result.observations[0].tools[0].result_bytes > 0


async def test_authentication_failure_stops_future_on_requests_including_fallback(protocol):
    attempts = []

    class Driver(paired.SyntheticDriver):
        async def route(self, task):
            attempts.append(self.trial.id)
            return paired.RouteObservation(outcome="authentication_failed", provider_requests=1)

    result = await run(protocol, Driver)
    report = paired.report(protocol, result)
    assert attempts == ["trial-0002"]
    assert [row.status for row in result.observations] == [
        "passed",
        "provider_failed",
        "blocked",
        "passed",
    ]
    assert report["authentication_failed"]
    assert not report["valid_classifier_comparison"]
    assert report["arms"]["routing_on"]["success_rate"] == 0
    assert report["arms"]["routing_on"]["routing_attempts_recorded"] == 1
    assert report["arms"]["routing_on"]["started"] == 1


@pytest.mark.parametrize(
    "mismatch",
    [
        "project_id",
        "model_id",
        "catalog_sha256",
        "agent_configuration_sha256",
        "environment_sha256",
        "initial_state_sha256",
        "cache_policy",
    ],
)
async def test_preflight_mismatch_prevents_tools_and_routing(protocol, mismatch):
    class Driver(paired.SyntheticDriver):
        async def prepare(self, task):
            identity = await super().prepare(task)
            return identity.model_copy(
                update={
                    mismatch: "cold" if mismatch == "cache_policy" else "f" * 64,
                }
            )

        async def call_tool(self, name, arguments):
            pytest.fail("Identity mismatch must stop all task tools")

        async def route(self, task):
            pytest.fail("Identity mismatch must stop provider calls")

    result = await run(protocol, Driver)
    assert {row.status for row in result.observations} == {"identity_mismatch"}
    assert paired.report(protocol, result)["matched_verified_pairs"] == 0


async def test_reused_agent_context_and_wrong_verification_are_rejected(protocol):
    class Reused(paired.SyntheticDriver):
        async def prepare(self, task):
            return (await super().prepare(task)).model_copy(update={"fresh_context_id": "shared"})

    result = await run(protocol, Reused)
    assert [r.status for r in result.observations] == ["passed"] + ["identity_mismatch"] * 3

    class WrongVerification(paired.SyntheticDriver):
        async def verify(self, task, answer_sha256):
            return (await super().verify(task, answer_sha256)).model_copy(
                update={"acceptance_sha256": "f" * 64}
            )

    assert {r.status for r in (await run(protocol, WrongVerification)).observations} == {
        "identity_mismatch",
    }


@pytest.mark.parametrize("violation", ["tool_budget", "tool_not_allowed", "concurrent_tool_calls"])
async def test_tool_boundary_cannot_be_hidden_by_adapter(protocol, violation):
    protocol.max_tool_calls = 1

    class Driver(paired.SyntheticDriver):
        async def call_tool(self, name, arguments):
            await asyncio.sleep(0.001)
            return await super().call_tool(name, arguments)

        async def execute(self, task, context, recommendation):
            try:
                if violation == "tool_not_allowed":
                    await context.call_tool("unreal_execute_python", {})
                elif violation == "concurrent_tool_calls":
                    await asyncio.gather(
                        context.call_tool("unreal_actor_details", {}),
                        context.call_tool("unreal_actor_details", {}),
                        return_exceptions=True,
                    )
                else:
                    await context.call_tool("unreal_actor_details", {})
                    await context.call_tool("unreal_actor_details", {})
            except paired.TrialContractError:
                pass
            return paired.DriverResult(answer_sha256=fingerprint({"pretend": "passed"}))

    result = await run(protocol, Driver)
    assert {r.failure_code for r in result.observations} == {violation}
    assert all(len(r.tools) <= 1 for r in result.observations)
    assert paired.report(protocol, result)["matched_verified_pairs"] == 0


async def test_failed_tool_attempt_is_counted_and_exception_is_sanitized(protocol):
    class Driver(paired.SyntheticDriver):
        async def call_tool(self, name, arguments):
            raise RuntimeError("secret-provider-payload")

    result = await run(protocol, Driver)
    assert "secret-provider-payload" not in result.model_dump_json()
    assert all(row.tools[0].succeeded is False for row in result.observations)
    assert paired.report(protocol, result)["arms"]["routing_on"]["tool_failures_recorded"] == 2


@pytest.mark.parametrize(
    "reply",
    [
        {"ok": False, "error": "native_failure"},
        {"isError": True, "content": []},
        {"structuredContent": {"ok": False}},
        {"content": [{"type": "text", "text": '{"ok":false}'}]},
        paired.ToolReply(False, {"custom_error": "failed"}),
    ],
)
async def test_returned_tool_failures_are_measured_even_if_agent_recovers(protocol, reply):
    class Driver(paired.SyntheticDriver):
        async def call_tool(self, name, arguments):
            return reply

        async def execute(self, task, context, recommendation):
            await context.call_tool("unreal_actor_details", {})
            # A recoverable failure may still lead to independently verified success.
            return paired.DriverResult(
                answer_sha256=fingerprint({"bounds_size_cm": [100, 100, 100]})
            )

    result = await run(protocol, Driver)
    assert all(not row.tools[0].succeeded for row in result.observations)
    assert all(row.status == "passed" for row in result.observations)
    assert paired.report(protocol, result)["arms"]["routing_on"]["tool_failures_recorded"] == 2


async def test_timeouts_cancel_current_tool_and_preserve_unstarted_trials(protocol):
    protocol.total_timeout_seconds = 0.02
    cancellations = []

    class Driver(paired.SyntheticDriver):
        async def call_tool(self, name, arguments):
            try:
                await asyncio.sleep(1)
            finally:
                cancellations.append(self.trial.id)

    result = await run(protocol, Driver)
    assert cancellations == ["trial-0001"]
    assert result.observations[0].status == "timed_out"
    assert [r.status for r in result.observations[1:]] == ["budget_exhausted"] * 3
    assert len(result.observations[0].tools) == 1


async def test_cancellation_retains_partial_accounting(protocol):
    class Driver(paired.SyntheticDriver):
        async def execute(self, task, context, recommendation):
            raise asyncio.CancelledError

    result = await run(protocol, Driver)
    assert result.observations[0].status == "cancelled"
    assert [r.status for r in result.observations[1:]] == ["missing"] * 3


async def test_interrupted_routing_keeps_attempt_but_unknown_request_count_and_cost(protocol):
    protocol.seed = 1
    protocol.total_timeout_seconds = 0.02

    class Driver(paired.SyntheticDriver):
        async def route(self, task):
            await asyncio.sleep(1)

    result = await run(protocol, Driver)
    first = result.observations[0]
    assert first.status == "timed_out" and first.route_ms is not None and first.route is None
    arm = paired.report(protocol, result)["arms"]["routing_on"]
    assert arm["routing_attempts_recorded"] == 1
    assert arm["provider_requests"]["known_sum"] is None
    assert arm["provider_cost_usd"]["known_sum"] is None


async def test_missing_verification_and_provider_error_stay_in_denominators(protocol):
    class Driver(paired.SyntheticDriver):
        async def verify(self, task, answer_sha256):
            return (await super().verify(task, answer_sha256)).model_copy(update={"passed": False})

        async def route(self, task):
            return paired.RouteObservation(outcome="error")

    result = await run(protocol, Driver)
    summary = paired.report(protocol, result)
    assert summary["arms"]["routing_off"]["not_passed"] == 2
    assert summary["arms"]["routing_on"]["not_passed"] == 2
    result.observations = result.observations[:1]
    summary = paired.report(protocol, result)
    assert summary["arms"]["routing_on"]["missing"] == 2
    assert summary["arms"]["routing_off"]["success_rate"] == 0


async def test_saved_observations_cannot_claim_invalid_passes(protocol):
    original = await run(protocol)
    edits = [
        lambda r: setattr(r, "protocol_sha256", "f" * 64),
        lambda r: r.observations.append(r.observations[0]),
        lambda r: setattr(r.observations[0], "trial_id", "unknown"),
        lambda r: setattr(r.observations[0], "route", r.observations[1].route),
        lambda r: setattr(r.observations[1], "route", None),
        lambda r: setattr(r.observations[0], "verification", None),
        lambda r: setattr(r.observations[0].identity, "project_id", "other-project"),
        lambda r: setattr(r.observations[1].identity, "fresh_context_id", "trial-0001"),
    ]
    for edit in edits:
        result = original.model_copy(deep=True)
        edit(result)
        with pytest.raises(ValueError):
            paired.report(protocol, result)


async def test_successful_pair_does_not_make_unfinished_or_mismatched_run_eligible(protocol):
    original = await run(protocol)
    for status, code in [
        ("missing", "missing"),
        ("blocked", "authentication_failed"),
        ("identity_mismatch", "identity_mismatch"),
    ]:
        result = original.model_copy(deep=True)
        result.observations[-1].status = status
        result.observations[-1].failure_code = code
        summary = paired.report(protocol, result)
        assert summary["matched_verified_pairs"] == 1
        assert not summary["valid_classifier_comparison"]
        assert summary["comparison_eligibility"]["exclusion_reasons"]
        assert not (summary["collection_complete"] and summary["all_trial_identities_valid"])
    result = original.model_copy(deep=True)
    result.observations.pop()
    assert not paired.report(protocol, result)["comparison_eligibility"]["eligible"]


async def test_complete_verified_failures_are_eligible_without_successful_pairs(protocol):
    class Driver(paired.SyntheticDriver):
        async def verify(self, task, answer_sha256):
            return (await super().verify(task, answer_sha256)).model_copy(update={"passed": False})

    summary = paired.report(protocol, await run(protocol, Driver))
    assert summary["collection_complete"] and summary["all_trial_identities_valid"]
    assert summary["valid_classifier_comparison"]
    assert summary["matched_verified_pairs"] == 0
    assert summary["arms"]["routing_on"]["success_rate"] == 0
    assert summary["arms"]["routing_off"]["success_rate"] == 0


def test_cost_and_token_contract_preserve_unknowns():
    assert paired.Usage().agent_cost_usd is None
    for values in (
        {"agent_cost_usd": 0.1},
        {"agent_cost_source": "estimated"},
        {"agent_input_tokens": 5, "agent_cached_input_tokens": 6},
        {"agent_input_tokens": True},
    ):
        with pytest.raises(ValidationError):
            paired.Usage(**values)
    with pytest.raises(ValidationError):
        paired.RouteObservation(outcome="recommend")
    with pytest.raises(ValidationError):
        paired.RouteObservation(outcome="defer", provider_requests=2)


def test_cli_runs_only_offline_demo_and_reports_concrete_saved_trials(tmp_path, capsys):
    fixture = Path("examples/benchmarks/paired-routing-demo.protocol.json")
    observations, output = tmp_path / "run.json", tmp_path / "report.json"
    assert paired.main(["demo", "--protocol", str(fixture), "--output", str(observations)]) == 0
    assert (
        paired.main(
            [
                "report",
                "--protocol",
                str(fixture),
                "--observations",
                str(observations),
                "--output",
                str(output),
            ]
        )
        == 0
    )
    saved = json.loads(output.read_text())
    assert saved["matched_verified_pairs"] == 2
    assert not saved["valid_classifier_comparison"]
    assert saved["evidence_origin"] == "synthetic_demo"
    assert paired.main(["schedule", "--protocol", str(fixture), "--output", str(output)]) == 0
    assert len(json.loads(output.read_text())["trials"]) == 4
    assert (
        paired.main(
            [
                "template",
                "--protocol",
                str(fixture),
                "--output",
                str(observations),
            ]
        )
        == 0
    )
    template = paired.PairedRun.model_validate(json.loads(observations.read_text()))
    assert len(template.observations) == 4
    assert {row.status for row in template.observations} == {"missing"}
    assert paired.main(["report", "--protocol", str(fixture), "--output", str(output)]) == 2
    observations.write_text('{"private": "secret-provider-payload"}')
    assert (
        paired.main(
            [
                "report",
                "--protocol",
                str(fixture),
                "--observations",
                str(observations),
                "--output",
                str(output),
            ]
        )
        == 2
    )
    assert "secret-provider-payload" not in capsys.readouterr().out
