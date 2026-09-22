"""Offline geometry and state-bound preview checks; these do not run Unreal."""

from copy import deepcopy
from decimal import Inexact, localcontext
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest
from pydantic import TypeAdapter, ValidationError

from jev_unreal.errors import JevError
from jev_unreal.spatial import (
    AlignRecipe,
    DistributeRecipe,
    GroundRecipe,
    SnapGridRecipe,
    SpatialRecipe,
    compile_spatial,
    preview_spatial,
)
from jev_unreal.verification import Check, verify

PATHS = [f"/Temp/World.World:PersistentLevel.Mesh_{name}" for name in "ABCD"]


def actor(index=0, *, location=(100, 200, 300), low=(80, 150, 250), high=(120, 210, 390)):
    return {
        "path": PATHS[index],
        "instance_id": f"actor-instance-{index}",
        "location": list(location),
        "rotation": [12.0, 37.0, -8.0],
        "scale": [2.0, 1.0, 0.5],
        "bounds_available": True,
        "bounds_cm": {
            "min": list(low),
            "max": list(high),
            "center": [(a + b) / 2 for a, b in zip(low, high, strict=True)],
            "size": [b - a for a, b in zip(low, high, strict=True)],
        },
        "editable": True,
        "edit_blockers": [],
        "materials_truncated": False,
    }


def snapshot(*actors):
    return {
        "project_file": "C:/Sandbox/Spatial.uproject",
        "session_id": "session-a",
        "world_path": "/Temp/World.World",
        "current_level": "/Temp/World",
        "revision": "revision-1",
        "truncated": False,
        "actors": list(actors) or [actor()],
    }


def line_actor(index, low, high, *, axis=0):
    minimum, maximum, location = [-5.0] * 3, [5.0] * 3, [0.0] * 3
    minimum[axis], maximum[axis] = low, high
    # The pivot is deliberately not the bounds center.
    location[axis] = low + 3
    return actor(index, location=location, low=minimum, high=maximum)


@pytest.mark.parametrize("axis,index", [("x", 0), ("y", 1), ("z", 2)])
@pytest.mark.parametrize("anchor", ["min", "center", "max"])
def test_alignment_uses_world_bounds_and_preserves_other_transform_axes(axis, index, anchor):
    details = snapshot(actor())
    untouched = deepcopy(details)
    result = compile_spatial(
        AlignRecipe(kind="align", actor_paths=PATHS[:1], axis=axis, anchor=anchor, target_cm=-150),
        details,
    )
    operation = result["operations"][0]
    before = details["actors"][0]
    expected = before["location"][:]
    expected[index] += -150 - before["bounds_cm"][anchor][index]
    assert operation == {
        "op": "set_transform",
        "actor_path": PATHS[0],
        "location": expected,
        "rotation": before["rotation"],
        "scale": before["scale"],
    }
    assert result["actors"][0]["after"]["bounds_cm"][anchor][index] == pytest.approx(-150)
    assert result["verification_targets"][0] == {
        "path": PATHS[0],
        **result["actors"][0]["after"],
    }
    assert result["applied"] is False
    assert result["cloud_used"] is False
    assert details == untouched


def test_ground_moves_each_bottom_to_height_with_off_center_pivots():
    details = snapshot(
        actor(), actor(1, location=(10, 20, -100), low=(0, 0, -200), high=(20, 40, 0))
    )
    result = compile_spatial(GroundRecipe(kind="ground", actor_paths=PATHS[:2], z_cm=25), details)
    assert [op["location"][2] for op in result["operations"]] == [75, 125]
    assert [target["bounds_cm"]["min"][2] for target in result["verification_targets"]] == [25, 25]
    assert [target["bounds_cm"]["size"] for target in result["verification_targets"]] == [
        [40, 60, 140],
        [20, 40, 200],
    ]


