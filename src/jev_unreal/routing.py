"""Opt-in selective recommendations; no natural-language guess authorizes an action."""

from .decision import DecisionClient
from .errors import JevError
from .health import request_was_sent
from .workflows import CATALOG, Candidate, route


async def selective_route(
    client: DecisionClient,
    goal: str,
    candidates: list[Candidate] | None = None,
    *,
    explicit_tool: str | None = None,
    allow_cloud: bool = False,
) -> dict:
    """Skip cloud for an explicit candidate or a sole option; defer other local requests."""
    if (
        not isinstance(goal, str) or not goal.strip() or len(goal) > 12000
        or not isinstance(allow_cloud, bool)
    ):
        raise JevError("invalid_request", "Supply a nonempty bounded goal and boolean allow_cloud.")
    if candidates is None:
        candidates = [Candidate(id=key, description=value) for key, value in CATALOG.items()]
    if (
        not isinstance(candidates, list) or not 1 <= len(candidates) <= 64
        or any(not isinstance(candidate, Candidate) for candidate in candidates)
    ):
        raise JevError("invalid_request", "Provide 1 to 64 typed candidate tools.")
    ids = [candidate.id for candidate in candidates]
    if len(set(ids)) != len(ids) or "__defer__" in ids:
        raise JevError("invalid_request", "Candidate IDs must be unique; __defer__ is reserved.")
    if explicit_tool is not None and (
        not isinstance(explicit_tool, str) or explicit_tool not in ids
    ):
        raise JevError("invalid_request", "explicit_tool must exactly match a candidate ID.")

    result = {
        "outcome": "defer", "selected": None, "executed": False,
        "reason": "ambiguous_cloud_disabled",
        "routing": {
            "method": "deferred", "cloud_requested": False,
            "provider_request_sent": False, "cache_hit": False,
        },
    }
    if explicit_tool is not None or len(candidates) == 1:
        result.update(
            outcome="recommend",
            selected=explicit_tool if explicit_tool is not None else ids[0],
            reason="explicit_candidate" if explicit_tool is not None else "single_candidate",
        )
        result["routing"]["method"] = "local"
        return result
    if not allow_cloud:
        result["candidate_ids"] = ids
        return result

    result["routing"].update(method="jev", cloud_requested=True)
    try:
        decision = await route(client, goal, candidates)
    except JevError as exc:
        result.update(reason="provider_failed", error=exc.as_dict()["error"])
        result["routing"]["provider_request_sent"] = request_was_sent(exc)
        return result
    cached = decision["cached"]
    result["routing"].update(provider_request_sent=not cached, cache_hit=cached)
    return {**result, **decision}
