"""Deterministic inspection fixtures only; no Unreal process or provider is contacted."""

import json
from copy import deepcopy
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from jev_unreal.errors import JevError
from jev_unreal.verification import (
    SceneSnapshots,
    SessionIdentity,
    TransformEquals,
    verify,
    verify_fresh,
)

PROJECT = "C:/PublicSmoke/Example.uproject"
A, B = "/Temp/Test.Test:PersistentLevel.A", "/Temp/Test.Test:PersistentLevel.B"


def actor(path=A, offset=0):
    return {
        "path": path,
        "instance_id": f"instance-{path[-1]}",
        "class": "/Script/Engine.StaticMeshActor",
        "label": path[-1],
        "folder": "",
        "location": [offset + 50, 50, 50],
        "rotation": [0, 0, 0],
        "scale": [1, 1, 1],
        "static_mesh_path": "/Engine/BasicShapes/Cube.Cube",
        "collision_enabled": True,
        "materials": [
            {"slot": 0, "path": "/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"}
        ],
        "materials_truncated": False,
        "bounds_available": True,
        "bounds_cm": {
            "min": [offset, 0, 0],
            "max": [offset + 100, 100, 100],
            "center": [offset + 50, 50, 50],
            "size": [100, 100, 100],
        },
        "editable": True,
        "edit_blockers": [],
    }


def details(actors=None, **kwargs):
    return {
        "project_file": PROJECT,
        "session_id": "session",
        "world_path": "/Temp/Test.Test",
        "current_level": "/Temp/Test.Test:PersistentLevel",
        "revision": "revision-a",
        "truncated": False,
        "actors": [actor()] if actors is None else actors,
        **kwargs,
    }


def bridge(result=None):
    value = SimpleNamespace(settings=SimpleNamespace(expected_project=PROJECT), call=AsyncMock())
    value.call.return_value = details() if result is None else result
    return value


def label_check(path=A, expected="A", **kwargs):
    return {"kind": "label", "actor_path": path, "expected": expected, **kwargs}


async def test_capture_diff_uses_exact_selection_and_immutable_baseline():
    connection = bridge()
    snapshots = SceneSnapshots(connection)
    captured = await snapshots.capture([A])
    connection.call.assert_awaited_once_with("actor_details", {"actor_paths": [A]})
    captured["actors"][0]["label"] = "tampered output"
    connection.call.return_value["actors"][0]["label"] = "Changed"
    result = await snapshots.diff(captured["snapshot_id"])
    assert result["status"] == "changed"
    assert result["changed"] == [
        {"actor_path": A, "field": "label", "before": "A", "after": "Changed"}
    ]
    result["changed"][0]["before"] = "tampered diff"
    assert snapshots.compare(captured["snapshot_id"], details())["status"] == "unchanged"
    assert result["scene_modified"] is False
    assert result["cloud_used"] is False


async def test_diff_reports_material_slot_add_remove_change_and_metadata_bounds():
    connection = bridge()
    connection.call.return_value["actors"][0]["materials"].append({"slot": 1, "path": None})
    snapshots = SceneSnapshots(connection)
    captured = await snapshots.capture([A])
    current = details([actor(offset=20)], revision="revision-b")
    current["actors"][0]["folder"] = "Blockout"
    current["actors"][0]["materials"] = [
        {"slot": 0, "path": None},
        {"slot": 2, "path": "/Game/M.M"},
    ]
    result = snapshots.compare(captured["snapshot_id"], current)
    assert result["status"] == "changed"
    assert result["added"][0]["slot"] == 2
    assert result["removed"][0]["slot"] == 1
    assert result["removed"][0]["before_exists"] is True
    assert result["removed"][0]["before"] is None
    assert {item["field"] for item in result["changed"]} == {
        "folder",
        "location",
        "materials",
        "bounds_cm",
    }
    assert result["baseline_revision"] == "revision-a"
    assert result["current_revision"] == "revision-b"