@pytest.mark.parametrize("axis,index", [("x", 0), ("y", 1), ("z", 2)])
def test_distribute_centers_preserves_endpoint_centers_and_noncentral_pivots(axis, index):
    actors = [
        line_actor(0, -10, 10, axis=index),
        line_actor(1, 15, 45, axis=index),
        line_actor(2, 90, 110, axis=index),
    ]
    result = compile_spatial(
        DistributeRecipe(kind="distribute", actor_paths=PATHS[:3], axis=axis, mode="centers"),
        snapshot(*actors),
    )
    assert [item["after"]["bounds_cm"]["center"][index] for item in result["actors"]] == [
        0,
        50,
        100,
    ]
    assert result["operations"][0]["location"] == actors[0]["location"]
    assert result["operations"][2]["location"] == actors[2]["location"]
    assert result["operations"][1]["location"][index] == 38
    assert result["measurements"]["center_spacing_cm"] == 50


def test_distribute_ties_sort_by_exact_path_but_operations_keep_requested_order():
    actors = [
        line_actor(1, -10, 10),
        line_actor(3, 290, 310),
        line_actor(0, -10, 10),
        line_actor(2, 40, 60),
    ]
    paths = [item["path"] for item in actors]
    result = compile_spatial(
        DistributeRecipe(kind="distribute", actor_paths=paths, axis="x"), snapshot(*actors)
    )
    assert result["measurements"]["axis_order"] == PATHS
    assert [op["actor_path"] for op in result["operations"]] == paths
    assert [item["after"]["bounds_cm"]["center"][0] for item in result["actors"]] == [
        100,
        300,
        0,
        200,
    ]


@pytest.mark.parametrize("axis,index", [("x", 0), ("y", 1), ("z", 2)])
def test_equal_gaps_uses_different_widths_and_preserves_global_outer_bounds(axis, index):
    # The second actor extends farther left than the first: use global outer
    # bounds, not the min of the first actor in center order.
    actors = [
        line_actor(0, -10, 10, axis=index),
        line_actor(1, -20, 40, axis=index),
        line_actor(2, 90, 110, axis=index),
    ]
    result = compile_spatial(
        DistributeRecipe(kind="distribute", actor_paths=PATHS[:3], axis=axis, mode="equal_gaps"),
        snapshot(*actors),
    )
    bounds = [target["bounds_cm"] for target in result["verification_targets"]]
    assert [bound["min"][index] for bound in bounds] == [-20, 15, 90]
    assert [bound["max"][index] for bound in bounds] == [0, 75, 110]
    assert result["measurements"]["gap_cm"] == 15
    assert result["measurements"]["outer_min_cm"] == -20
    assert result["measurements"]["outer_max_cm"] == 110


def test_equal_gaps_accepts_touching_and_rejects_overlap_that_cannot_fit():
    recipe = DistributeRecipe(kind="distribute", actor_paths=PATHS[:3], axis="x", mode="equal_gaps")
    result = compile_spatial(
        recipe, snapshot(line_actor(0, 0, 10), line_actor(1, 10, 30), line_actor(2, 30, 60))
    )
    assert result["measurements"]["gap_cm"] == 0
    with pytest.raises(JevError, match="cannot fit") as caught:
        compile_spatial(
            recipe, snapshot(line_actor(0, -50, 50), line_actor(1, -40, 60), line_actor(2, -30, 70))
        )
    assert caught.value.code == "invalid_spatial_recipe"


@pytest.mark.parametrize(
    "location,grid,origin,expected",
    [
        ([5, -5, 15], 10, [0, 0, 0], [10, -10, 20]),
        ([4.99, -4.99, -15], 10, [0, 0, 0], [0, 0, -20]),
        ([0.15, -0.15, 0.35], 0.1, [0, 0, 0], [0.2, -0.2, 0.4]),
        ([105, 95, 100], 10, [100, 100, 100], [110, 90, 100]),
        ([6, 12, -16], [10, 20, 30], [1, 2, -1], [11, 22, -31]),
        ([1, -1, 1.5], 3, [0, 0, 0], [0, 0, 3]),
    ],
)
def test_snap_grid_decimal_ties_away_from_origin_and_axis_spacing(location, grid, origin, expected):
    recipe = SnapGridRecipe(kind="snap_grid", actor_paths=PATHS[:1], grid_cm=grid, origin_cm=origin)
    with localcontext() as context:
        context.prec = 3  # The caller cannot change spatial rounding semantics.
        context.traps[Inexact] = True
        result = compile_spatial(recipe, snapshot(actor(location=location)))
    assert result["operations"][0]["location"] == expected
    assert result["measurements"] == {"anchor": "actor_pivot", "half_grid_ties": "away_from_origin"}


