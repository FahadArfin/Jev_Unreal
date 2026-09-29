from unittest.mock import AsyncMock

import pytest
from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.exceptions import ToolError
from pydantic import TypeAdapter, ValidationError

from jev_unreal.blueprints import GraphEdit, register_blueprint_tools
from jev_unreal.domain_workflows import Change, Inspection

GUID = "1" * 32
STATE = {"session_id": "session", "world_path": "/Game/Map.Map", "revision": "revision"}


@pytest.mark.parametrize("operation", ["connect", "disconnect"])
async def test_graph_link_roundtrip_preserves_exact_endpoints(operation):
    edit = {
        "operation": operation,
        "graph_id": GUID,
        "output_node_id": "2" * 32,
        "output_pin_id": "3" * 32,
        "input_node_id": "4" * 32,
        "input_pin_id": "5" * 32,
    }
    server, bridge = FastMCP("graph-test"), AsyncMock()
    register_blueprint_tools(server, bridge)
    await server.call_tool(
        "unreal_blueprint_graph_preview",
        {"target_id": "door", "graph_edit": edit, "expected_state": STATE},
    )
    bridge.call.assert_awaited_once_with(
        "blueprint_graph_preview",
        {"target_id": "door", "graph_edit": edit, "expected_state": STATE},
    )


@pytest.mark.parametrize(
    "change",
    [
        {"function": "ExecuteConsoleCommand"},
        {"x": 100001},
        {"x": "1"},
        {"graph_id": "not-a-guid"},
        {"operation": "execute"},
        {"script": "arbitrary"},
    ],
)
async def test_graph_invalid_edits_never_dispatch(change):
    edit = {
        "operation": "add_math_node",
        "graph_id": GUID,
        "function": "Add_IntInt",
        "x": 100,
        "y": 100,
        **change,
    }
    server, bridge = FastMCP("graph-test"), AsyncMock()
    register_blueprint_tools(server, bridge)
    with pytest.raises(ToolError):
        await server.call_tool(
            "unreal_blueprint_graph_preview",
            {"target_id": "door", "graph_edit": edit, "expected_state": STATE},
        )
    bridge.call.assert_not_awaited()


def test_remove_requires_node_identity():
    with pytest.raises(ValidationError):
        TypeAdapter(GraphEdit).validate_python({"operation": "remove_math_node", "graph_id": GUID})


@pytest.mark.parametrize("association,index", [("global", 0), ("layer", -1), ("blend", 64)])
def test_material_layer_identity_must_be_consistent(association, index):
    with pytest.raises(ValidationError):
        TypeAdapter(Change).validate_python(
            {
                "kind": "material_texture",
                "target_path": "/Game/M.M",
                "parameter": "Surface",
                "value": "/Game/T.T",
                "association": association,
                "index": index,
            }
        )


def test_static_switch_requires_boolean_and_camera_finite_exposure():
    with pytest.raises(ValidationError):
        TypeAdapter(Change).validate_python(
            {
                "kind": "material_static_switch",
                "target_path": "/Game/M.M",
                "parameter": "UseTexture",
                "value": 1,
            }
        )
    with pytest.raises(ValidationError):
        TypeAdapter(Change).validate_python(
            {
                "kind": "camera_render",
                "exposure_mode": "fixed",
                "fixed_ev100": float("nan"),
                "view_mode": "lit",
                "realtime": False,
                "motion_blur": False,
            }
        )


@pytest.mark.parametrize("samples", [0, 2, 10, "5", True, False, 1.0, 5.0, None])
def test_terrain_sampling_is_bounded(samples):
    with pytest.raises(ValidationError):
        TypeAdapter(Inspection).validate_python(
            {
                "kind": "surface",
                "actor_path": "/Game/M.M:PersistentLevel.Prop",
                "surface_paths": ["/Game/M.M:PersistentLevel.Ground"],
                "support_samples": samples,
            }
        )


def test_runtime_widget_inspection_only_takes_identity():
    query = TypeAdapter(Inspection).validate_python(
        {
            "kind": "widgets",
            "target_path": "/Game/W.W",
            "runtime_instance_path": "/Game/UEDPIE_0_M.M:PersistentLevel.Widget",
        }
    )
    assert query.runtime_instance_path.endswith(".Widget")
    with pytest.raises(ValidationError):
        TypeAdapter(Inspection).validate_python({**query.model_dump(), "execute_binding": True})
