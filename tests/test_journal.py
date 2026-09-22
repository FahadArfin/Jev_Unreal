import asyncio
from copy import deepcopy
from unittest.mock import AsyncMock, Mock

import pytest

from jev_unreal.errors import JevError
from jev_unreal.journal import PlanJournal
from jev_unreal.layouts import PreviewTracker, verify_readback


def metadata_operation():
    return {
        "op": "set_metadata",
        "actor_path": "/Temp/Map.Map:PersistentLevel.Cube",
        "label": "Cover",
        "folder": "Blockout/Cover",
        "location": [0, 0, 50],
        "rotation": [0, 0, 0],
        "scale": [1, 1, 1],
    }


def actor_for(operation):
    actor = deepcopy(operation)
    actor["path"] = actor.pop("actor_path")
    return actor


@pytest.mark.parametrize("field", ["label", "folder", "location", "rotation", "scale"])
def test_metadata_readback_checks_edits_and_preserved_transforms(field):
    operation = metadata_operation()
    actor = actor_for(operation)
    assert verify_readback([operation], [actor])["status"] == "passed"
    actor[field] = "Unexpected" if field in {"label", "folder"} else [2, 3, 4]
    assert verify_readback([operation], [actor])["status"] == "mismatch"


@pytest.mark.parametrize(
    "materials",
    [
        None,
        [],
        [{"slot": True, "path": "/Game/M.M"}],
        [{"slot": 0, "path": "/Game/Wrong.Wrong"}],
        [{"slot": 0, "path": "/Game/M.M"}] * 2,
    ],
)
def test_material_readback_requires_one_exact_slot(materials):
    operation = {
        **metadata_operation(),
        "op": "set_material",
        "slot": 0,
        "material_path": "/Game/M.M",
    }
    actor = actor_for(operation)
    actor["materials"] = materials
    assert verify_readback([operation], [actor])["status"] == "mismatch"
    actor["materials"] = [{"slot": 0, "path": "/Game/M.M"}]
    assert verify_readback([operation], [actor])["status"] == "passed"


async def test_expected_state_forwarded_and_success_record_survives_replay():
    bridge = AsyncMock()
    op = metadata_operation()
    bridge.call.return_value = {"plan_id": "p", "operations": [op], "revision": "before"}
    tracker = PreviewTracker(bridge)
    state = {"session_id": "s", "world_path": "w", "revision": "r"}
    await tracker.preview([op], expected_state=state)
    assert bridge.call.await_args.args == ("preview", {"operations": [op], "expected_state": state})
    preview_record = tracker.journal.get("p")
    preview_record["operations"][0]["label"] = "Tampered"
    assert tracker.journal.get("p")["operations"][0]["label"] == "Cover"
    bridge.call.return_value = {"applied": True, "actors": [actor_for(op)], "revision": "after"}
    await tracker.apply("p")
    assert tracker.journal.get("p")["status"] == "applied"
    assert tracker.journal.get("p")["verification"]["status"] == "passed"
    with pytest.raises(JevError, match="already attempted"):
        await tracker.apply("p")
    assert bridge.call.await_count == 2
    assert tracker.journal.get("p")["status"] == "applied"


@pytest.mark.parametrize(
    "code,status,executed",
    [
        ("editor_unavailable", "unknown", None),
        ("rollback_failed", "unknown", None),
        ("apply_failed", "unknown", None),
        ("unknown_plan", "unknown", None),
        ("stale_plan", "rejected", False),
        ("material_slot_invalid", "rejected", False),
        ("wrong_project", "rejected", False),
    ],
)
async def test_uncertain_outcomes_do_not_become_failures_or_retries(code, status, executed):
    bridge = AsyncMock()
    bridge.call.side_effect = JevError(code, "Private detail not for retention")
    tracker = PreviewTracker(bridge)
    with pytest.raises(JevError):
        await tracker.apply("p")
    record = tracker.journal.get("p")
    assert record["status"] == status and record["executed"] is executed
    assert "Private detail" not in str(record)
    with pytest.raises(JevError):
        await tracker.apply("p")
    assert bridge.call.await_count == 1