def test_maximum_selection_and_snap_default_origin():
    actors = [actor() for _ in range(20)]
    for index, item in enumerate(actors):
        item["path"] = f"/Temp/World.World:PersistentLevel.Actor_{index}"
        item["instance_id"] = f"actor-instance-{index}"
    recipe = SnapGridRecipe(kind="snap_grid", actor_paths=[a["path"] for a in actors], grid_cm=10)
    result = compile_spatial(recipe, snapshot(*actors))
    assert result["operation_count"] == 20
    assert result["recipe"]["origin_cm"] == [0, 0, 0]
    assert len(result["verification_checks"]) == 40
    grounded = compile_spatial(
        GroundRecipe(kind="ground", actor_paths=recipe.actor_paths, z_cm=0), snapshot(*actors)
    )
    assert len(grounded["verification_checks"]) == 60
    TypeAdapter(list[Check]).validate_python(grounded["verification_checks"])
    aligned = compile_spatial(
        AlignRecipe(kind="align", actor_paths=recipe.actor_paths, axis="x", target_cm=0),
        snapshot(*actors),
    )
    assert len(aligned["verification_checks"]) == 60
    TypeAdapter(list[Check]).validate_python(aligned["verification_checks"])


@pytest.mark.parametrize(
    "patch",
    [
        {"actor_paths": []},
        {"actor_paths": PATHS[:1] * 2},
        {"actor_paths": [f"/Game/W.A_{i}" for i in range(21)]},
        {"actor_paths": "actor"},
        {"actor_paths": [True]},
        {"actor_paths": ["Cube"]},
        {"actor_paths": ["/Temp/W.Actor "]},
        {"actor_paths": ["/Temp/W.Actor\n"]},
        {"actor_paths": ["/Temp/W.*"]},
        {"actor_paths": ["/Temp/../W.Actor"]},
        {"actor_paths": ["/Temp/W.\ud800"]},
        {"actor_paths": ["/Temp/W." + "\U0001f600" * 600]},
        {"target_cm": True},
        {"target_cm": "10"},
        {"target_cm": float("nan")},
        {"target_cm": float("inf")},
        {"target_cm": 1_000_001},
        {"axis": "pitch"},
        {"anchor": "pivot"},
        {"extra": "value"},
        {"kind": "execute"},
    ],
)
def test_invalid_recipes_are_strict_and_bounded(patch):
    value = {"kind": "align", "actor_paths": PATHS[:1], "axis": "x", "target_cm": 10, **patch}
    with pytest.raises(ValidationError):
        TypeAdapter(SpatialRecipe).validate_python(value)


@pytest.mark.parametrize(
    "grid", [0, -1, 0.0001, True, "10", float("inf"), [10, 20], [10, 20, False], [10, 0, 30]]
)
def test_invalid_grids(grid):
    with pytest.raises(ValidationError):
        SnapGridRecipe(kind="snap_grid", actor_paths=PATHS[:1], grid_cm=grid)


def test_distribution_minimum_and_json_array_schema():
    with pytest.raises(ValidationError, match="at least three"):
        DistributeRecipe(kind="distribute", actor_paths=PATHS[:2], axis="x")
    parsed = TypeAdapter(SpatialRecipe).validate_json(
        '{"kind":"snap_grid","actor_paths":["/Temp/World.Actor"],"grid_cm":[10,20,30]}'
    )
    assert isinstance(parsed, SnapGridRecipe)
    assert parsed.grid_cm == [10, 20, 30]
    assert TypeAdapter(SpatialRecipe).json_schema()["discriminator"]["propertyName"] == "kind"


