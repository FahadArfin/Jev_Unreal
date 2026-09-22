from unittest.mock import AsyncMock

import pytest
from pydantic import ValidationError

from jev_unreal.errors import JevError
from jev_unreal.workflows import Candidate, ExpectedState, Operation, gate, route, triage


@pytest.mark.parametrize(
    "value",
    [
        {"op": "set_material", "actor_path": "/Temp/A.A", "slot": 0, "material_path": "/Game/M.M"},
        {
            "op": "set_metadata",
            "actor_path": "/Temp/A.A",
            "label": "Crate",
            "folder": "Props/Crates",
        },
        {"op": "set_metadata", "actor_path": "/Temp/A.A", "folder": ""},
    ],
)
def test_material_and_metadata_operations_keep_typed_bounded_arguments(value):
    assert Operation.model_validate(value).model_dump(exclude_none=True) == value


@pytest.mark.parametrize(
    "changes",
    [
        {"slot": True},
        {"slot": -1},
        {"slot": 64},
        {"slot": "0"},
        {"material_path": "C:/material.uasset"},
        {"material_path": "/Game/M.M:Subobject"},
        {"label": "unexpected"},
        {"folder": "unexpected"},
        {"location": [0, 0, 0]},
    ],
)
def test_material_operation_rejects_cross_operation_fields_and_invalid_slots(changes):
    value = {
        "op": "set_material",
        "actor_path": "/Temp/A.A",
        "slot": 0,
        "material_path": "/Game/M.M",
        **changes,
    }
    with pytest.raises(ValidationError):
        Operation.model_validate(value)


@pytest.mark.parametrize(
    "folder",
    ["/Root", "A//B", "A/../B", "A/./B", "A\\B", "A:B", "A\x00B", " A", "A/ ", "A/", "😀" * 129],
)
def test_metadata_folder_rejects_invalid_paths_and_utf16_overflow(folder):
    with pytest.raises(ValidationError):
        Operation(op="set_metadata", actor_path="/Temp/A.A", folder=folder)


def test_metadata_needs_an_explicit_change_and_measured_state_is_complete():
    with pytest.raises(ValidationError):
        Operation(op="set_metadata", actor_path="/Temp/A.A")
    with pytest.raises(ValidationError):
        ExpectedState(session_id="s", world_path="w")
    with pytest.raises(ValidationError):
        ExpectedState(session_id="s", world_path="w", revision="r", skip_checks=True)


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


@pytest.mark.parametrize(
    "field,value",
    [
        ("location", ["100", 0, 0]),
        ("location", [True, 0, 0]),
        ("rotation", [False, 0, 0]),
        ("scale", ["1", 1, 1]),
        ("location", [1000001, 0, 0]),
        ("rotation", [0, 36001, 0]),
        ("scale", [0, 1, 1]),
        ("scale", [1001, 1, 1]),
        ("scale", [-1, 1, 1]),
        ("location", [float("nan"), 0, 0]),
        ("rotation", [float("inf"), 0, 0]),
        ("label", "   "),
        ("label", "line\nbreak"),
        ("label", "null\x00character"),
        ("label", "delete\x7fcharacter"),
        ("label", "\U0001f600" * 41),
    ],
)
def test_operation_rejects_coercion_and_native_limit_violations(field, value):
    with pytest.raises(ValidationError):
        Operation.model_validate(
            {
                "op": "spawn_primitive",
                "shape": "Cube",
                "label": "Box",
                field: value,
            }
        )


def test_operation_accepts_json_lists_with_exact_numeric_native_boundaries():
    operation = Operation.model_validate(
        {
            "op": "spawn_primitive",
            "shape": "Cube",
            "label": "Box",
            "location": [-1000000, 0, 1000000],
            "rotation": [-36000, 0, 36000],
            "scale": [0.001, 1, 1000],
        }
    )
    assert operation.model_dump(mode="json")["location"] == [-1000000, 0, 1000000]
    assert (
        Operation.model_validate(
            {
                "op": "set_transform",
                "actor_path": "/Temp/World.Box",
                "scale": [1, 2, 3],
            }
        ).shape
        is None
    )


@pytest.mark.parametrize(
    "operation",
    [
        {"op": "spawn_primitive", "shape": "Cube"},
        {"op": "spawn_primitive", "label": "Box"},
        {"op": "spawn_primitive", "shape": "Cube", "label": "Box", "actor_path": "/Temp/Box"},
        {"op": "set_transform", "location": [1, 2, 3]},
        {"op": "set_transform", "actor_path": "/Temp/Box"},
        {"op": "set_transform", "actor_path": " ", "scale": [1, 1, 1]},
        {"op": "set_transform", "actor_path": "/Temp/Box", "label": "Box", "scale": [1, 1, 1]},
        {"op": "set_transform", "actor_path": "/Temp/Box", "shape": "Cube", "scale": [1, 1, 1]},
    ],
)
def test_operation_requires_only_fields_for_its_native_action(operation):
    with pytest.raises(ValidationError):
        Operation.model_validate(operation)


def test_static_mesh_spawn_requires_exact_asset_and_no_cross_operation_fields():
    valid = {
        "op": "spawn_static_mesh",
        "asset_path": "/Engine/BasicShapes/Cube.Cube",
        "label": "ExistingMesh",
        "location": [0, 0, 50],
    }
    assert Operation.model_validate(valid).asset_path == valid["asset_path"]
    for replacement in [
        {"asset_path": None},
        {"asset_path": "/Game/NoObject"},
        {"asset_path": "/Game/Bad.Bad:Subobject"},
        {"asset_path": "/Game/../Bad.Bad"},
        {"asset_path": "C:/Private/Bad.Bad"},
        {"shape": "Cube"},
        {"actor_path": "/Temp/Actor"},
        {"label": None},
    ]:
        with pytest.raises(ValidationError):
            Operation.model_validate({**valid, **replacement})
