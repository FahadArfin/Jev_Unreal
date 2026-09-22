"""Project jobs preserve identity gates, capability checks and explicit side effects."""

import json
from unittest.mock import AsyncMock

import httpx
import pytest
from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.exceptions import ToolError

from jev_unreal.bridge import UnrealBridge
from jev_unreal.config import Settings
from jev_unreal.errors import JevError
from jev_unreal.project_tools import register_project_tools

ACTIONS = [
    "pending_plans",
    "plan_status",
    "blueprint_inspect",
    "asset_dependencies",
    "asset_import_info",
    "validation_rules",
    "validation_start",
    "validation_job",
    "validation_cancel",
    "functional_tests",
    "functional_start",
    "functional_job",
    "functional_cancel",
]


@pytest.mark.parametrize("action", ACTIONS)
@pytest.mark.parametrize("wrong_project", [False, True])
async def test_new_native_actions_cannot_bypass_identity_or_capabilities(action, wrong_project):
    seen = []

    def handler(request):
        body = json.loads(request.content)
        seen.append(body["action"])
        return httpx.Response(
            200,
            json={
                "ok": True,
                "result": {
                    "project_file": "/other/Game.uproject"
                    if wrong_project
                    else "/expected/Game.uproject",
                    "capabilities": [],
                },
            },
        )

    bridge = UnrealBridge(
        Settings(bridge_token="x" * 64, expected_project="/expected/Game.uproject"),
        httpx.MockTransport(handler),
    )
    try:
        with pytest.raises(JevError) as failure:
            await bridge.call(action)
        assert failure.value.code == (
            "wrong_project" if wrong_project else "capability_unavailable"
        )
        assert seen == ["status"]
    finally:
        await bridge.close()


@pytest.mark.parametrize(
    "action", ["validation_start", "validation_cancel", "functional_start", "functional_cancel"]
)
async def test_running_project_code_requires_explicit_project(action):
    calls = []

    def handler(request):
        calls.append(json.loads(request.content)["action"])
        return httpx.Response(
            200,
            json={
                "ok": True,
                "result": {
                    "project_file": "/expected/Game.uproject",
                    "capabilities": ACTIONS,
                },
            },
        )

    bridge = UnrealBridge(Settings(bridge_token="x" * 64), httpx.MockTransport(handler))
    try:
        with pytest.raises(JevError, match="JEV_EXPECTED_PROJECT"):
            await bridge.call(action)
        assert calls == ["status"]
    finally:
        await bridge.close()


async def test_selected_validation_binding_is_not_marked_read_only():
    bridge = AsyncMock()
    bridge.call.return_value = {"job_id": "native-id", "state": "queued"}
    server = FastMCP("project-test")
    register_project_tools(server, bridge)
    tools = {tool.name: tool for tool in await server.list_tools()}
    assert tools["unreal_validation_start"].annotations.readOnlyHint is False
    assert tools["unreal_validation_cancel"].annotations.readOnlyHint is False
    assert tools["unreal_validation_job"].annotations.readOnlyHint is True
    assert tools["unreal_blueprint_inspect"].annotations.readOnlyHint is True
    await server.call_tool(
        "unreal_validation_start",
        {
            "rule_ids": ["approved_mesh"],
            "asset_paths": ["/Game/Mesh.Mesh"],
        },
    )
    bridge.call.assert_awaited_once_with(
        "validation_start",
        {
            "rule_ids": ["approved_mesh"],
            "asset_paths": ["/Game/Mesh.Mesh"],
        },
    )


@pytest.mark.parametrize(
    "name,arguments",
    [
        ("unreal_validation_start", {"rule_ids": [], "asset_paths": ["/Game/A.A"]}),
        ("unreal_validation_start", {"rule_ids": ["a"] * 9, "asset_paths": ["/Game/A.A"]}),
        ("unreal_validation_start", {"rule_ids": ["a"], "asset_paths": ["/Game/A.A"] * 21}),
        ("unreal_blueprint_inspect", {"asset_path": "/Game/B.B", "pin_limit": True}),
        ("unreal_blueprint_inspect", {"asset_path": "/Game/B.B", "node_limit": 257}),
        ("unreal_asset_dependencies", {"asset_path": "/Game/A.A", "direction": "execute"}),
        ("unreal_asset_dependencies", {"asset_path": "/Game/A.A", "limit": 201}),
        ("unreal_pending_plans", {"limit": 65}),
    ],
)
async def test_invalid_project_tool_inputs_never_reach_native_bridge(name, arguments):
    bridge = AsyncMock()
    server = FastMCP("project-test")
    register_project_tools(server, bridge)
    with pytest.raises(ToolError):
        await server.call_tool(name, arguments)
    bridge.call.assert_not_called()


