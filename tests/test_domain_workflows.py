"""Domain workflow contracts and independent measurement checks; no engine/provider claims."""

from copy import deepcopy
from unittest.mock import AsyncMock

import httpx
import pytest
from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.exceptions import ToolError
from pydantic import TypeAdapter, ValidationError

from jev_unreal.bridge import UnrealBridge
from jev_unreal.config import Settings
from jev_unreal.domain_workflows import (
    Change,
    Inspection,
    compare_performance,
    register_domain_tools,
)
from jev_unreal.errors import JevError

STATE = {"session_id": "session", "world_path": "/Game/Map.Map", "revision": "revision"}
PROJECT = "C:/Public/Project.uproject"
ASSET = "/Game/Materials/Instance.Instance"
ACTOR = "/Game/Map.Map:PersistentLevel.Prop"
MUTATIONS = [
    "blueprint_pin_preview",
    "workflow_preview",
    "workflow_apply",
    "performance_start",
    "performance_cancel",
]


def capture(identifier="a", factor=1):
    return {
        "job_id": identifier,
        "project_file": PROJECT,
        **STATE,
        "protocol_id": "same-workload",
        "metric": "editor_game_thread_ticker_wall_interval_ms",
        "requested_samples": 100,
        "status": "completed",
        "editor_tick_interval": {
            "count": 100,
            "mean_ms": 10 * factor,
            "p50_ms": 9 * factor,
            "p95_ms": 15 * factor,
        },
    }


def server():
    bridge, previews = AsyncMock(), AsyncMock()
    app = FastMCP("domain-tests")
    register_domain_tools(app, bridge, previews)
    return app, bridge, previews


@pytest.mark.parametrize("action", MUTATIONS)
async def test_new_mutations_bind_actual_project_without_refreshing_review(action):
    bridge = UnrealBridge(Settings(expected_project=PROJECT, bridge_token="t" * 40))
    bridge._call = AsyncMock(side_effect=[{"project_file": PROJECT, "capabilities": [action]}, {}])
    try:
        await bridge.call(action, {"expected_project": "forged", "expected_state": STATE})
        assert bridge._call.await_args.args == (
            action,
            {"expected_project": PROJECT, "expected_state": STATE},
        )
    finally:
        await bridge.close()


@pytest.mark.parametrize("action", MUTATIONS)
@pytest.mark.parametrize(
    "expected,capabilities,code",
    [
        ("", [], "project_required"),
        (PROJECT, [], "capability_unavailable"),
        ("wrong", MUTATIONS, "wrong_project"),
    ],
)
async def test_mutations_fail_before_dispatch(action, expected, capabilities, code):
    bridge = UnrealBridge(Settings(expected_project=expected, bridge_token="t" * 40))
    bridge._call = AsyncMock(return_value={"project_file": PROJECT, "capabilities": capabilities})
    try:
        with pytest.raises(JevError) as error:
            await bridge.call(action, {})
        assert error.value.code == code
        assert bridge._call.await_count == 1
    finally:
        await bridge.close()


@pytest.mark.parametrize("action", ["workflow_inspect", "workflow_receipt", "performance_job"])
async def test_new_reads_require_capability(action):
    bridge = UnrealBridge(
        Settings(bridge_token="t" * 40),
        httpx.MockTransport(
            lambda _: httpx.Response(
                200, json={"ok": True, "result": {"project_file": PROJECT, "capabilities": []}}
            )
        ),
    )
    try:
        with pytest.raises(JevError, match="capability"):
            await bridge.call(action, {})
    finally:
        await bridge.close()


@pytest.mark.parametrize("value", [float("nan"), float("inf"), -1_000_001, 1_000_001, True, "0.5"])
def test_scalar_values_are_bounded_and_typed(value):
    with pytest.raises(ValidationError):
        TypeAdapter(Change).validate_python(
            {
                "kind": "material_scalar",
                "target_path": ASSET,
                "parameter": "Roughness",
                "value": value,
            }
        )


@pytest.mark.parametrize(
    "query",
    [
        {"kind": "python", "code": "print(1)"},
        {"kind": "camera", "execute": "anything"},
        {"kind": "asset_diagnosis", "target_path": ASSET, "dependency_depth": 4},
        {"kind": "rig", "target_path": ASSET, "required_bones": ["root"] * 65},
        {"kind": "surface", "actor_path": ACTOR, "surface_paths": []},
        {
            "kind": "surface",
            "actor_path": ACTOR,
            "surface_paths": [ACTOR],
            "trace_channel": "arbitrary",
        },
        {"kind": "surface", "actor_path": ACTOR, "surface_paths": [ACTOR], "max_slope_degrees": 61},
        {"kind": "widgets", "target_path": "bad\npath"},
    ],
)
def test_inspections_reject_unbounded_or_executable_inputs(query):
    with pytest.raises(ValidationError):
        TypeAdapter(Inspection).validate_python(query)


async def test_discovery_annotations_and_discriminators():
    app, _, _ = server()
    tools = {tool.name: tool for tool in await app.list_tools()}
    assert len(tools) == 9
    assert tools["unreal_workflow_inspect"].annotations.readOnlyHint
    assert tools["unreal_workflow_apply"].annotations.destructiveHint
    assert not tools["unreal_workflow_apply"].annotations.idempotentHint
    assert not tools["unreal_surface_preview"].annotations.readOnlyHint
    kinds = tools["unreal_workflow_inspect"].inputSchema["properties"]["query"]["discriminator"][
        "mapping"
    ]
    assert set(kinds) == {
        "material",
        "light",
        "camera",
        "asset_diagnosis",
        "rig",
        "widgets",
        "surface",
        "navigation",
    }