async def test_global_revision_change_alone_is_not_a_selected_actor_change():
    snapshots = SceneSnapshots(bridge())
    captured = await snapshots.capture([A])
    assert (
        snapshots.compare(captured["snapshot_id"], details(revision="other-actor-edit"))["status"]
        == "unchanged"
    )


async def test_current_level_change_is_reported_without_claiming_new_world():
    snapshots = SceneSnapshots(bridge())
    captured = await snapshots.capture([A])
    result = snapshots.compare(
        captured["snapshot_id"], details(current_level="/Game/Sublevel.Sublevel")
    )
    assert result["status"] == "changed"
    assert result["context_changes"][0]["field"] == "current_level"


@pytest.mark.parametrize(
    "field,value",
    [
        ("project_file", "C:/Other/Other.uproject"),
        ("session_id", "restarted"),
        ("world_path", "/Temp/AnotherWorld.AnotherWorld"),
    ],
)
async def test_diff_rejects_changed_project_session_or_world(field, value):
    snapshots = SceneSnapshots(bridge())
    captured = await snapshots.capture([A])
    result = snapshots.compare(captured["snapshot_id"], details(**{field: value}))
    assert result["status"] == "unverifiable"
    assert result["reason"] == "identity_changed"
    assert result["removed"] == []


async def test_missing_actor_is_unverifiable_not_confirmed_deletion():
    connection = bridge()
    snapshots = SceneSnapshots(connection)
    captured = await snapshots.capture([A])
    connection.call.side_effect = JevError("actor_not_found", "private actor diagnostic")
    result = await snapshots.diff(captured["snapshot_id"])
    assert result["status"] == "unverifiable"
    assert result["reason"] == "actor_not_found"
    assert result["removed"] == []
    assert "private actor diagnostic" not in json.dumps(result)


@pytest.mark.parametrize(
    "code", ["capability_unavailable", "response_too_large", "actor_bounds_unavailable"]
)
async def test_inspection_error_codes_remain_actionable_without_raw_backend_text(code):
    connection = bridge()
    snapshots = SceneSnapshots(connection)
    captured = await snapshots.capture([A])
    connection.call.side_effect = JevError(code, "synthetic private backend detail")
    results = [
        await snapshots.diff(captured["snapshot_id"]),
        await verify_fresh(connection, [label_check()]),
    ]
    for result in results:
        assert result["status"] == "unverifiable"
        assert result["reason"] == code
        assert "synthetic private" not in json.dumps(result)
        if code == "capability_unavailable":
            assert "Rebuild and relaunch" in result["next_step"]


async def test_snapshot_expiry_and_process_identity_are_enforced_without_network():
    now = [10.0]
    connection = bridge()
    snapshots = SceneSnapshots(connection, ttl_seconds=1, clock=lambda: now[0])
    captured = await snapshots.capture([A])
    now[0] = 11
    assert (await snapshots.diff(captured["snapshot_id"]))["reason"] == "snapshot_expired"
    assert connection.call.await_count == 1
    assert (await SceneSnapshots(connection).diff(captured["snapshot_id"]))[
        "reason"
    ] == "snapshot_not_found"
    assert snapshots.status()["stored_bytes"] == 0


async def test_expiry_during_fresh_inspection_never_returns_valid_comparison():
    now = [0.0]
    connection = bridge()
    snapshots = SceneSnapshots(connection, ttl_seconds=1, clock=lambda: now[0])
    captured = await snapshots.capture([A])

    async def slow_read(*args, **kwargs):
        now[0] = 2
        return details()

    connection.call.side_effect = slow_read
    assert (await snapshots.diff(captured["snapshot_id"]))["reason"] == "snapshot_expired"