async def test_cancelled_apply_keeps_ambiguity_record():
    bridge = AsyncMock()
    bridge.call.side_effect = asyncio.CancelledError()
    tracker = PreviewTracker(bridge)
    with pytest.raises(asyncio.CancelledError):
        await tracker.apply("p")
    assert tracker.journal.get("p")["status"] == "unknown"
    assert tracker.journal.get("p")["error_code"] == "interrupted"


async def test_invalid_optional_receipt_metadata_does_not_hide_applied_result():
    bridge = AsyncMock()
    bridge.call.return_value = {"applied": True, "actors": [], "revision": float("nan")}
    tracker = PreviewTracker(bridge)
    result = await tracker.apply("p")
    assert result["applied"] is True
    assert tracker.journal.get("p")["status"] == "applied"
    assert tracker.journal.get("p")["details_omitted"] is True


def test_history_expiration_record_caps_byte_budget_and_deepcopy():
    now = [0.0]
    journal = PlanJournal(clock=lambda: now[0])
    for i in range(70):
        journal.put(str(i), status="previewed", operations=[])
    assert len(journal._records) == 64 and not journal.contains("0")
    journal.max_bytes = 1600
    for i in range(70, 80):
        journal.put(str(i), status="previewed", operations=[{"label": "x" * 500}])
    assert sum(record[1] for record in journal._records.values()) <= 1600
    now[0] = 121
    assert journal.get("79")["preview_expired"] is True
    journal.put("79", status="applied")  # Updating never extends retention indefinitely.
    now[0] = 901
    with pytest.raises(JevError, match="No retained"):
        journal.get("79")


def test_oversized_operation_details_are_omitted_without_unbounded_history():
    journal = PlanJournal()
    journal.put("p", status="previewed", operations=[{"description": "x" * 70000}])
    assert journal.get("p")["details_omitted"] is True
    assert "operations" not in journal.get("p")


@pytest.mark.parametrize("op", [[], {}, ["set_metadata"], True, None])
async def test_malformed_normalized_operation_cannot_hide_native_applied_outcome(op):
    operation = metadata_operation()
    bridge = AsyncMock()
    bridge.call.return_value = {"plan_id": "p", "operations": [{**operation, "op": op}]}
    tracker = PreviewTracker(bridge)
    await tracker.preview([operation])
    bridge.call.return_value = {"applied": True, "actors": [actor_for(operation)]}
    result = await tracker.apply("p")
    assert result["applied"] is True
    assert result["verification"]["status"] == "mismatch"
    assert result["verification"]["issues"] == [{"index": 0, "code": "invalid_expected_operation"}]
    assert tracker.journal.get("p")["status"] == "applied"


async def test_receipt_and_fallback_failures_do_not_hide_apply_or_allow_immediate_replay():
    bridge = AsyncMock()
    bridge.call.return_value = {"applied": True, "actors": []}
    tracker = PreviewTracker(bridge)
    tracker.journal.put = Mock(side_effect=RuntimeError("synthetic private receipt failure"))
    result = await tracker.apply("p")
    assert result["applied"] is True
    assert result["verification"]["status"] == "unavailable"
    assert "synthetic private" not in str(result)
    assert tracker.journal.put.call_count == 4  # Normal and fallback before/after the native call.
    with pytest.raises(JevError, match="already attempted"):
        await tracker.apply("p")
    assert bridge.call.await_count == 1


async def test_receipt_failures_do_not_replace_native_error_or_cancellation():
    bridge = AsyncMock()
    tracker = PreviewTracker(bridge)
    tracker.journal.put = Mock(side_effect=RuntimeError("receipt failed"))
    native_error = JevError("editor_unavailable", "Connection lost after dispatch.")
    bridge.call.side_effect = native_error
    with pytest.raises(JevError) as caught:
        await tracker.apply("network-loss")
    assert caught.value is native_error
    bridge.call.side_effect = asyncio.CancelledError()
    with pytest.raises(asyncio.CancelledError):
        await tracker.apply("cancelled")
    with pytest.raises(JevError, match="already attempted"):
        await tracker.apply("cancelled")
    assert bridge.call.await_count == 2


