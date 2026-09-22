"""Offline bounded mesh recipes, native contract and independent fresh requirements."""

from copy import deepcopy
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest
from pydantic import TypeAdapter, ValidationError

from jev_unreal.errors import JevError
from jev_unreal.layouts import PreviewTracker, verify_readback
from jev_unreal.mesh_state import mesh_checks, mesh_state
from jev_unreal.meshes import MeshRecipe, compile_mesh, preview_mesh
from jev_unreal.verification import SceneSnapshots, verify, verify_fresh
from jev_unreal.workflows import Operation

PROJECT = "C:/PublicSmoke/Mesh.uproject"
A, B = "/Temp/Map.Map:PersistentLevel.Source", "/Temp/Map.Map:PersistentLevel.Copy"
MESH = "/Engine/BasicShapes/Cube.Cube"
OTHER = "/Engine/BasicShapes/Sphere.Sphere"
MATERIAL = "/Game/Materials/Red.Red"


def settings():
    return {
        "mobility": "Movable",
        "collision_mode": 3,
        "collision_profile": "BlockAll",
        "use_mesh_default_collision": True,
        "collision_object_type": 0,
        "collision_responses": [2] * 32,
        "actor_collision_enabled": True,
        "cast_shadow": False,
        "receives_decals": True,
        "render_custom_depth": False,
        "custom_depth_stencil_value": 0,
        "translucency_sort_priority": 0,
        "visible": True,
        "hidden_in_game": True,
        "actor_hidden_in_game": False,
        "actor_hidden_in_editor": False,
        "actor_tags": ["Source"],
        "component_tags": ["Common"],
        "tags_truncated": False,
    }


def actor(path=A, instance="source-instance"):
    return {
        "path": path,
        "instance_id": instance,
        "class": "/Script/Engine.StaticMeshActor",
        "label": "Source",
        "folder": "Meshes/Review",
        "location": [10, 20, 30],
        "rotation": [5, 17, -3],
        "scale": [2, 1, 0.5],
        "static_mesh_path": MESH,
        "collision_enabled": True,
        "mesh_settings": settings(),
        "material_slot_count": 1,
        "material_override_count": 1,
        "materials": [{"slot": 0, "path": MATERIAL, "override_path": MATERIAL}],
        "materials_truncated": False,
        "bounds_available": False,
        "bounds_cm": None,
        "editable": True,
        "edit_blockers": [],
    }


def details(actors=None):
    return {
        "project_file": PROJECT,
        "session_id": "session",
        "world_path": "/Temp/Map.Map",
        "current_level": "/Temp/Map.Map:PersistentLevel",
        "revision": "revision1",
        "truncated": False,
        "capabilities": ["replace_mesh", "duplicate_mesh"],
        "actors": [actor()] if actors is None else actors,
    }


def recipe(kind="replace", **kwargs):
    result = {"kind": kind, "actor_paths": [A]}
    if kind == "replace":
        result.update(asset_path=OTHER, material_policy="preserve_slots")
    else:
        result.update(offset_cm=[100, -25, 5], label_suffix="_Copy")
    result.update(kwargs)
    return result


def normalized(kind="replace", source=None, policy="preserve_slots"):
    source = source or actor()
    state = mesh_state(source, actor=True).model_dump(mode="json")
    result = {
        "op": kind + "_mesh",
        "actor_path": source["path"],
        "source_instance_id": source["instance_id"],
        **state,
        **{key: source[key] for key in ("location", "rotation", "scale")},
    }
    if kind == "replace":
        result.update(asset_path=OTHER, material_policy=policy)
        if policy == "mesh_defaults":
            result["material_override_count"] = 0
            result["materials"] = [{"slot": 0, "path": None, "override_path": None}]
    else:
        result.update(source_actor_path=source["path"], label=source["label"] + "_Copy")
        result["location"] = [110, -5, 35]
    return result


def after(operation):
    result = actor()
    for key in (
        "label",
        "folder",
        "materials",
        "material_slot_count",
        "material_override_count",
        "mesh_settings",
        "location",
        "rotation",
        "scale",
    ):
        result[key] = deepcopy(operation[key])
    result["static_mesh_path"] = operation["asset_path"]
    if operation["op"] == "duplicate_mesh":
        result.update(path=B, instance_id="new-instance")
    return result