async def test_snapshot_count_and_aggregate_byte_eviction():
    first_store = SceneSnapshots(bridge(), max_snapshots=2)
    first = await first_store.capture([A])
    await first_store.capture([A])
    third = await first_store.capture([A])
    assert third["evicted_snapshots"] == 1
    assert first_store.compare(first["snapshot_id"], details())["reason"] == "snapshot_not_found"
    assert first_store.status()["count"] == 2
    size = first["stored_bytes"]
    byte_store = SceneSnapshots(bridge(), max_bytes=size + 1)
    one = await byte_store.capture([A])
    two = await byte_store.capture([A])
    assert two["evicted_snapshots"] == 1
    assert byte_store.status()["stored_bytes"] <= size + 1
    assert byte_store.compare(one["snapshot_id"], details())["status"] == "unverifiable"
    with pytest.raises(JevError, match="snapshot budget"):
        await SceneSnapshots(bridge(), max_bytes=1).capture([A])


@pytest.mark.parametrize(
    "limits",
    [
        {"max_snapshots": 33},
        {"max_snapshots": True},
        {"ttl_seconds": 901},
        {"ttl_seconds": float("nan")},
        {"max_bytes": 2 * 1024 * 1024 + 1},
    ],
)
def test_snapshot_hard_limits_cannot_be_relaxed(limits):
    with pytest.raises(ValueError):
        SceneSnapshots(bridge(), **limits)


@pytest.mark.parametrize("paths", [[], [A, A], [A] * 21, [None], [" "], "not-list"])
async def test_invalid_capture_selection_fails_before_editor(paths):
    connection = bridge()
    with pytest.raises(JevError):
        await SceneSnapshots(connection).capture(paths)
    connection.call.assert_not_called()


@pytest.mark.parametrize(
    "mutation",
    [
        "missing_label",
        "missing_truncation",
        "truncated",
        "duplicate_material",
        "missing_material_path",
        "nonfinite",
        "bool_vector",
        "string_vector",
        "invalid_bounds",
        "bounds_flag",
        "missing_actor",
        "extra_actor",
        "duplicate_actor",
    ],
)
def test_incomplete_or_malformed_records_never_pass(mutation):
    value = details()
    item = value["actors"][0]
    if mutation == "missing_label":
        item.pop("label")
    elif mutation == "missing_truncation":
        value.pop("truncated")
    elif mutation == "truncated":
        value["truncated"] = True
    elif mutation == "duplicate_material":
        item["materials"].append(deepcopy(item["materials"][0]))
    elif mutation == "missing_material_path":
        item["materials"][0].pop("path")
    elif mutation in {"nonfinite", "bool_vector", "string_vector"}:
        item["location"][0] = {
            "nonfinite": float("nan"),
            "bool_vector": True,
            "string_vector": "50",
        }[mutation]
    elif mutation == "invalid_bounds":
        item["bounds_cm"]["size"] = [200, 100, 100]
    elif mutation == "bounds_flag":
        item["bounds_available"] = False
    elif mutation == "missing_actor":
        value["actors"] = []
    elif mutation == "extra_actor":
        value["actors"].append(actor(B))
    else:
        value["actors"].append(actor())
    result = verify([label_check()], value)
    assert result["status"] == "unverifiable"
    assert all(check["status"] == "unverifiable" for check in result["checks"])


def test_known_unavailable_bounds_preserve_metadata_checks_but_not_spatial_proof():
    value = details()
    value["actors"][0].update(bounds_available=False, bounds_cm=None)
    checks = [label_check(), {"kind": "bottom_z", "actor_path": A, "expected_cm": 0}]
    result = verify(checks, value)
    assert [check["status"] for check in result["checks"]] == ["passed", "unverifiable"]
    assert result["status"] == "unverifiable"


async def test_snapshot_preserves_explicit_absent_bounds():
    value = details()
    value["actors"][0].update(bounds_available=False, bounds_cm=None)
    snapshots = SceneSnapshots(bridge(value))
    captured = await snapshots.capture([A])
    assert captured["actors"][0]["bounds_cm"] is None
    assert snapshots.compare(captured["snapshot_id"], value)["status"] == "unchanged"