@pytest.mark.parametrize(
    "field,value",
    [
        ("truncated", True),
        ("truncated", 0),
        ("truncated", None),
        ("revision", ""),
        ("session_id", " "),
        ("world_path", None),
        ("project_file", "project\nfile"),
        ("current_level", ""),
        ("actors", []),
        ("actors", "actors"),
        ("actors_truncated", True),
        ("actors_truncated", 0),
        ("scan_incomplete", True),
    ],
)
def test_invalid_snapshot_envelopes(field, value):
    details = snapshot()
    details[field] = value
    with pytest.raises(JevError) as caught:
        compile_spatial(GroundRecipe(kind="ground", actor_paths=PATHS[:1], z_cm=0), details)
    assert caught.value.code == "invalid_actor_details"


@pytest.mark.parametrize(
    "field,value",
    [
        ("bounds_available", False),
        ("bounds_available", 1),
        ("bounds_cm", None),
        ("editable", False),
        ("editable", 1),
        ("edit_blockers", ["attached_actor"]),
        ("location", [float("nan"), 0, 0]),
        ("location", [1_000_001, 0, 0]),
        ("location", [True, 0, 0]),
        ("location", ["0", 0, 0]),
        ("rotation", [0, float("inf"), 0]),
        ("rotation", [0, 36001, 0]),
        ("scale", [0, 1, 1]),
        ("scale", [-1, 1, 1]),
        ("scale", [1001, 1, 1]),
        ("scale", [1, 1]),
    ],
)
def test_invalid_actor_measurements(field, value):
    item = actor()
    item[field] = value
    with pytest.raises(JevError) as caught:
        compile_spatial(GroundRecipe(kind="ground", actor_paths=PATHS[:1], z_cm=0), snapshot(item))
    assert caught.value.code == "invalid_actor_details"


@pytest.mark.parametrize(
    "field,value",
    [
        ("min", [121, 150, 250]),
        ("min", [float("nan"), 150, 250]),
        ("max", [float("inf"), 210, 390]),
        ("center", [101, 180, 320]),
        ("size", [41, 60, 140]),
        ("size", [-40, 60, 140]),
        ("size", [True, 60, 140]),
        ("min", [1_000_000_001, 150, 250]),
        ("center", [100, 180]),
    ],
)
def test_inconsistent_or_invalid_bounds_are_rejected(field, value):
    item = actor()
    item["bounds_cm"][field] = value
    with pytest.raises(JevError) as caught:
        compile_spatial(GroundRecipe(kind="ground", actor_paths=PATHS[:1], z_cm=0), snapshot(item))
    assert caught.value.code == "invalid_actor_details"


@pytest.mark.parametrize("indices", [[1, 0], [0, 0], [0], [0, 1, 2], [0, 2]])
def test_exact_requested_identities_and_order_are_required(indices):
    with pytest.raises(JevError, match="exact requested"):
        compile_spatial(
            GroundRecipe(kind="ground", actor_paths=PATHS[:2], z_cm=0),
            snapshot(*(actor(index) for index in indices)),
        )


def test_missing_completeness_fields_reject_but_material_truncation_is_irrelevant():
    details = snapshot()
    details["actors"][0]["materials_truncated"] = True
    recipe = GroundRecipe(kind="ground", actor_paths=PATHS[:1], z_cm=0)
    assert compile_spatial(recipe, details)["operation_count"] == 1
    del details["truncated"]
    with pytest.raises(JevError):
        compile_spatial(recipe, details)
    details = snapshot()
    del details["actors"][0]["editable"]
    with pytest.raises(JevError):
        compile_spatial(recipe, details)


@pytest.mark.parametrize(
    "recipe",
    [
        AlignRecipe(
            kind="align", actor_paths=PATHS[:1], axis="x", anchor="min", target_cm=1_000_000
        ),
        GroundRecipe(kind="ground", actor_paths=PATHS[:1], z_cm=1_000_000),
        SnapGridRecipe(
            kind="snap_grid", actor_paths=PATHS[:1], grid_cm=1_000_000, origin_cm=[999999, 0, 0]
        ),
    ],
)
def test_computed_locations_cannot_exceed_editor_limits(recipe):
    item = actor(location=(-900000, 0, 100), low=(-900100, -50, 0), high=(-899900, 50, 200))
    with pytest.raises(JevError, match="Calculated locations") as caught:
        compile_spatial(recipe, snapshot(item))
    assert caught.value.code == "invalid_spatial_recipe"