async def test_deep_optional_receipt_metadata_does_not_hide_native_apply():
    metadata = {}
    for _ in range(600):
        metadata = {"nested": metadata}
    bridge = AsyncMock()
    bridge.call.return_value = {"applied": True, "actors": [], "revision": metadata}
    tracker = PreviewTracker(bridge)
    result = await tracker.apply("p")
    assert result["applied"] is True
    record = tracker.journal.get("p")
    assert record["status"] == "applied"
    assert record["executed"] is True
    assert record["details_omitted"] is True
    assert "revision_after" not in record


@pytest.mark.parametrize("expiry", ["120", True, -1, 121, None, [], float("inf"), float("nan")])
def test_invalid_optional_expiry_is_omitted_and_record_remains_readable(expiry):
    now = [0.0]
    journal = PlanJournal(clock=lambda: now[0])
    journal.put("p", status="previewed", expires_in_seconds=expiry)
    record = journal.get("p")
    assert record["details_omitted"] is True
    assert "expires_in_seconds" not in record
    assert record["preview_expired"] is False
    now[0] = 120
    assert journal.get("p")["preview_expired"] is True


def test_rejected_oversized_record_update_retains_previous_record_and_byte_count():
    journal = PlanJournal()
    journal.put("p", status="previewed", revision="measured-before", operations=[])
    previous = deepcopy(journal._records)
    with pytest.raises(JevError) as caught:
        journal.put("p", revision_after="x" * (journal.max_record_bytes + 1))
    assert caught.value.code == "plan_record_too_large"
    assert journal._records == previous
    assert journal.get("p")["revision"] == "measured-before"


@pytest.mark.parametrize("lifetime", [0, 30, 120])
async def test_expired_expectations_cannot_claim_verification_for_native_success(
    monkeypatch, lifetime
):
    now = [0.0]
    monkeypatch.setattr("jev_unreal.layouts.time.monotonic", lambda: now[0])
    operation = metadata_operation()
    bridge = AsyncMock()
    bridge.call.return_value = {
        "plan_id": "p",
        "operations": [operation],
        "expires_in_seconds": lifetime,
    }
    tracker = PreviewTracker(bridge)
    await tracker.preview([operation])
    now[0] = lifetime
    bridge.call.return_value = {"applied": True, "actors": [actor_for(operation)]}
    result = await tracker.apply("p")
    assert result["applied"] is True
    assert result["verification"]["status"] == "unavailable"
    assert tracker.journal.get("p")["verification"]["status"] == "unavailable"
    assert "p" not in tracker._plans


async def test_attempt_guard_is_bounded_and_independent_of_receipt_eviction(monkeypatch):
    now = [0.0]
    monkeypatch.setattr("jev_unreal.layouts.time.monotonic", lambda: now[0])
    bridge = AsyncMock()
    bridge.call.return_value = {"applied": True, "actors": []}
    tracker = PreviewTracker(bridge)
    tracker.journal.max_bytes = 0
    for index in range(70):
        await tracker.apply(str(index))
    assert len(tracker._attempted) == 64
    assert "0" not in tracker._attempted
    assert not tracker.journal.contains("69")
    with pytest.raises(JevError, match="already attempted"):
        await tracker.apply("69")
    assert bridge.call.await_count == 70
    now[0] = 900
    bridge.call.side_effect = JevError("unknown_plan", "Native plan is no longer available.")
    with pytest.raises(JevError):
        await tracker.apply("69")
    assert len(tracker._attempted) == 1
    assert bridge.call.await_count == 71


async def test_simultaneous_duplicate_apply_dispatches_only_once():
    entered, release = asyncio.Event(), asyncio.Event()

    async def call(action, params):
        entered.set()
        await release.wait()
        return {"applied": True, "actors": []}

    bridge = AsyncMock()
    bridge.call.side_effect = call
    tracker = PreviewTracker(bridge)
    first = asyncio.create_task(tracker.apply("p"))
    try:
        await asyncio.wait_for(entered.wait(), timeout=1)
        with pytest.raises(JevError, match="already attempted"):
            await tracker.apply("p")
    finally:
        release.set()
    assert (await first)["applied"] is True
    assert bridge.call.await_count == 1