@pytest.mark.parametrize(
    "check",
    [
        {"kind": "transform_equals", "actor_path": A, "location": [50.05, 50, 50]},
        {"kind": "transform_equals", "actor_path": A, "rotation": [360, -360, 720]},
        {"kind": "bounds_size", "actor_path": A, "expected_cm": [100, 100, 100]},
        {"kind": "bottom_z", "actor_path": A, "expected_cm": 0},
        {"kind": "folder", "actor_path": A, "expected": ""},
        label_check(),
    ],
)
def test_supported_known_checks_pass(check):
    assert verify([check], details())["status"] == "passed"


def test_rotation_verification_handles_gimbal_equivalence():
    value = details()
    value["actors"][0]["rotation"] = [90, 0, 0]
    check = {"kind": "transform_equals", "actor_path": A, "rotation": [90, 90, 90]}
    assert verify([check], value)["status"] == "passed"


def test_null_material_is_known_unassigned_but_absent_slot_is_not():
    value = details()
    value["actors"][0]["materials"][0]["path"] = None
    check = {"kind": "material_slot", "actor_path": A, "slot": 0, "expected_path": None}
    assert verify([check], value)["status"] == "passed"
    assert verify([{**check, "expected_path": "/Game/M.M"}], value)["status"] == "failed"
    assert verify([{**check, "slot": 1}], value)["status"] == "unverifiable"


def test_material_truncation_does_not_discard_other_field_or_observed_slot_evidence():
    value = details()
    value["actors"][0]["materials_truncated"] = True
    value["actors"][0]["materials"][0]["path"] = None
    checks = [
        label_check(),
        {"kind": "transform_equals", "actor_path": A, "location": [50, 50, 50]},
        {"kind": "bounds_size", "actor_path": A, "expected_cm": [100, 100, 100]},
        {"kind": "material_slot", "actor_path": A, "slot": 0, "expected_path": None},
    ]
    assert verify(checks, value)["status"] == "passed"
    checks.append({"kind": "material_slot", "actor_path": A, "slot": 1, "expected_path": None})
    result = verify(checks, value)
    assert result["status"] == "unverifiable"
    assert result["checks"][-1]["reason"] == "material_slot_unavailable"
    assert all(check["status"] == "passed" for check in result["checks"][:-1])


async def test_snapshot_and_diff_still_require_complete_material_records():
    connection = bridge()
    snapshots = SceneSnapshots(connection)
    captured = await snapshots.capture([A])
    connection.call.return_value["actors"][0]["materials_truncated"] = True
    with pytest.raises(JevError) as caught:
        await snapshots.capture([A])
    assert caught.value.code == "invalid_actor_details"
    result = await snapshots.diff(captured["snapshot_id"])
    assert result["status"] == "unverifiable"
    assert result["reason"] == "invalid_actor_details"


async def test_same_path_replacement_is_reported_without_comparing_clone_as_original():
    snapshots = SceneSnapshots(bridge())
    captured = await snapshots.capture([A])
    assert captured["actors"][0]["instance_id"] == "instance-A"
    clone = details()
    clone["actors"][0]["instance_id"] = "replacement-instance"
    result = snapshots.compare(captured["snapshot_id"], clone)
    assert result["status"] == "unverifiable"
    assert result["reason"] == "actor_replaced"
    assert result["changed"] == result["added"] == result["removed"] == []
    assert result["replaced"] == [
        {
            "actor_path": A,
            "before_instance_id": "instance-A",
            "after_instance_id": "replacement-instance",
        }
    ]


def test_expected_actor_instance_binds_single_actor_checks_to_original_object():
    check = label_check(expected_instance_id="instance-A")
    assert verify([check], details())["status"] == "passed"
    clone = details()
    clone["actors"][0]["instance_id"] = "replacement-instance"
    result = verify([check], clone)
    assert result["status"] == "unverifiable"
    assert result["checks"][0]["reason"] == "actor_replaced"
    assert result["checks"][0]["expected"] == "instance-A"
    assert result["checks"][0]["actual"] == "replacement-instance"
    # Without an expected token the explicit contract checks the current object at that path.
    assert verify([label_check()], clone)["status"] == "passed"


