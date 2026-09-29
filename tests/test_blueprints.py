"""Blueprint MCP bindings keep inspection, review and trusted compilation explicit."""

from unittest.mock import AsyncMock

import pytest
from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.exceptions import ToolError

from jev_unreal.blueprints import register_blueprint_tools
from jev_unreal.errors import JevError

STATE = {"session_id": "session", "world_path": "/Game/Map.Map", "revision": "revision"}


async def test_compile_annotations_separate_discovery_preview_and_execution():
    server = FastMCP("blueprint-test")
    register_blueprint_tools(server, AsyncMock())
    tools = {tool.name: tool for tool in await server.list_tools()}
    assert len(tools) == 6
    assert tools["unreal_blueprint_compile_targets"].annotations.readOnlyHint is True
    assert tools["unreal_blueprint_compile_receipt"].annotations.readOnlyHint is True
    assert tools["unreal_blueprint_compile_preview"].annotations.destructiveHint is False
    assert tools["unreal_blueprint_compile_preview"].annotations.idempotentHint is False
    assert tools["unreal_blueprint_compile"].annotations.destructiveHint is True
    assert tools["unreal_blueprint_compile"].annotations.idempotentHint is False
    assert "expected_state" in tools["unreal_blueprint_compile_preview"].inputSchema["required"]
    assert "expected_project" not in tools["unreal_blueprint_compile"].inputSchema["properties"]


@pytest.mark.parametrize(
    "tool,arguments,action,params",
    [
        ("unreal_blueprint_compile_targets", {}, "blueprint_compile_targets", None),
        (
            "unreal_blueprint_compile_preview",
            {"target_id": "door", "expected_state": STATE},
            "blueprint_compile_preview",
            {"target_id": "door", "expected_state": STATE},
        ),
        (
            "unreal_blueprint_compile",
            {"plan_id": "reviewed-plan"},
            "blueprint_compile",
            {"plan_id": "reviewed-plan"},
        ),
        (
            "unreal_blueprint_compile_receipt",
            {"plan_id": "reviewed-plan"},
            "blueprint_compile_receipt",
            {"plan_id": "reviewed-plan"},
        ),
    ],
)
async def test_bindings_preserve_exact_user_intent(tool, arguments, action, params):
    bridge = AsyncMock()
    bridge.call.return_value = {"status": "pending"}
    server = FastMCP("blueprint-test")
    register_blueprint_tools(server, bridge)
    await server.call_tool(tool, arguments)
    bridge.call.assert_awaited_once_with(action, params)


@pytest.mark.parametrize(
    "arguments",
    [
        {"target_id": "", "expected_state": STATE},
        {"target_id": "a" * 65, "expected_state": STATE},
        {"target_id": "/Game/Arbitrary.Asset", "expected_state": STATE},
        {"target_id": "approved\nexecute", "expected_state": STATE},
        {"target_id": "approved"},
        {"target_id": "approved", "expected_state": {**STATE, "revision": ""}},
    ],
)
async def test_invalid_compile_previews_do_not_reach_bridge(arguments):
    bridge = AsyncMock()
    server = FastMCP("blueprint-test")
    register_blueprint_tools(server, bridge)
    with pytest.raises(ToolError):
        await server.call_tool("unreal_blueprint_compile_preview", arguments)
    bridge.call.assert_not_called()


async def test_uncertain_transport_error_is_returned_without_retry():
    bridge = AsyncMock()
    bridge.call.side_effect = JevError("bridge_timeout", "Compilation may still be running.")
    server = FastMCP("blueprint-test")
    register_blueprint_tools(server, bridge)
    result = await server.call_tool("unreal_blueprint_compile", {"plan_id": "reviewed"})
    assert "bridge_timeout" in str(result)
    bridge.call.assert_awaited_once()


async def test_gameplay_graph_vocabulary_is_bounded_and_preserved():
    from pydantic import TypeAdapter, ValidationError

    from jev_unreal.blueprints import GraphEdit

    adapter = TypeAdapter(GraphEdit)
    base = {"graph_id": "12345678-1234-1234-1234-123456789012", "x": 0, "y": 0}
    for operation in [
        {"operation": "add_event", "event": "ReceiveActorBeginOverlap"},
        {"operation": "add_branch"},
        {"operation": "add_variable_get", "name": "CanOpen"},
        {"operation": "add_variable_set", "name": "CanOpen"},
        {"operation": "add_actor_call", "function": "K2_SetActorRelativeLocation",
         "location": [0, 0, 300]},
    ]:
        edit = adapter.validate_python({**base, **operation})
        assert edit.operation == operation["operation"]
    for operation in [
        {"operation": "add_event", "event": "ArbitraryCallback"},
        {"operation": "add_actor_call", "function": "ExecuteConsoleCommand"},
        {"operation": "add_variable_get", "name": "/Game/External"},
        {"operation": "add_actor_call", "function": "K2_SetActorRelativeLocation",
         "location": [0, 0, float("inf")]},
    ]:
        with pytest.raises(ValidationError):
            adapter.validate_python({**base, **operation})
    bridge = AsyncMock()
    server = FastMCP("blueprint-test")
    register_blueprint_tools(server, bridge)
    edit = {**base, "operation": "add_actor_call", "function": "SetActorEnableCollision"}
    await server.call_tool("unreal_blueprint_graph_preview", {
        "target_id": "door", "graph_edit": edit, "expected_state": STATE,
    })
    bridge.call.assert_awaited_once_with("blueprint_graph_preview", {
        "target_id": "door", "graph_edit": edit, "expected_state": STATE,
    })