def connection(*results):
    return SimpleNamespace(
        settings=SimpleNamespace(expected_project=PROJECT),
        call=AsyncMock(side_effect=list(results)),
    )


@pytest.mark.parametrize("kind", ["replace", "duplicate"])
def test_operation_schema_requires_declared_fields_and_no_unrelated_edits(kind):
    value = {"op": kind + "_mesh", "actor_path": A}
    if kind == "replace":
        value.update(asset_path=OTHER, material_policy="mesh_defaults")
    else:
        value["label"] = "Copy"
    assert Operation.model_validate(value).op == kind + "_mesh"
    for invalid in ({**value, "folder": "Moved"}, {**value, "python": "anything"}):
        with pytest.raises(ValidationError):
            Operation.model_validate(invalid)
    del value["material_policy" if kind == "replace" else "label"]
    with pytest.raises(ValidationError):
        Operation.model_validate(value)


@pytest.mark.parametrize(
    "change",
    [
        {"actor_paths": []},
        {"actor_paths": [A] * 2},
        {"actor_paths": [A + str(i) for i in range(21)]},
        {"actor_paths": ["/Game/*"]},
        {"actor_paths": ["/Temp/Map.Map:Bad\nPath"]},
        {"actor_paths": ["/Temp/../Bad.Bad"]},
        {"asset_path": "C:/Local/Cube"},
        {"material_policy": "guess"},
        {"location": [1, 2, 3]},
    ],
    ids=[
        "empty",
        "duplicate",
        "too-many",
        "wildcard",
        "control",
        "traversal",
        "asset",
        "policy",
        "extra",
    ],
)
def test_recipe_input_bounds(change):
    with pytest.raises(ValidationError):
        TypeAdapter(MeshRecipe).validate_python(recipe(**change))


@pytest.mark.parametrize(
    "change",
    [
        {"offset_cm": [True, 0, 0]},
        {"offset_cm": [float("nan"), 0, 0]},
        {"offset_cm": [1e6 + 1, 0, 0]},
        {"offset_cm": [1, 2]},
        {"label_prefix": "\n"},
        {"label_suffix": "\U0001f600" * 21},
        {"label_prefix": "", "label_suffix": ""},
        {"label_suffix": "x" * 41},
    ],
    ids=["bool", "nan", "large", "dimension", "control", "utf16", "empty-label", "long-label"],
)
def test_duplicate_recipe_strict_bounds(change):
    with pytest.raises(ValidationError):
        TypeAdapter(MeshRecipe).validate_python(recipe("duplicate", **change))


@pytest.mark.parametrize("kind", ["replace", "duplicate"])
def test_recipes_measure_exact_selection_without_changing_source(kind):
    source = details()
    untouched = deepcopy(source)
    result = compile_mesh(recipe(kind), source)
    assert source == untouched
    assert result["measurement_state"]["revision"] == "revision1"
    assert result["measurements"][0]["source_instance_id"] == "source-instance"
    assert result["applied"] is False and result["cloud_used"] is False
    if kind == "replace":
        assert result["operations"] == [
            {
                "op": "replace_mesh",
                "actor_path": A,
                "asset_path": OTHER,
                "material_policy": "preserve_slots",
            }
        ]
    else:
        op = result["operations"][0]
        assert op["location"] == [110, -5, 35]
        assert op["rotation"] == actor()["rotation"] and op["scale"] == actor()["scale"]
        assert op["label"] == "Source_Copy"


@pytest.mark.parametrize(
    "change",
    [
        {"editable": False},
        {"edit_blockers": ["physics"]},
        {"class": "/Script/Game.CustomActor"},
        {"material_override_count": None},
        {"mesh_settings": None},
        {"materials_truncated": True},
        {"location": [1e6, 0, 0]},
        {"label": "X" * 80},
    ],
    ids=[
        "locked",
        "blocker",
        "subclass",
        "missing-count",
        "missing-settings",
        "truncated",
        "overflow",
        "label-overflow",
    ],
)
def test_duplicate_refuses_unsupported_or_out_of_range_source(change):
    value = actor()
    value.update(change)
    with pytest.raises(JevError):
        compile_mesh(recipe("duplicate"), details([value]))


