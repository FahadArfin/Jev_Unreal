"""Adversarial tests for the typed provider boundary, independent of transport."""

import copy
import math

import pytest

from jev_unreal.errors import JevError
from jev_unreal.schema import request_body, validate_answers


def assert_error(code, function, *args):
    with pytest.raises(JevError) as caught:
        function(*args)
    assert caught.value.code == code


def test_mixed_primitives_preserve_question_ids_and_canonical_score_legend():
    questions = {
        "asset": {"type": "choice", "instructions": "Choose", "criteria": {"a": None, "b": "B"}},
        "relevant": {"type": "noul", "instructions": "Relevant?"},
        "severity": {"type": "score", "instructions": "Severity?", "criteria": ["Low", "High"]},
    }
    request = request_body({"selection": ["Wall", "床"], "visible": True}, questions, "jev-1.13.0")
    result = validate_answers(
        {
            "model": "jev-1.13.0",
            "answers": {
                "asset": {"type": "choice", "choice": "a"},
                "relevant": {"type": "noul", "noul": 0.8},
                "severity": {
                    "type": "score",
                    "score": 0.25,
                    "probabilities": {"0": 0.75, "1": 0.25},
                },
            },
        },
        request["questions"],
    )
    assert set(result["answers"]) == set(questions)
    assert result["answers"]["severity"]["legend"] == {"0": "Low", "1": "High"}
    assert "confidence" not in result["answers"]["asset"]
    assert "confidence" not in result["answers"]["relevant"]
    assert "confidence" not in result["answers"]["severity"]
    assert request["questions"]["asset"]["criteria"]["a"] is None


@pytest.mark.parametrize("state", [None, False, True, 0, 1.5, b"binary"])
def test_rejects_non_text_non_json_container_state(state, decision_questions):
    assert_error("invalid_request", request_body, state, decision_questions, "jev-1.13.0")


@pytest.mark.parametrize("state", [{"x": math.nan}, [math.inf], {"x": {1, 2}}])
def test_rejects_non_json_or_nonfinite_state(state, decision_questions):
    assert_error("invalid_request", request_body, state, decision_questions, "jev-1.13.0")


def test_rejects_circular_state(decision_questions):
    state = {}
    state["self"] = state
    assert_error("invalid_request", request_body, state, decision_questions, "jev-1.13.0")


@pytest.mark.parametrize("questions", [None, 5, [], ["route"], {}, {"": {}}, {3: {}}])
def test_invalid_question_containers_and_ids_raise_public_errors(questions):
    assert_error("invalid_request", request_body, "state", questions, "jev-1.13.0")


@pytest.mark.parametrize(
    "question",
    [
        {"type": "choice", "instructions": "Choose", "criteria": {"a": "Only option"}},
        {"type": "choice", "instructions": "Choose", "criteria": {"": "Bad", "b": "Good"}},
        {"type": "choice", "instructions": "Choose", "criteria": {"a": 4, "b": "B"}},
        {"type": "noul", "instructions": "", "criteria": {"true": "T"}},
        {"type": "noul", "instructions": "Check", "criteria": {"maybe": "M"}},
        {"type": "noul", "instructions": "Check", "unexpected": "injected"},
        {"type": "score", "instructions": "Rate", "criteria": ["Only one"]},
        {"type": "score", "instructions": "Rate", "criteria": ["a"] * 11},
        {"type": "other", "instructions": "Run arbitrary Python"},
    ],
)
def test_rejects_invalid_or_unsupported_question_schemas(question):
    assert_error("invalid_request", request_body, "state", {"q": question}, "jev-1.13.0")


def test_caps_question_count_and_choice_count():
    question = {"type": "noul", "instructions": "Valid?"}
    assert (
        len(request_body("state", {str(i): question for i in range(32)}, "jev")["questions"]) == 32
    )
    assert_error(
        "invalid_request", request_body, "state", {str(i): question for i in range(33)}, "jev"
    )
    choice = {
        "type": "choice",
        "instructions": "Choose",
        "criteria": {str(i): None for i in range(255)},
    }
    assert len(request_body("state", {"q": choice}, "jev")["questions"]["q"]["criteria"]) == 255
    choice["criteria"]["256"] = None
    assert_error("invalid_request", request_body, "state", {"q": choice}, "jev")


def test_size_limit_counts_utf8_bytes(decision_questions):
    assert request_body("a" * 20000, decision_questions, "jev")["state"]
    assert_error("request_too_large", request_body, "床" * 23000, decision_questions, "jev")


@pytest.mark.parametrize("payload", [None, [], "not an object", {}, {"model": 4, "answers": {}}])
def test_rejects_malformed_response_envelopes(payload, decision_questions):
    assert_error("invalid_response", validate_answers, payload, decision_questions)


def test_rejects_empty_response_model(decision_response, decision_questions):
    decision_response["model"] = ""
    assert_error("invalid_response", validate_answers, decision_response, decision_questions)