async def test_valid_native_error_remains_actionable_without_private_message():
    def handler(request):
        action = json.loads(request.content)["action"]
        if action == "status":
            return httpx.Response(
                200,
                json={
                    "ok": True,
                    "result": {
                        "project_file": "/expected/Game.uproject",
                        "capabilities": ACTIONS,
                        "session_id": "session-a",
                        "world_path": "/Game/WorldA",
                        "revision": "revision-a",
                    },
                },
            )
        return httpx.Response(
            200,
            json={
                "ok": False,
                "error": {
                    "code": "rule_not_allowed",
                    "message": "private project diagnostics",
                },
            },
        )

    bridge = UnrealBridge(
        Settings(bridge_token="x" * 64, expected_project="/expected/Game.uproject"),
        httpx.MockTransport(handler),
    )
    try:
        with pytest.raises(JevError) as failure:
            await bridge.call("validation_start")
        assert failure.value.code == "rule_not_allowed"
        assert "private" not in str(failure.value)
    finally:
        await bridge.close()


@pytest.mark.parametrize("changed", ["project_file", "session_id", "world_path", "revision"])
async def test_validation_start_is_bound_to_preflight_inside_native_request(changed):
    original = {
        "project_file": "/expected/Game.uproject",
        "session_id": "session-a",
        "world_path": "/Game/WorldA",
        "revision": "revision-a",
        "capabilities": ACTIONS,
    }
    replacement = {**original, changed: "/replacement/value"}
    seen, executed = [], []

    def handler(request):
        body = json.loads(request.content)
        seen.append(body)
        if body["action"] == "status":
            return httpx.Response(200, json={"ok": True, "result": original})
        assert body["params"]["expected_project"] == original["project_file"]
        assert body["params"]["expected_state"] == {
            name: original[name] for name in ("session_id", "world_path", "revision")
        }
        # Model native endpoint replacement after the authenticated status read.
        matches = body["params"]["expected_project"] == replacement["project_file"] and all(
            body["params"]["expected_state"][name] == replacement[name]
            for name in ("session_id", "world_path", "revision")
        )
        if matches:
            executed.append(True)
            return httpx.Response(200, json={"ok": True, "result": {"state": "queued"}})
        return httpx.Response(
            200,
            json={
                "ok": False,
                "error": {"code": "wrong_project" if changed == "project_file" else "stale_plan"},
            },
        )

    bridge = UnrealBridge(
        Settings(bridge_token="x" * 64, expected_project=original["project_file"]),
        httpx.MockTransport(handler),
    )
    arguments = {
        "rule_ids": ["approved"],
        "asset_paths": ["/Game/A.A"],
        "expected_project": "/caller-cannot-override/project.uproject",
        "expected_state": {"session_id": "caller-cannot-override"},
    }
    try:
        with pytest.raises(JevError) as failure:
            await bridge.call("validation_start", arguments)
        expected_code = "wrong_project" if changed == "project_file" else "stale_plan"
        assert failure.value.code == expected_code
        assert not executed
        assert [call["action"] for call in seen] == ["status", "validation_start"]
        assert arguments["expected_state"] == {"session_id": "caller-cannot-override"}
    finally:
        await bridge.close()


@pytest.mark.parametrize(
    "name,value",
    [
        ("session_id", None),
        ("session_id", ""),
        ("session_id", "x" * 65),
        ("world_path", 4),
        ("world_path", "x" * 1025),
        ("revision", "x" * 129),
        ("revision", "invalid\nrevision"),
    ],
)
async def test_invalid_validation_preflight_identity_never_sends_mutation(name, value):
    status = {
        "project_file": "/expected/Game.uproject",
        "session_id": "session-a",
        "world_path": "/Game/WorldA",
        "revision": "revision-a",
        "capabilities": ACTIONS,
        name: value,
    }
    actions = []

    def handler(request):
        actions.append(json.loads(request.content)["action"])
        return httpx.Response(200, json={"ok": True, "result": status})

    bridge = UnrealBridge(
        Settings(bridge_token="x" * 64, expected_project=status["project_file"]),
        httpx.MockTransport(handler),
    )
    try:
        with pytest.raises(JevError) as failure:
            await bridge.call("validation_start", {"rule_ids": ["a"], "asset_paths": ["/Game/A.A"]})
        assert failure.value.code == "bridge_error"
        assert actions == ["status"]
    finally:
        await bridge.close()