@pytest.mark.parametrize("capabilities", [None, [], ["preview"], "replace_mesh", {}])
def test_legacy_editor_gets_capability_error_before_new_fields_are_required(capabilities):
    with pytest.raises(JevError) as exc:
        compile_mesh(recipe(), {"capabilities": capabilities})
    assert exc.value.code == "capability_unavailable"


@pytest.mark.parametrize("kind", ["replace", "duplicate"])
async def test_preview_binds_fresh_measurements_and_apply_derives_fresh_checks(kind):
    operation = normalized(kind)
    output = after(operation)
    bridge = connection(
        details(),
        {"plan_id": "p", "operations": [operation], "expires_in_seconds": 120},
        {"applied": True, "actors": [output], "revision": "revision2"},
    )
    previews = PreviewTracker(bridge)
    proposal = await preview_mesh(bridge, previews, recipe(kind))
    assert bridge.call.await_args_list[0].args == ("actor_details", {"actor_paths": [A]})
    assert bridge.call.await_args_list[1].args[1]["expected_state"] == {
        "session_id": "session",
        "world_path": "/Temp/Map.Map",
        "revision": "revision1",
    }
    assert bridge.call.await_count == 2  # Preview never applies.
    assert len(proposal["verification_checks"]) == (2 if kind == "replace" else 0)
    applied = await previews.apply("p")
    assert applied["verification"]["status"] == "passed"
    checks = applied["verification_checks"]
    assert checks[0]["actor_path"] == output["path"]
    assert checks[0]["expected_instance_id"] == output["instance_id"]
    fresh = connection(details([output]))
    assert (await verify_fresh(fresh, checks))["status"] == "passed"
    fresh.call.assert_awaited_once_with("actor_details", {"actor_paths": [output["path"]]})
    assert previews.journal.get("p")["status"] == "applied"
    with pytest.raises(JevError, match="already attempted"):
        await previews.apply("p")
    assert bridge.call.await_count == 3


async def test_preview_mesh_defaults_accepts_native_effective_slots_and_clears_overrides():
    operation = normalized(policy="mesh_defaults")
    operation["materials"].append({"slot": 1, "path": MATERIAL, "override_path": None})
    operation["material_slot_count"] = 2
    bridge = connection(details(), {"plan_id": "p", "operations": [operation]})
    result = await preview_mesh(
        bridge, PreviewTracker(bridge), recipe(material_policy="mesh_defaults")
    )
    assert result["verification_checks"][0]["expected"]["material_slot_count"] == 2


@pytest.mark.parametrize("code", ["stale_plan", "wrong_project", "actor_unsupported"])
async def test_mesh_preview_propagates_native_preflight_failures_without_retry(code):
    bridge = connection(details(), JevError(code, "Rejected"))
    with pytest.raises(JevError) as exc:
        await preview_mesh(bridge, PreviewTracker(bridge), recipe())
    assert exc.value.code == code
    assert bridge.call.await_count == 2


async def test_wrong_project_details_never_preview():
    value = details()
    value["project_file"] = "C:/Another/Other.uproject"
    bridge = connection(value)
    with pytest.raises(JevError) as exc:
        await preview_mesh(bridge, PreviewTracker(bridge), recipe())
    assert exc.value.code == "wrong_project"
    assert bridge.call.await_count == 1


@pytest.mark.parametrize(
    "field,value",
    [
        ("source_instance_id", "replaced"),
        ("actor_path", B),
        ("op", "set_metadata"),
        ("asset_path", MESH),
        ("material_policy", "mesh_defaults"),
        ("label", "Changed"),
        ("folder", "Other"),
        ("location", [999, 0, 0]),
        ("material_slot_count", 0),
    ],
    ids=["instance", "path", "op", "mesh", "policy", "label", "folder", "transform", "slots"],
)
async def test_preview_rejects_normalized_state_disagreeing_with_fresh_inspection(field, value):
    operation = normalized()
    operation[field] = value
    bridge = connection(details(), {"plan_id": "p", "operations": [operation]})
    with pytest.raises(JevError) as exc:
        await preview_mesh(bridge, PreviewTracker(bridge), recipe())
    assert exc.value.code == "invalid_mesh_preview"
    assert bridge.call.await_count == 2