@pytest.mark.parametrize("change", ["missing", "extra", "wrong_type", "nested", "unknown_choice"])
def test_exact_question_binding_blocks_response_confusion(
    change, decision_response, decision_questions
):
    if change == "missing":
        decision_response["answers"] = {}
    elif change == "extra":
        decision_response["answers"]["unexpected"] = {"type": "noul", "noul": 1}
    elif change == "wrong_type":
        decision_response["answers"]["route"] = {"type": "noul", "noul": 1}
    elif change == "nested":
        decision_response["answers"]["route"] = {"nested": decision_response["answers"]["route"]}
    else:
        decision_response["answers"]["route"]["choice"] = "execute_python"
    assert_error("invalid_response", validate_answers, decision_response, decision_questions)


@pytest.mark.parametrize(
    "value", [True, False, -0.1, 1.1, math.nan, math.inf, -math.inf, "0.9", None]
)
@pytest.mark.parametrize("field", ["confidence", "probability", "noul"])
def test_invalid_uncertainty_values_are_never_coerced(
    value, field, decision_response, decision_questions
):
    if field == "noul":
        questions = {"q": {"type": "noul", "instructions": "Relevant?"}}
        payload = {"model": "jev", "answers": {"q": {"type": "noul", "noul": value}}}
    else:
        questions, payload = decision_questions, decision_response
        answer = payload["answers"]["route"]
        if field == "confidence":
            answer["confidence"] = value
        else:
            answer["probabilities"]["list_assets"] = value
    assert_error("invalid_response", validate_answers, payload, questions)


@pytest.mark.parametrize(
    "distribution",
    [
        {"list_assets": 1.0},
        {"list_assets": 0.9, "review": 0.1, "unknown": 0},
        {"list_assets": 0.8, "review": 0.8},
        {"list_assets": 0.4, "review": 0.6},
        [0.96, 0.04],
    ],
)
def test_invalid_choice_distributions_rejected(distribution, decision_response, decision_questions):
    decision_response["answers"]["route"]["probabilities"] = distribution
    assert_error("invalid_response", validate_answers, decision_response, decision_questions)


def test_accepts_tied_choice_and_small_probability_rounding(decision_response, decision_questions):
    decision_response["answers"]["route"]["probabilities"] = {"list_assets": 0.5, "review": 0.5}
    assert (
        validate_answers(decision_response, decision_questions)["answers"]["route"]["choice"]
        == "list_assets"
    )
    decision_response["answers"]["route"]["probabilities"] = {
        "list_assets": 0.96,
        "review": 0.039999,
    }
    assert validate_answers(decision_response, decision_questions)


@pytest.mark.parametrize(
    "missing", [("confidence",), ("probabilities",), ("confidence", "probabilities")]
)
def test_optional_provider_uncertainty_remains_absent(
    missing, decision_response, decision_questions
):
    for key in missing:
        del decision_response["answers"]["route"][key]
    result = validate_answers(decision_response, decision_questions)
    assert all(key not in result["answers"]["route"] for key in missing)


@pytest.mark.parametrize("score", [True, -0.01, 2.01, math.nan, math.inf, "1"])
def test_invalid_score_values_rejected(score):
    questions = {"q": {"type": "score", "criteria": ["Low", "Medium", "High"]}}
    payload = {"model": "jev", "answers": {"q": {"type": "score", "score": score}}}
    assert_error("invalid_response", validate_answers, payload, questions)


def test_score_must_agree_with_distribution_and_level_keys():
    questions = {"q": {"type": "score", "criteria": ["Low", "Medium", "High"]}}
    payload = {
        "model": "jev",
        "answers": {
            "q": {"type": "score", "score": 0.0, "probabilities": {"0": 0, "1": 0, "2": 1}}
        },
    }
    assert_error("invalid_response", validate_answers, payload, questions)
    payload["answers"]["q"].update(score=1, probabilities={"1": 1, "2": 0, "3": 0})
    assert_error("invalid_response", validate_answers, payload, questions)


@pytest.mark.parametrize("key", ["input_tokens", "output_tokens", "cost"])
@pytest.mark.parametrize("value", [True, -1, math.nan, math.inf, "100"])
def test_invalid_usage_is_rejected(key, value, decision_response, decision_questions):
    decision_response["usage"][key] = value
    assert_error("invalid_response", validate_answers, decision_response, decision_questions)


@pytest.mark.parametrize("key", ["input_tokens", "output_tokens"])
def test_token_usage_requires_whole_numbers(key, decision_response, decision_questions):
    decision_response["usage"][key] = 1.5
    assert_error("invalid_response", validate_answers, decision_response, decision_questions)


def test_unknown_provider_fields_are_not_returned(decision_response, decision_questions):
    original = copy.deepcopy(decision_response)
    decision_response["debug"] = "sensitive upstream diagnostics"
    decision_response["answers"]["route"]["debug"] = "sensitive upstream diagnostics"
    decision_response["usage"]["debug"] = "sensitive upstream diagnostics"
    result = validate_answers(decision_response, decision_questions)
    assert "sensitive" not in repr(result)
    assert result["answers"]["route"] == original["answers"]["route"]