@pytest.mark.parametrize("changed_index", [0, 1])
def test_gap_can_bind_both_actor_instances(changed_index):
    value = details([actor(), actor(B, 150)])
    check = {
        "kind": "min_gap",
        "first_actor_path": A,
        "second_actor_path": B,
        "first_expected_instance_id": "instance-A",
        "second_expected_instance_id": "instance-B",
        "axis": "x",
        "minimum_cm": 50,
    }
    assert verify([check], value)["status"] == "passed"
    value["actors"][changed_index]["instance_id"] = "clone"
    assert verify([check], value)["checks"][0]["reason"] == "actor_replaced"


@pytest.mark.parametrize("value", [None, "", " ", "id\n", "id\x7f", "id\ud800", True])
def test_missing_or_invalid_instance_identity_is_unverifiable(value):
    inspected = details()
    if value is None:
        inspected["actors"][0].pop("instance_id")
    else:
        inspected["actors"][0]["instance_id"] = value
    assert verify([label_check()], inspected)["reason"] == "invalid_actor_details"


@pytest.mark.parametrize(
    "field", ["project_file", "session_id", "world_path", "current_level", "revision"]
)
@pytest.mark.parametrize("value", [" ", "bad\nidentity", "bad\x7fidentity", "bad\ud800"])
def test_inspection_identity_cannot_be_blank_or_contain_controls(field, value):
    assert verify([label_check()], details(**{field: value}))["reason"] == "invalid_actor_details"


def test_mutated_expected_identity_is_revalidated_and_bad_revision_rejected():
    identity = SessionIdentity(
        project_file=PROJECT, session_id="session", world_path="/Temp/Test.Test"
    )
    identity.session_id = " "
    with pytest.raises(JevError, match="Expected identity"):
        verify([label_check()], details(), expected_identity=identity)
    for value in [" ", "revision\n", "\ud800", True]:
        with pytest.raises(JevError, match="Expected revision"):
            verify([label_check()], details(), expected_revision=value)


@pytest.mark.parametrize(
    "count,truncated,slots,expected",
    [
        (1, False, [0], "passed"),
        (0, False, [], "passed"),
        (65, True, [0], "passed"),
        (2, False, [0], "unverifiable"),
        (0, False, [0], "unverifiable"),
        (1, True, [0], "unverifiable"),
        (2, False, [0, 2], "unverifiable"),
        (True, False, [0], "unverifiable"),
    ],
)
def test_declared_material_count_must_be_consistent(count, truncated, slots, expected):
    value = details()
    value["actors"][0].update(
        material_slot_count=count,
        materials_truncated=truncated,
        materials=[{"slot": slot, "path": None} for slot in slots],
    )
    assert verify([label_check()], value)["status"] == expected


@pytest.mark.parametrize("axis,index", [("x", 0), ("y", 1), ("z", 2)])
@pytest.mark.parametrize("anchor", ["min", "center", "max"])
def test_bounds_anchor_checks_axis_and_tolerance(axis, index, anchor):
    value = details()
    coordinate = value["actors"][0]["bounds_cm"][anchor][index]
    check = {
        "kind": "bounds_anchor",
        "actor_path": A,
        "axis": axis,
        "anchor": anchor,
        "expected_cm": coordinate + 0.05,
    }
    assert verify([check], value)["status"] == "passed"
    assert verify([{**check, "expected_cm": coordinate + 1}], value)["status"] == "failed"
    value["actors"][0].update(bounds_available=False, bounds_cm=None)
    assert verify([check], value)["checks"][0]["reason"] == "bounds_unavailable"


@pytest.mark.parametrize(
    "offset,minimum,expected", [(150, 50, "passed"), (150, 51, "failed"), (90, 0, "failed")]
)
def test_gap_is_symmetric_world_aabb_axis_separation(offset, minimum, expected):
    check = {
        "kind": "min_gap",
        "first_actor_path": A,
        "second_actor_path": B,
        "axis": "x",
        "minimum_cm": minimum,
        "tolerance_cm": 0,
    }
    result = verify([check], details([actor(), actor(B, offset)]))
    assert result["status"] == expected
    assert result["checks"][0]["actual"]["gap_cm"] == offset - 100
    assert "not exact geometry" in result["checks"][0]["scope"]