@pytest.mark.parametrize("kind", ["replace", "duplicate"])
def test_native_mesh_readback_requires_all_declared_fields(kind):
    operation = normalized(kind)
    output = after(operation)
    assert verify_readback([operation], [output])["status"] == "passed"
    for field in (
        "static_mesh_path",
        "mesh_settings",
        "materials",
        "material_slot_count",
        "material_override_count",
        "folder",
        "label",
        "instance_id",
        "materials_truncated",
        "location",
        "rotation",
        "scale",
    ):
        changed = deepcopy(output)
        del changed[field]
        assert verify_readback([operation], [changed])["status"] == "mismatch", field
    for field in ("asset_path", "source_instance_id", "materials", "mesh_settings", "folder"):
        changed = deepcopy(operation)
        del changed[field]
        assert verify_readback([changed], [output])["status"] == "mismatch", field


@pytest.mark.parametrize(
    "field,value",
    [
        ("path", A),
        ("path", []),
        ("instance_id", "source-instance"),
        ("instance_id", ""),
        ("instance_id", []),
        ("static_mesh_path", OTHER),
        ("folder", "Wrong"),
        ("label", "Wrong"),
        ("location", [0, 0, 0]),
        ("scale", [1, 1, 1]),
    ],
    ids=[
        "source-path",
        "bad-path",
        "source-instance",
        "empty-id",
        "bad-id",
        "mesh",
        "folder",
        "label",
        "location",
        "scale",
    ],
)
def test_duplicate_readback_rejects_source_identity_or_changed_state(field, value):
    operation = normalized("duplicate")
    output = after(operation)
    output[field] = value
    assert verify_readback([operation], [output])["status"] == "mismatch"


def test_duplicate_readback_rejects_other_sources_and_duplicate_result_instances():
    first = normalized("duplicate")
    second = normalized("duplicate", actor(B, "source2"))
    outputs = [after(first), after(second)]
    outputs[0]["path"] = B  # Another source is not a new copy.
    outputs[1].update(path=B + "2", instance_id="copy2")
    assert verify_readback([first, second], outputs)["status"] == "mismatch"
    outputs[0]["path"] = B + "1"
    outputs[1]["instance_id"] = outputs[0]["instance_id"]
    assert verify_readback([first, second], outputs)["status"] == "mismatch"


@pytest.mark.parametrize(
    "change",
    [
        {"mesh_settings": {**settings(), "cast_shadow": True}},
        {"mesh_settings": {**settings(), "collision_responses": [1] * 32}},
        {"mesh_settings": {**settings(), "actor_tags": ["Changed"]}},
        {"mesh_settings": {**settings(), "visible": 1}},
        {"materials": [{"slot": 0, "path": MATERIAL}]},
        {"materials": [{"slot": 0, "path": MATERIAL, "override_path": None}]},
        {"material_override_count": 0},
        {"material_slot_count": True},
    ],
    ids=[
        "shadow",
        "responses",
        "tags",
        "bool",
        "missing-override",
        "override",
        "count",
        "bool-count",
    ],
)
def test_new_readback_never_passes_changed_or_incomplete_mesh_settings(change):
    operation = normalized()
    output = after(operation)
    output.update(change)
    assert verify_readback([operation], [output])["status"] == "mismatch"


@pytest.mark.parametrize(
    "field", ["mesh_settings", "material_override_count", "material_slot_count"]
)
def test_fresh_mesh_check_reports_missing_legacy_fields_as_unverifiable(field):
    operation = normalized()
    output = after(operation)
    del output[field]
    result = verify(mesh_checks(operation, A, "source-instance"), details([output]))
    assert result["status"] == "unverifiable"
    assert result["checks"][0]["reason"] == "mesh_state_unavailable"
    assert result["checks"][1]["status"] == "passed"


