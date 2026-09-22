"""Validate both directions of the external decision contract, independently of the model."""

import json
import math
from typing import Annotated, Any, Literal

from pydantic import BaseModel, ConfigDict, Field, TypeAdapter, model_validator

from .errors import JevError


class QuestionBase(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    instructions: str = Field(min_length=1, max_length=8000)


class Choice(QuestionBase):
    type: Literal["choice"] = "choice"
    criteria: dict[str, str | None] = Field(min_length=2, max_length=255)

    @model_validator(mode="after")
    def keys(self):
        if any(not key or len(key) > 128 for key in self.criteria):
            raise ValueError("Choice keys must contain 1 to 128 characters")
        return self


class Noul(QuestionBase):
    type: Literal["noul"] = "noul"
    criteria: dict[Literal["true", "false"], str] | None = None


class Score(QuestionBase):
    type: Literal["score"] = "score"
    criteria: list[str] = Field(min_length=2, max_length=10)


Question = Annotated[Choice | Noul | Score, Field(discriminator="type")]
QUESTION_MAP = TypeAdapter(dict[str, Question])


def request_body(state: Any, questions: dict, model: str) -> dict:
    if not isinstance(state, (str, list, dict)):
        raise JevError("invalid_request", "State must be a string, object, or array.")
    if not isinstance(questions, dict) or not 1 <= len(questions) <= 32:
        raise JevError("invalid_request", "Send between 1 and 32 questions per call.")
    if any(not isinstance(k, str) or not k or len(k) > 128 for k in questions):
        raise JevError("invalid_request", "Question IDs must contain 1 to 128 characters.")
    try:
        parsed = QUESTION_MAP.validate_python(questions)
        body = {
            "model": model,
            "state": state,
            "questions": {k: q.model_dump(exclude_none=True) for k, q in parsed.items()},
        }
        encoded = json.dumps(body, allow_nan=False, ensure_ascii=False)
    except (ValueError, TypeError, RecursionError):
        raise JevError(
            "invalid_request", "Invalid question schema or non-finite JSON value."
        ) from None
    if len(encoded.encode("utf-8")) > 65536:
        raise JevError("request_too_large", "Decision payload exceeds the 64 KiB limit.")
    return body


def number(value: Any, low: float, high: float) -> float:
    if (
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(value)
        or not low <= value <= high
    ):
        raise ValueError("Invalid numeric value")
    return float(value)


def validate_answers(payload: Any, questions: dict) -> dict:
    """Optional OpenRouter uncertainty fields stay absent; callers must then abstain."""
    try:
        if (
            not isinstance(payload, dict)
            or not isinstance(payload.get("model"), str)
            or not payload["model"].strip()
        ):
            raise ValueError
        answers = payload["answers"]
        if not isinstance(answers, dict) or set(answers) != set(questions):
            raise ValueError
        clean = {}
        for key, question in questions.items():
            answer = answers[key]
            kind = question["type"]
            if not isinstance(answer, dict) or answer.get("type") != kind:
                raise ValueError
            result: dict = {"type": kind}
            if kind == "noul":
                result["noul"] = number(answer.get("noul"), 0, 1)
            else:
                criteria = question["criteria"]
                keys = set(criteria) if kind == "choice" else {str(i) for i in range(len(criteria))}
                if kind == "choice":
                    if answer.get("choice") not in keys:
                        raise ValueError
                    result["choice"] = answer["choice"]
                else:
                    result["score"] = number(answer.get("score"), 0, len(criteria) - 1)
                    result["legend"] = {str(i): level for i, level in enumerate(criteria)}
                if "confidence" in answer:
                    result["confidence"] = number(answer["confidence"], 0, 1)
                if "probabilities" in answer:
                    probs = answer["probabilities"]
                    if not isinstance(probs, dict) or set(probs) != keys:
                        raise ValueError
                    probs = {k: number(v, 0, 1) for k, v in probs.items()}
                    if abs(sum(probs.values()) - 1) > 0.015:
                        raise ValueError
                    if kind == "choice" and probs[result["choice"]] + 1e-6 < max(probs.values()):
                        raise ValueError
                    if kind == "score":
                        mean = sum(int(k) * p for k, p in probs.items())
                        if abs(mean - result["score"]) > 0.03:
                            raise ValueError
                    result["probabilities"] = probs
            clean[key] = result
        usage = payload.get("usage", {})
        if not isinstance(usage, dict):
            raise ValueError
        clean_usage = {}
        for key in ("input_tokens", "output_tokens", "cost"):
            if key in usage and usage[key] is not None:
                clean_usage[key] = number(usage[key], 0, 1e12)
                if key != "cost":
                    if not isinstance(usage[key], int):
                        raise ValueError
                    clean_usage[key] = int(clean_usage[key])
        return {"model": payload["model"], "answers": clean, "usage": clean_usage}
    except (KeyError, TypeError, ValueError, OverflowError):
        raise JevError("invalid_response", "Provider returned an invalid typed decision.") from None