def test_no_checks_and_duplicate_ids_never_produce_a_vacuous_pass():
    with pytest.raises(JevError):
        verify([], details())
    with pytest.raises(JevError):
        verify([label_check(id="same"), label_check(id="same")], details())


@pytest.mark.parametrize(
    "check",
    [
        {"kind": "transform_equals", "actor_path": A},
        {"kind": "transform_equals", "actor_path": A, "location": [True, 0, 0]},
        {"kind": "transform_equals", "actor_path": A, "scale": [float("inf"), 1, 1]},
        {"kind": "bottom_z", "actor_path": A, "expected_cm": 0, "tolerance_cm": 101},
        {"kind": "bottom_z", "actor_path": A, "expected_cm": 0, "tolerance_cm": -1},
        {"kind": "bottom_z", "actor_path": A, "expected_cm": 0, "tolerance_cm": float("nan")},
        {
            "kind": "min_gap",
            "first_actor_path": A,
            "second_actor_path": A,
            "axis": "x",
            "minimum_cm": 0,
        },
        {"kind": "material_slot", "actor_path": A, "slot": True, "expected_path": None},
        {"kind": "material_slot", "actor_path": A, "slot": 64, "expected_path": None},
        {"kind": "run_python", "actor_path": A, "code": "pass"},
    ],
)
async def test_bad_checks_fail_before_fresh_read(check):
    connection = bridge()
    with pytest.raises(JevError):
        await verify_fresh(connection, [check])
    connection.call.assert_not_called()


async def test_checks_and_selected_actor_counts_are_bounded_before_read():
    connection = bridge()
    for checks in ([label_check()] * 65, [label_check(path=f"/Temp/Actor_{i}") for i in range(21)]):
        with pytest.raises(JevError):
            await verify_fresh(connection, checks)
    connection.call.assert_not_called()


def test_mutated_typed_checks_cannot_bypass_finite_validation():
    check = TransformEquals(kind="transform_equals", actor_path=A, location=[0, 0, 0])
    check.location[0] = float("nan")
    with pytest.raises(JevError):
        verify([check], details())


def test_expected_session_and_revision_are_explicit_staleness_checks():
    value = details()
    identity = {key: value[key] for key in ("project_file", "session_id", "world_path")}
    assert (
        verify([label_check()], value, expected_identity=identity, expected_revision="revision-a")[
            "status"
        ]
        == "passed"
    )
    assert (
        verify([label_check()], value, expected_revision="revision-b")["reason"]
        == "revision_changed"
    )
    identity["session_id"] = "old-session"
    assert (
        verify([label_check()], value, expected_identity=identity)["reason"] == "identity_changed"
    )


async def test_fresh_verification_rechecks_exact_paths_and_rejects_wrong_project():
    connection = bridge(details(project_file="C:/Different/Game.uproject"))
    assert (await verify_fresh(connection, [label_check()]))["reason"] == "wrong_project"
    connection.call.assert_awaited_once_with("actor_details", {"actor_paths": [A]})
    with pytest.raises(JevError) as caught:
        await SceneSnapshots(connection).capture([A])
    assert caught.value.code == "wrong_project"


async def test_missing_actor_on_fresh_verification_never_claims_success():
    connection = bridge()
    connection.call.side_effect = JevError("actor_not_found", "private backend message")
    result = await verify_fresh(connection, [label_check()])
    assert result["status"] == "unverifiable"
    assert result["checks"][0]["actual"] is None
    assert "private backend message" not in json.dumps(result)


def test_failure_remains_visible_when_other_checks_are_unverifiable():
    value = details()
    value["actors"][0].update(bounds_available=False, bounds_cm=None)
    result = verify(
        [label_check(expected="Wrong"), {"kind": "bottom_z", "actor_path": A, "expected_cm": 0}],
        value,
    )
    assert result["status"] == "failed"
    assert [check["status"] for check in result["checks"]] == ["failed", "unverifiable"]