def test_fresh_mesh_check_missing_null_override_is_not_assumed_null():
    operation = normalized(policy="mesh_defaults")
    output = after(operation)
    del output["materials"][0]["override_path"]
    result = verify(mesh_checks(operation, A, "source-instance"), details([output]))
    assert result["status"] == "unverifiable"
    assert result["checks"][0]["reason"] == "mesh_state_unavailable"


def test_fresh_mesh_check_distinguishes_failed_replaced_and_truncated_state():
    operation = normalized()
    checks = mesh_checks(operation, A, "source-instance")
    output = after(operation)
    output["mesh_settings"]["visible"] = False
    assert verify(checks, details([output]))["status"] == "failed"
    output["instance_id"] = "replacement-instance"
    assert verify(checks, details([output]))["checks"][0]["reason"] == "actor_replaced"
    output = after(operation)
    output["mesh_settings"]["tags_truncated"] = True
    assert verify(checks, details([output]))["status"] == "unverifiable"


async def test_snapshot_retains_mesh_settings_and_override_changes_even_when_effective_same():
    source = actor()
    bridge = connection(details([source]))
    snapshots = SceneSnapshots(bridge)
    baseline = await snapshots.capture([A])
    changed = deepcopy(source)
    changed["materials"][0]["override_path"] = None
    changed["material_override_count"] = 0
    changed["mesh_settings"]["cast_shadow"] = True
    result = snapshots.compare(baseline["snapshot_id"], details([changed]))
    assert {item["field"] for item in result["changed"]} == {
        "material_overrides",
        "material_override_count",
        "mesh_settings",
    }


async def test_bad_apply_readback_never_returns_fresh_requirements_or_retries():
    operation = normalized("duplicate")
    output = after(operation)
    output["instance_id"] = "source-instance"
    bridge = connection(
        {"plan_id": "p", "operations": [operation]},
        {
            "applied": True,
            "actors": [output],
            "verification_checks": [{"kind": "label", "actor_path": A, "expected": "Anything"}],
            "verification_checks_note": "unreviewed native supplied requirements",
        },
    )
    tracker = PreviewTracker(bridge)
    await tracker.preview([{"op": "duplicate_mesh", "actor_path": A, "label": "Copy"}])
    result = await tracker.apply("p")
    assert result["verification"]["status"] == "mismatch"
    assert "verification_checks" not in result
    assert "verification_checks_note" not in result
    assert tracker.journal.get("p")["status"] == "applied"
    assert bridge.call.await_count == 2


@pytest.mark.parametrize("label", [" Source ", "Source ", " Source"])
def test_mesh_evidence_keeps_literal_label_whitespace(label):
    operation = normalized()
    output = after(operation)
    output["label"] = label
    assert mesh_state(output, actor=True).label == label
    assert verify_readback([operation], [output])["status"] == "mismatch"
    assert (
        verify(mesh_checks(operation, A, "source-instance"), details([output]))["status"]
        == "failed"
    )


@pytest.mark.parametrize(
    "field,value",
    [
        ("instance_id", "new\nidentity"),
        ("instance_id", "new\x7fidentity"),
        ("instance_id", "new\ud800identity"),
        ("path", B + "\nOther"),
        ("path", B + "\ud800"),
        ("path", "not-an-actor-path"),
        ("path", "/Temp/*"),
        ("class", "/Script/Game.DerivedMeshActor"),
    ],
    ids=[
        "id-newline",
        "id-del",
        "id-unicode",
        "path-newline",
        "path-unicode",
        "path",
        "wildcard",
        "subclass",
    ],
)
def test_malformed_duplicate_identity_cannot_pass_or_generate_invalid_fresh_requirements(
    field, value
):
    operation = normalized("duplicate")
    output = after(operation)
    output[field] = value
    assert verify_readback([operation], [output])["status"] == "mismatch"


