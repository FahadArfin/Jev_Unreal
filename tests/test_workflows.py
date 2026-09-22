from unittest.mock import AsyncMock

import pytest

from jev_unreal.errors import JevError
from jev_unreal.workflows import Candidate, gate, route, triage


@pytest.mark.parametrize(
    "confidence,probability,expected",
    [
        (0.9, 0.95, "recommend"),
        (0.2, 0.95, "defer"),
        (0.9, 0.60, "defer"),
    ],
)
def test_gate_uses_probability_and_confidence(confidence, probability, expected):
    assert (
        gate(
            {
                "choice": "a",
                "confidence": confidence,
                "probabilities": {"a": probability, "b": 1 - probability},
            }
        )["outcome"]
        == expected
    )


def test_missing_uncertainty_never_becomes_permission():
    assert gate({"choice": "a"}) == {
        "outcome": "defer",
        "selected": None,
        "reason": "missing_uncertainty",
    }


def test_explicit_abstention_wins_over_high_confidence():
    assert (
        gate({"choice": "__defer__", "confidence": 1, "probabilities": {"a": 0, "__defer__": 1}})[
            "outcome"
        ]
        == "defer"
    )


async def test_route_returns_candidate_without_execution():
    client = AsyncMock()
    client.decide.return_value = {
        "answers": {
            "route": {
                "choice": "inspect",
                "confidence": 0.95,
                "probabilities": {"inspect": 0.99, "__defer__": 0.01},
            }
        }
    }
    result = await route(
        client, "Inspect actor", [Candidate(id="inspect", description="Read actor")]
    )
    assert result["selected"] == "inspect"
    assert result["executed"] is False
    assert "__defer__" in client.decide.call_args.args[1]["route"]["criteria"]


@pytest.mark.parametrize(
    "candidates",
    [
        [],
        [Candidate(id="__defer__", description="Reserved")],
        [Candidate(id="a", description="A"), Candidate(id="a", description="Duplicate")],
    ],
)
async def test_bad_candidates_fail_before_network(candidates):
    client = AsyncMock()
    with pytest.raises(JevError):
        await route(client, "goal", candidates)
    client.decide.assert_not_called()


async def test_triage_batches_questions_in_one_request():
    client = AsyncMock()
    client.decide.return_value = {"answers": {"category": {"choice": "cpp_compile"}}}
    result = await triage(client, "C2065 undeclared identifier")
    assert result["outcome"] == "defer"
    client.decide.assert_awaited_once()
    assert set(client.decide.call_args.args[1]) == {"category", "blocking"}