async def test_preview_preserves_reviewed_state_and_change():
    app, bridge, _ = server()
    bridge.call.return_value = {"plan_id": "one"}
    change = {
        "kind": "material_scalar",
        "target_path": ASSET,
        "parameter": "Roughness",
        "value": 0.5,
    }
    await app.call_tool("unreal_workflow_preview", {"change": change, "expected_state": STATE})
    bridge.call.assert_awaited_once_with(
        "workflow_preview", {"change": change, "expected_state": STATE}
    )


async def test_invalid_schema_never_calls_native_bridge():
    app, bridge, _ = server()
    with pytest.raises(ToolError):
        await app.call_tool(
            "unreal_workflow_preview",
            {
                "change": {
                    "kind": "camera",
                    "location": [0, 0, 0],
                    "rotation": [0, 0, 0],
                    "fov_degrees": 180,
                },
                "expected_state": STATE,
            },
        )
    bridge.call.assert_not_awaited()


async def test_ambiguous_apply_never_retries():
    app, bridge, _ = server()
    bridge.call.side_effect = JevError("editor_unavailable", "Uncertain response")
    result = await app.call_tool("unreal_workflow_apply", {"plan_id": "reviewed"})
    assert "editor_unavailable" in str(result)
    bridge.call.assert_awaited_once_with("workflow_apply", {"plan_id": "reviewed"})


@pytest.mark.parametrize("valid,actor", [(False, ACTOR), (True, "wrong")])
async def test_surface_refuses_failed_or_wrong_actor_evidence(valid, actor):
    app, bridge, previews = server()
    bridge.call.return_value = {"placement_valid": valid, "actor_path": actor}
    result = await app.call_tool(
        "unreal_surface_preview",
        {"query": {"kind": "surface", "actor_path": ACTOR, "surface_paths": [ACTOR + "Floor"]}},
    )
    assert "placement_refused" in str(result)
    previews.preview.assert_not_awaited()


async def test_surface_preview_uses_measured_state_and_transform():
    app, bridge, previews = server()
    bridge.call.return_value = {
        "placement_valid": True,
        "actor_path": ACTOR,
        **STATE,
        "location": [0, 0, 51],
        "rotation": [0, 0, 0],
        "scale": [1, 1, 1],
    }
    previews.preview.return_value = {"plan_id": "terrain-plan"}
    result = await app.call_tool(
        "unreal_surface_preview",
        {"query": {"kind": "surface", "actor_path": ACTOR, "surface_paths": [ACTOR + "Floor"]}},
    )
    assert "terrain-plan" in str(result)
    assert previews.preview.await_args.kwargs["expected_state"] == STATE
    assert previews.preview.await_args.args[0][0]["location"] == [0, 0, 51]
    assert bridge.call.await_count == 1


def test_comparison_reports_regression_without_significance_claim():
    result = compare_performance(capture(), capture("b", 1.2), 5)
    assert result["regression_flagged"]
    assert result["comparisons"][0]["delta_percent"] == pytest.approx(20)
    assert result["comparisons"][0]["delta_ms"] == 2
    assert result["statistical_significance_established"] is False


@pytest.mark.parametrize(
    "field",
    ["project_file", "session_id", "world_path", "protocol_id", "metric", "requested_samples"],
)
def test_unmatched_protocol_measurements_refused(field):
    candidate = capture("b")
    candidate[field] = "different"
    with pytest.raises(JevError):
        compare_performance(capture(), candidate, 5)


@pytest.mark.parametrize("status", ["running", "cancelled", "state_changed", "timed_out", "failed"])
def test_partial_failure_measurements_never_compare_as_success(status):
    candidate = capture("b")
    candidate["status"] = status
    with pytest.raises(JevError) as error:
        compare_performance(capture(), candidate, 5)
    assert error.value.code == "incomplete_measurement"


@pytest.mark.parametrize("metric", ["mean_ms", "p50_ms", "p95_ms"])
@pytest.mark.parametrize("value", [0, -1, float("nan"), float("inf"), "10", True, None])
def test_invalid_timing_values_rejected(metric, value):
    candidate = capture("b")
    candidate["editor_tick_interval"][metric] = value
    with pytest.raises(JevError) as error:
        compare_performance(capture(), candidate, 5)
    assert error.value.code == "invalid_measurement"


async def test_compare_fetches_distinct_native_receipts_serially():
    app, bridge, _ = server()
    bridge.call.side_effect = [capture(), capture("b", 0.9)]
    result = await app.call_tool(
        "unreal_performance_compare", {"baseline_job_id": "a", "candidate_job_id": "b"}
    )
    assert "regression_flagged" in str(result)
    assert [call.args for call in bridge.call.await_args_list] == [
        ("performance_job", {"job_id": "a"}),
        ("performance_job", {"job_id": "b"}),
    ]


def test_identical_capture_rejected_and_inputs_not_mutated():
    original = capture()
    copy = deepcopy(original)
    with pytest.raises(JevError):
        compare_performance(original, original, 5)
    assert original == copy