def test_computed_bounds_cannot_exceed_measurement_limits():
    item = actor(location=(0, 0, 0), low=(-1_000_000_000, 0, 0), high=(0, 0, 0))
    recipe = AlignRecipe(kind="align", actor_paths=PATHS[:1], axis="x", anchor="max", target_cm=-1)
    with pytest.raises(JevError, match="Calculated world bounds"):
        compile_spatial(recipe, snapshot(item))


async def test_preview_reads_once_and_passes_exact_measurement_state_without_applying():
    bridge, previews = AsyncMock(), AsyncMock()
    bridge.call.return_value = snapshot()
    previews.preview.return_value = {"plan_id": "one-shot-id", "expires_in_seconds": 120}
    recipe = GroundRecipe(kind="ground", actor_paths=PATHS[:1], z_cm=0)
    result = await preview_spatial(bridge, previews, recipe)
    bridge.call.assert_awaited_once_with("actor_details", {"actor_paths": PATHS[:1]})
    previews.preview.assert_awaited_once_with(
        result["operations"],
        expected_state={
            "session_id": "session-a",
            "world_path": "/Temp/World.World",
            "revision": "revision-1",
        },
    )
    assert result["preview"]["plan_id"] == "one-shot-id"
    assert result["applied"] is False
    previews.apply.assert_not_called()


async def test_bad_measurements_never_reach_preview_and_stale_preview_is_not_retried():
    bridge, previews = AsyncMock(), AsyncMock()
    bridge.call.return_value = snapshot()
    bridge.call.return_value["truncated"] = True
    recipe = GroundRecipe(kind="ground", actor_paths=PATHS[:1], z_cm=0)
    with pytest.raises(JevError):
        await preview_spatial(bridge, previews, recipe)
    previews.preview.assert_not_called()
    bridge.call.return_value = snapshot()
    previews.preview.side_effect = JevError("stale_state", "The world changed.")
    with pytest.raises(JevError) as caught:
        await preview_spatial(bridge, previews, recipe)
    assert caught.value.code == "stale_state"
    assert bridge.call.await_count == 2
    assert previews.preview.await_count == 1


async def test_mutated_recipe_is_revalidated_before_inspection():
    recipe = GroundRecipe(kind="ground", actor_paths=PATHS[:1], z_cm=0)
    recipe.actor_paths.append(PATHS[0])
    bridge, previews = AsyncMock(), AsyncMock()
    with pytest.raises(ValidationError):
        await preview_spatial(bridge, previews, recipe)
    bridge.call.assert_not_called()
    previews.preview.assert_not_called()


async def test_wrong_project_measurement_cannot_reach_preview():
    bridge, previews = AsyncMock(), AsyncMock()
    bridge.settings = SimpleNamespace(expected_project="C:/Other/Other.uproject")
    bridge.call.return_value = snapshot()
    with pytest.raises(JevError) as caught:
        await preview_spatial(
            bridge, previews, GroundRecipe(kind="ground", actor_paths=PATHS[:1], z_cm=0)
        )
    assert caught.value.code == "wrong_project"
    previews.preview.assert_not_called()
    bridge.settings.expected_project = "c:\\sandbox\\SPATIAL.uproject"
    await preview_spatial(
        bridge, previews, GroundRecipe(kind="ground", actor_paths=PATHS[:1], z_cm=0)
    )
    previews.preview.assert_awaited_once()


def test_generated_checks_verify_after_edit_snapshot_without_requiring_old_revision():
    details = snapshot()
    details["actors"][0].update(
        {
            "class": "StaticMeshActor",
            "label": "Mesh A",
            "folder": "",
            "static_mesh_path": "/Engine/BasicShapes/Cube.Cube",
            "collision_enabled": True,
            "materials": [],
        }
    )
    result = compile_spatial(GroundRecipe(kind="ground", actor_paths=PATHS[:1], z_cm=25), details)
    expected_identity = {
        field: result["measurement_state"][field]
        for field in ("project_file", "session_id", "world_path")
    }
    after = deepcopy(details)
    after["revision"] = "revision-after-edit"
    after["actors"][0].update(result["actors"][0]["after"])
    checked = verify(result["verification_checks"], after, expected_identity=expected_identity)
    assert checked["status"] == "passed"
    assert [item["kind"] for item in checked["checks"]] == [
        "transform_equals",
        "bounds_size",
        "bottom_z",
    ]
    assert verify(result["verification_checks"], details)["status"] == "failed"


