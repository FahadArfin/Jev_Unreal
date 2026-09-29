"""Observe provider health locally, or explicitly spend one bounded synthetic probe."""

from .decision import DecisionClient
from .errors import JevError


def request_was_sent(error: JevError) -> bool:
    """Only these DecisionClient errors occur after transport was attempted."""
    return error.code in {
        "provider_error", "provider_unavailable", "rate_limited", "invalid_response",
    }


async def provider_health(client: DecisionClient, *, probe: bool = False) -> dict:
    """No automatic probe, retries or editor data; normal session limits still apply."""
    if not isinstance(probe, bool):
        raise JevError("invalid_request", "probe must be a boolean.")
    result = {
        "provider": client.settings.provider,
        "model": client.settings.model,
        "probe": {"requested": probe, "request_sent": False, "succeeded": None},
    }
    if probe:
        try:
            decision = await client.decide(
                {"purpose": "Jev Unreal provider health check", "sample": "blue"},
                {
                    "health": {
                        "type": "noul",
                        "instructions": "Does the sample equal the word blue?",
                    }
                },
                use_cache=False,
            )
        except JevError as exc:
            result["probe"].update(
                request_sent=request_was_sent(exc), succeeded=False,
                error=exc.as_dict()["error"],
            )
        else:
            result["probe"].update(
                request_sent=True, succeeded=True,
                latency_ms=decision["latency_ms"], usage=decision["usage"],
            )
    return {
        **result,
        **client.health(),
        "requests_sent": client.requests,
        "request_limit": client.settings.max_requests,
        "interpretation": (
            "Authentication is a past observation, not a guarantee of current access. "
            "A saved key alone is not authentication. HTTP 403, rate limits and transport "
            "errors do not establish whether a key is valid. Editor health is separate."
        ),
    }
