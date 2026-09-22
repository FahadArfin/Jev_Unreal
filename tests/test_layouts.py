"""Measured recipe geometry and native readback checks, without an Unreal process."""

from copy import deepcopy
from unittest.mock import AsyncMock

import pytest
from pydantic import TypeAdapter, ValidationError

from jev_unreal.errors import JevError
from jev_unreal.layouts import (
    GridLayout,
    Layout,
    PreviewTracker,
    RoomLayout,
    StairLayout,
    compile_layout,
    verify_readback,
)


def test_grid_clear_spacing_and_rotated_origin():
    result = compile_layout(
        GridLayout(
            kind="grid",
            rows=2,
            columns=2,
            size_cm=(100, 200, 300),
            gap_cm=(50, 75),
            origin=(1000, 2000, 10),
            yaw_degrees=90,
        )
    )
    operations = result["operations"]
    assert operations[0]["location"] == pytest.approx([900, 2050, 160])
    assert operations[1]["location"] == pytest.approx([900, 2200, 160])
    assert operations[2]["location"] == pytest.approx([625, 2050, 160])
    assert operations[0]["scale"] == [1, 2, 3]
    assert len({op["label"] for op in operations}) == 4
    assert result["cloud_used"] is False


def test_stair_tops_have_exact_rise_and_tread_depth():
    result = compile_layout(StairLayout(kind="stairs", steps=5, rise_cm=20, tread_depth_cm=30))
    for index, operation in enumerate(result["operations"]):
        top = operation["location"][2] + operation["scale"][2] * 50
        assert top == pytest.approx((index + 1) * 20)
        assert operation["location"][0] == pytest.approx((index + 0.5) * 30)
        assert operation["scale"][0] == pytest.approx(0.3)


def test_room_walls_preserve_clear_inner_dimensions():
    result = compile_layout(
        RoomLayout(
            kind="room",
            inner_size_cm=(800, 600, 300),
            wall_thickness_cm=20,
            ceiling=True,
        )
    )
    pieces = {op["label"]: op for op in result["operations"]}
    assert pieces["Jev_West"]["location"][0] + pieces["Jev_West"]["scale"][0] * 50 == 0
    assert pieces["Jev_East"]["location"][0] - pieces["Jev_East"]["scale"][0] * 50 == 800
    assert pieces["Jev_North"]["location"][1] - pieces["Jev_North"]["scale"][1] * 50 == 600
    assert pieces["Jev_Floor"]["location"][2] + pieces["Jev_Floor"]["scale"][2] * 50 == 0
    assert pieces["Jev_Ceiling"]["location"][2] - pieces["Jev_Ceiling"]["scale"][2] * 50 == 300


def test_static_mesh_readback_checks_the_selected_asset():
    operation, actor = readback()
    operation.update(op="spawn_static_mesh", asset_path="/Game/Door.Door")
    actor["static_mesh_path"] = "/Game/Crate.Crate"
    assert verify_readback([operation], [actor])["issues"][0]["code"] == "mesh_identity_mismatch"
    actor["static_mesh_path"] = "/Game/Door.Door"
    assert verify_readback([operation], [actor])["status"] == "passed"


@pytest.mark.parametrize(
    "value",
    [
        {"kind": "grid", "rows": 5, "columns": 5},
        {"kind": "grid", "rows": True},
        {"kind": "grid", "rows": "2"},
        {"kind": "grid", "gap_cm": [10, 20, 30]},
        {"kind": "stairs", "steps": 21},
        {"kind": "room", "inner_size_cm": [0, 10, 10]},
        {"kind": "room", "inner_size_cm": [True, 10, 10]},
        {"kind": "room", "wall_thickness_cm": float("nan")},
        {"kind": "room", "ceiling": "yes"},
        {"kind": "grid", "origin": [0, 0, float("inf")]},
        {"kind": "room", "label_prefix": "bad\nlabel"},
        {"kind": "room", "arbitrary": "field"},
        {"kind": "unknown"},
    ],
)
def test_invalid_layouts_rejected_before_editor(value):
    with pytest.raises(ValidationError):
        TypeAdapter(Layout).validate_python(value)


def test_generated_scale_and_location_bounds_are_enforced():
    with pytest.raises(JevError, match="transform limits"):
        compile_layout(StairLayout(kind="stairs", steps=20, rise_cm=50000))
    with pytest.raises(JevError, match="transform limits"):
        compile_layout(
            GridLayout(kind="grid", rows=1, columns=20, origin=(900000, 0, 0), gap_cm=(50000, 0))
        )


def readback():
    operation = compile_layout(GridLayout(kind="grid", rows=1, columns=1))["operations"][0]
    actor = {key: deepcopy(operation[key]) for key in ("location", "rotation", "scale")}
    actor["path"] = "/Temp/World.Cube"
    return operation, actor