def complete_snapshot(*actors):
    value = snapshot(*actors)
    for item in value["actors"]:
        item.update(
            {
                "class": "StaticMeshActor",
                "label": "Measured mesh",
                "folder": "",
                "static_mesh_path": "/Engine/BasicShapes/Cube.Cube",
                "collision_enabled": True,
                "materials": [],
                "material_slot_count": 0,
            }
        )
    return value


def after_spatial(value, compiled):
    after = deepcopy(value)
    after["revision"] = "revision-after-edit"
    for item, translated in zip(after["actors"], compiled["actors"], strict=True):
        item.update(translated["after"])
    return after


@pytest.mark.parametrize("axis,index", [("x", 0), ("y", 1), ("z", 2)])
@pytest.mark.parametrize("anchor", ["min", "center", "max"])
def test_alignment_verification_detects_bounds_shift_without_transform_or_size_change(
    axis, index, anchor
):
    value = complete_snapshot()
    recipe = AlignRecipe(
        kind="align", actor_paths=PATHS[:1], axis=axis, anchor=anchor, target_cm=100
    )
    compiled = compile_spatial(recipe, value)
    after = after_spatial(value, compiled)
    assert verify(compiled["verification_checks"], after)["status"] == "passed"
    for field in ("min", "center", "max"):
        after["actors"][0]["bounds_cm"][field][index] += 10
    result = verify(compiled["verification_checks"], after)
    assert result["status"] == "failed"
    assert [(row["kind"], row["status"]) for row in result["checks"]] == [
        ("transform_equals", "passed"),
        ("bounds_size", "passed"),
        ("bounds_anchor", "failed"),
    ]


@pytest.mark.parametrize("mode", ["centers", "equal_gaps"])
def test_distribution_verification_detects_independently_shifted_middle_bounds(mode):
    value = complete_snapshot(line_actor(0, 0, 10), line_actor(1, 20, 40), line_actor(2, 80, 100))
    recipe = DistributeRecipe(kind="distribute", actor_paths=PATHS[:3], axis="x", mode=mode)
    compiled = compile_spatial(recipe, value)
    after = after_spatial(value, compiled)
    assert verify(compiled["verification_checks"], after)["status"] == "passed"
    for field in ("min", "center", "max"):
        after["actors"][1]["bounds_cm"][field][0] += 10
    result = verify(compiled["verification_checks"], after)
    failures = [row for row in result["checks"] if row["status"] != "passed"]
    assert result["status"] == "failed"
    assert len(failures) == 1
    assert failures[0]["kind"] == "bounds_anchor"


def test_spatial_checks_cannot_verify_replacement_clone_at_same_path():
    value = complete_snapshot()
    compiled = compile_spatial(GroundRecipe(kind="ground", actor_paths=PATHS[:1], z_cm=25), value)
    assert all(
        check["expected_instance_id"] == "actor-instance-0"
        for check in compiled["verification_checks"]
    )
    after = after_spatial(value, compiled)
    after["actors"][0]["instance_id"] = "replacement-clone"
    result = verify(compiled["verification_checks"], after)
    assert result["status"] == "unverifiable"
    assert all(row["reason"] == "actor_replaced" for row in result["checks"])


@pytest.mark.parametrize(
    "instance_id", [None, "", " ", "token\n", "token\x7f", "token\ud800", True]
)
def test_spatial_requires_well_formed_instance_identity(instance_id):
    value = snapshot()
    if instance_id is None:
        value["actors"][0].pop("instance_id")
    else:
        value["actors"][0]["instance_id"] = instance_id
    with pytest.raises(JevError) as caught:
        compile_spatial(GroundRecipe(kind="ground", actor_paths=PATHS[:1], z_cm=0), value)
    assert caught.value.code == "invalid_actor_details"