@pytest.mark.parametrize(
    "field,value",
    [
        ("location", [1e308, 0, 0]),
        ("rotation", [1e308, 0, 0]),
        ("scale", [0, 1, 1]),
        ("scale", [-1, 1, 1]),
    ],
    ids=["huge-location", "huge-rotation", "zero-scale", "negative-scale"],
)
def test_matching_unbounded_expected_and_actual_mesh_transform_never_passes(field, value):
    operation = normalized()
    operation[field] = value
    assert verify_readback([operation], [after(operation)])["status"] == "mismatch"


def test_mesh_fresh_check_schema_rejects_undeclared_fields_and_bad_settings():
    from jev_unreal.verification import Check

    check = mesh_checks(normalized(), A, "source-instance")[0]
    adapter = TypeAdapter(Check)
    for key, value in (
        ("collision_responses", [0] * 31),
        ("collision_mode", True),
        ("use_mesh_default_collision", 1),
        ("use_mesh_default_collision", "false"),
        ("actor_tags", ["A"] * 33),
        ("component_tags", ["A" * 129]),
        ("tags_truncated", True),
        ("arbitrary_component", "anything"),
    ):
        invalid = deepcopy(check)
        invalid["expected"]["mesh_settings"][key] = value
        with pytest.raises(ValidationError):
            adapter.validate_python(invalid)


async def test_twenty_sources_generate_forty_bounded_post_apply_requirements():
    sources = [actor(A + str(index), f"source-{index}") for index in range(20)]
    operations = [normalized("duplicate", source) for source in sources]
    outputs = []
    for index, operation in enumerate(operations):
        output = after(operation)
        output.update(path=B + str(index), instance_id=f"created-{index}")
        outputs.append(output)
    bridge = connection(
        details(sources),
        {"plan_id": "p", "operations": operations},
        {"applied": True, "actors": outputs},
    )
    tracker = PreviewTracker(bridge)
    proposal = await preview_mesh(
        bridge, tracker, recipe("duplicate", actor_paths=[item["path"] for item in sources])
    )
    assert proposal["operation_count"] == 20
    result = await tracker.apply("p")
    assert len(result["verification_checks"]) == 40
    assert verify(result["verification_checks"], details(outputs))["status"] == "passed"


@pytest.mark.parametrize("kind", ["replace", "duplicate"])
@pytest.mark.parametrize("inherits_mesh_collision", [True, False])
def test_collision_inheritance_is_required_exact_evidence(kind, inherits_mesh_collision):
    source = actor()
    source["mesh_settings"]["use_mesh_default_collision"] = inherits_mesh_collision
    operation = normalized(kind, source)
    output = after(operation)
    checks = mesh_checks(operation, output["path"], output["instance_id"])
    assert (
        checks[0]["expected"]["mesh_settings"]["use_mesh_default_collision"]
        is inherits_mesh_collision
    )
    assert verify_readback([operation], [output])["status"] == "passed"
    assert verify(checks, details([output]))["status"] == "passed"

    # Identical resolved responses/profile cannot hide a changed inheritance rule.
    output["mesh_settings"]["use_mesh_default_collision"] = not inherits_mesh_collision
    assert verify_readback([operation], [output])["status"] == "mismatch"
    assert verify(checks, details([output]))["status"] == "failed"

    del output["mesh_settings"]["use_mesh_default_collision"]
    assert verify_readback([operation], [output])["status"] == "mismatch"
    assert verify(checks, details([output]))["status"] == "unverifiable"
    incomplete_operation = deepcopy(operation)
    del incomplete_operation["mesh_settings"]["use_mesh_default_collision"]
    assert verify_readback([incomplete_operation], [after(operation)])["status"] == "mismatch"


async def test_snapshot_diff_reports_collision_inheritance_only_change():
    source = actor()
    snapshots = SceneSnapshots(connection(details([source])))
    baseline = await snapshots.capture([A])
    changed = deepcopy(source)
    changed["mesh_settings"]["use_mesh_default_collision"] = False
    result = snapshots.compare(baseline["snapshot_id"], details([changed]))
    assert result["status"] == "changed"
    assert len(result["changed"]) == 1
    change = result["changed"][0]
    assert change["field"] == "mesh_settings"
    assert change["before"]["use_mesh_default_collision"] is True
    assert change["after"]["use_mesh_default_collision"] is False
