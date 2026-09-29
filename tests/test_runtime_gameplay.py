"""Lifecycle bindings preserve review, ownership and bounded capture inputs."""

from unittest.mock import AsyncMock

import pytest
from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.exceptions import ToolError

from jev_unreal.errors import JevError
from jev_unreal.runtime_gameplay import register_runtime_gameplay_tools

STATE = {"session_id": "session", "world_path": "/Game/Map.Map", "revision": "revision"}
ID = "12345678-1234-1234-1234-123456789012"


async def test_lifecycle_review_ownership_receipts_and_no_retry():
    bridge = AsyncMock()
    bridge.call.return_value = {"status": "pending"}
    server = FastMCP("runtime")
    register_runtime_gameplay_tools(server, bridge)
    tools = {tool.name: tool for tool in await server.list_tools()}
    assert tools["unreal_runtime_status"].annotations.readOnlyHint
    assert not tools["unreal_runtime_preview"].annotations.destructiveHint
    assert tools["unreal_runtime_apply"].annotations.destructiveHint
    assert not tools["unreal_runtime_apply"].annotations.idempotentHint
    for operation, owner in [("start", None), ("stop", ID)]:
        args = {"operation": operation, "expected_state": STATE}
        if owner:
            args["owned_session_id"] = owner
        await server.call_tool("unreal_runtime_preview", args)
        bridge.call.assert_awaited_with("runtime_preview", args)
    bridge.reset_mock()
    for args in [
        {"operation": "start", "expected_state": STATE, "owned_session_id": ID},
        {"operation": "stop", "expected_state": STATE},
    ]:
        result = await server.call_tool("unreal_runtime_preview", args)
        assert "bad_request" in str(result)
    bridge.call.assert_not_called()
    bridge.call.side_effect = JevError("bridge_timeout", "Read the receipt.")
    await server.call_tool("unreal_runtime_apply", {"plan_id": ID})
    bridge.call.assert_awaited_once_with("runtime_apply", {"plan_id": ID})


@pytest.mark.parametrize(
    "tool,args",
    [
        ("unreal_runtime_apply", {"plan_id": "not-an-id"}),
        ("unreal_runtime_preview", {"operation": "console", "expected_state": STATE}),
        ("unreal_runtime_capture", {"owned_session_id": ID, "max_dimension": 1025}),
        ("unreal_runtime_capture", {"owned_session_id": ID, "max_dimension": 63}),
    ],
)
async def test_invalid_runtime_requests_never_reach_native(tool, args):
    bridge = AsyncMock()
    server = FastMCP("runtime")
    register_runtime_gameplay_tools(server, bridge)
    with pytest.raises(ToolError):
        await server.call_tool(tool, args)
    bridge.call.assert_not_called()


async def test_runtime_capture_rejects_untrusted_image_and_preserves_native_denial():
    bridge = AsyncMock()
    server = FastMCP("runtime")
    register_runtime_gameplay_tools(server, bridge)
    bridge.call.return_value = {"mime_type": "image/png", "data": "not-a-png"}
    result = await server.call_tool("unreal_runtime_capture", {"owned_session_id": ID})
    assert "capture_failed" in str(result)
    bridge.call.side_effect = JevError("session_not_owned", "Wrong PIE world.")
    result = await server.call_tool("unreal_runtime_capture", {"owned_session_id": ID})
    assert "session_not_owned" in str(result)
