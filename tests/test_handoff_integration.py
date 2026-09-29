"""Handoff adapter/policy failure boundaries using synthetic loopback HTTP only."""

import json
from unittest.mock import AsyncMock

import httpx
import pytest

from jev_unreal.bridge import UnrealBridge
from jev_unreal.config import Settings
from jev_unreal.errors import JevError
from jev_unreal.infrastructure import ProjectInfrastructure

PROJECT = "C:/PublicFixture/Fixture.uproject"
TOKEN = "synthetic-bridge-token-longer-than-thirty-two"
STATE = {"session_id": "session", "world_path": "/Game/Approved/Map.Map", "revision": "r"}


@pytest.mark.parametrize("action", ["handoff_preview", "handoff_apply"])
async def test_handoff_transport_requires_project_capability_and_rebinds_project(action):
    calls = []
    advertised = []

    def handler(request):
        body = json.loads(request.content)
        calls.append(body)
        result = {"project_file": PROJECT, "capabilities": advertised, **STATE}
        return httpx.Response(200, json={"ok": True, "result": result})

    for expected, failure in [("", "project_required"), (PROJECT, "capability_unavailable")]:
        bridge = UnrealBridge(
            Settings(expected_project=expected, bridge_token=TOKEN), httpx.MockTransport(handler)
        )
        try:
            with pytest.raises(JevError) as caught:
                await bridge.call(action, {"plan_id": "p", "expected_project": "forged"})
            assert caught.value.code == failure
        finally:
            await bridge.close()
    assert [item["action"] for item in calls] == ["status", "status"]
    advertised.append("handoff_import")
    bridge = UnrealBridge(
        Settings(expected_project=PROJECT, bridge_token=TOKEN), httpx.MockTransport(handler)
    )
    try:
        await bridge.call(action, {"expected_project": "forged", "expected_state": STATE})
    finally:
        await bridge.close()
    assert calls[-1]["action"] == action
    assert calls[-1]["params"]["expected_project"] == PROJECT
    assert calls[-1]["params"]["expected_state"] == STATE


async def test_unapproved_native_handoff_target_never_authorizes_apply(tmp_path):
    project = tmp_path / "project" / "Fixture.uproject"
    project.parent.mkdir()
    project.write_text("{}")
    config = tmp_path / "runtime.json"
    config.write_text(
        json.dumps(
            {
                "version": 1,
                "project_file": str(project),
                "state_directory": str(tmp_path / "state"),
                "allowed_actions": ["handoff_preview", "handoff_apply"],
                "asset_roots": ["/Game/Approved"],
            }
        )
    )
    runtime = ProjectInfrastructure(
        Settings(expected_project=str(project), runtime_config_file=str(config))
    )
    dispatch = AsyncMock(return_value={"plan_id": "p", "asset_path": "/Game/Other/Prop.Prop"})
    try:
        with pytest.raises(JevError) as caught:
            await runtime.execute("handoff_preview", {"alias": "reviewed"}, STATE, dispatch)
        assert caught.value.code == "policy_denied"
        dispatch.reset_mock()
        with pytest.raises(JevError) as caught:
            await runtime.execute("handoff_apply", {"plan_id": "p"}, STATE, dispatch)
        assert caught.value.code == "policy_plan_required"
        dispatch.assert_not_awaited()
    finally:
        await runtime.close()