def test_readback_accepts_equivalent_rotation_and_float_precision():
    operation, actor = readback()
    actor["rotation"] = [0, 360, 0]
    actor["location"][0] += 1e-6
    assert verify_readback([operation], [actor])["status"] == "passed"


@pytest.mark.parametrize(
    "field,value",
    [
        ("location", [1, 2, 3]),
        ("rotation", [45, 0, 0]),
        ("scale", [2, 2, 2]),
        ("location", [float("nan"), 0, 0]),
        ("rotation", [True, 0, 0]),
        ("scale", "bad"),
        ("path", None),
        ("path", []),
    ],
)
def test_readback_detects_wrong_or_invalid_editor_results(field, value):
    operation, actor = readback()
    actor[field] = value
    assert verify_readback([operation], [actor])["status"] == "mismatch"


def test_readback_checks_count_identity_and_duplicate_paths():
    operation, actor = readback()
    assert verify_readback([operation], [])["status"] == "mismatch"
    assert verify_readback([operation, operation], [actor, actor])["status"] == "mismatch"
    operation.update(op="set_transform", actor_path="/Temp/World.OtherActor")
    assert verify_readback([operation], [actor])["issues"][0]["code"] == "actor_identity_mismatch"


async def test_preview_tracker_verifies_apply_once_and_never_retries_failure():
    operation, actor = readback()
    bridge = AsyncMock()
    bridge.call.return_value = {"plan_id": "test", "operations": [operation]}
    tracker = PreviewTracker(bridge)
    await tracker.preview([operation])
    bridge.call.return_value = {"applied": True, "actors": [actor]}
    result = await tracker.apply("test")
    assert result["verification"]["status"] == "passed"
    assert len(bridge.call.await_args_list) == 2
    bridge.call.side_effect = JevError("editor_unavailable", "Connection lost")
    with pytest.raises(JevError):
        await tracker.apply("another_plan")
    assert len(bridge.call.await_args_list) == 3


async def test_untracked_native_plan_does_not_claim_verification():
    bridge = AsyncMock()
    bridge.call.return_value = {"applied": True, "actors": []}
    result = await PreviewTracker(bridge).apply("external_plan")
    assert result["verification"]["status"] == "unavailable"


async def test_preview_tracker_is_bounded():
    bridge = AsyncMock()
    tracker = PreviewTracker(bridge)
    for index in range(70):
        bridge.call.return_value = {"plan_id": str(index), "operations": []}
        await tracker.preview([])
    assert len(tracker._plans) == 64


@pytest.mark.parametrize("actors", [None, "invalid", {}, [None], ["invalid"]])
def test_malformed_readback_is_reported_without_raising_after_apply(actors):
    operation, _ = readback()
    assert verify_readback([operation], actors)["status"] == "mismatch"


@pytest.mark.parametrize("expected", [None, "invalid", {}, [None], [{}]])
def test_malformed_preview_expectations_never_hide_a_native_apply_result(expected):
    _, actor = readback()
    assert verify_readback(expected, [actor])["status"] == "mismatch"


def test_unrepresentable_readback_number_is_an_issue_not_an_exception():
    operation, actor = readback()
    actor["location"] = [10**1000, 0, 0]
    assert verify_readback([operation], [actor])["issues"][0]["code"] == "invalid_transform"


@pytest.mark.parametrize(
    "expected,actual",
    [
        ([90, 0, 0], [90, 90, 90]),
        ([100, 0, 0], [80, 180, 180]),
        ([0, 0, 0], [360, -360, 720]),
    ],
)
def test_readback_accepts_gimbal_and_equivalent_euler_orientations(expected, actual):
    operation, actor = readback()
    operation["rotation"] = expected
    actor["rotation"] = actual
    assert verify_readback([operation], [actor])["status"] == "passed"


@pytest.mark.parametrize(
    "native_result,status",
    [
        ({"applied": True, "actors": None}, "mismatch"),
        ({"applied": True, "actors": [None]}, "mismatch"),
        ({"applied": False, "actors": []}, "unavailable"),
        ({"actors": []}, "unavailable"),
        (None, "unavailable"),
    ],
)
async def test_tracker_preserves_post_apply_outcome_with_bad_readback(native_result, status):
    operation, _ = readback()
    bridge = AsyncMock()
    bridge.call.return_value = {"plan_id": "test", "operations": [operation]}
    tracker = PreviewTracker(bridge)
    await tracker.preview([operation])
    bridge.call.return_value = native_result
    result = await tracker.apply("test")
    assert result["verification"]["status"] == status
    if isinstance(native_result, dict):
        assert result is native_result
    assert "test" not in tracker._plans
    assert bridge.call.await_count == 2
