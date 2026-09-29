import pytest

from jev_unreal.config import Settings
from jev_unreal.errors import JevError
from jev_unreal.server import create_server
from jev_unreal.tool_groups import GROUPS, parse_groups


async def test_default_catalog_is_covered_and_core_is_a_smaller_callable_catalog():
    full = create_server(Settings())
    core = create_server(Settings(tool_groups="core"))
    async with full.settings.lifespan(full), core.settings.lifespan(core):
        all_names = {t.name for t in await full.list_tools()}
        assert all_names == {t.name for t in await full.all_tools()}
        assert all_names == set().union(*GROUPS.values())
        assert {t.name for t in await core.list_tools()} == GROUPS["core"]
        with pytest.raises(ValueError, match="outside the active catalog"):
            await core.call_tool("unreal_apply", {"plan_id": "not-a-plan"})
        _, schema = await core.call_tool("jev_tool_schema", {"name": "unreal_apply"})
        assert schema["result"]["active"] is False
        assert schema["result"]["tool"]["inputSchema"]["properties"]["plan_id"]
        assert schema["result"]["executed"] is False
        _, compact = await core.call_tool("jev_tool_groups", {})
        _, complete = await full.call_tool("jev_tool_groups", {})
        assert compact["result"]["schema_bytes"] < complete["result"]["schema_bytes"]
        assert compact["result"]["catalog_sha256"] != complete["result"]["catalog_sha256"]
        assert compact["result"]["token_count"] is None
        _, health = await core.call_tool("jev_provider_health", {})
        assert health["result"]["authentication"]["status"] == "unknown"
        assert health["result"]["probe"]["request_sent"] is False
        _, ambiguous = await core.call_tool(
            "jev_route_selective", {"goal": "Investigate the scene"}
        )
        assert ambiguous["result"]["outcome"] == "defer"
        assert set(ambiguous["result"]["candidate_ids"]) == {"unreal_status", "unreal_read"}


def test_group_config_is_explicit_and_fails_closed(monkeypatch):
    for value in ("", "core,", "all,scene", "typo"):
        with pytest.raises(JevError) as caught:
            Settings(tool_groups=value)
        assert caught.value.code == "configuration"
    assert parse_groups("scene, blueprints") == {"core", "scene", "blueprints"}
    monkeypatch.setenv("JEV_TOOL_GROUPS", "core")
    assert Settings.from_env().tool_groups == "core"


async def test_live_catalog_notification_is_session_local_and_can_disable_cached_tools():
    from mcp.shared.memory import create_connected_server_and_client_session

    from jev_unreal.tool_groups import GroupedMCP, register_group_tools

    server = GroupedMCP("groups-test", tool_groups="core")
    register_group_tools(server)

    @server.tool()
    async def unreal_actors() -> dict:
        return {"inspected": True}

    notifications = []

    async def receive(message):
        if getattr(getattr(message, "root", None), "method", None):
            notifications.append(message.root.method)

    assert server._mcp_server.create_initialization_options().capabilities.tools.listChanged
    async with (
        create_connected_server_and_client_session(server, message_handler=receive) as first,
        create_connected_server_and_client_session(server) as other,
    ):
        initial = {t.name for t in (await first.list_tools()).tools}
        no_opt_in = await first.call_tool("jev_tool_groups_activate", {"groups": ["inspection"]})
        assert no_opt_in.structuredContent["result"]["reconnect_required"]
        assert initial == {t.name for t in (await first.list_tools()).tools}
        activated = await first.call_tool("jev_tool_groups_activate", {
            "groups": ["inspection"], "client_supports_list_changed": True
        })
        assert activated.structuredContent["result"]["notification_sent"]
        assert "notifications/tools/list_changed" in notifications
        assert "unreal_actors" in {t.name for t in (await first.list_tools()).tools}
        assert "unreal_actors" not in {t.name for t in (await other.list_tools()).tools}
        assert not (await first.call_tool("unreal_actors", {})).isError
        assert (await other.call_tool("unreal_actors", {})).isError
        await first.call_tool("jev_tool_groups_activate", {
            "groups": ["core"], "client_supports_list_changed": True
        })
        assert (await first.call_tool("unreal_actors", {})).isError
        assert initial == {t.name for t in (await first.list_tools()).tools}
    assert server.selected_groups == {"core"}


async def test_activation_failure_and_missing_session_preserve_previous_catalog(monkeypatch):
    from jev_unreal.tool_groups import GroupedMCP

    server = GroupedMCP("groups-test", tool_groups="core")
    fallback = await server.activate_groups(["scene"], True)
    assert fallback["reconnect_required"] and server.selected_groups == {"core"}

    class BrokenSession:
        async def send_tool_list_changed(self):
            raise OSError("transport closed")

    session = BrokenSession()
    monkeypatch.setattr(server, "current_session", lambda: session)
    with pytest.raises(OSError):
        await server.activate_groups(["scene"], True)
    assert server.selected_groups == {"core"}
    for groups in (["core", "core"], [], ["unknown"], ["all", "scene"]):
        with pytest.raises(JevError):
            await server.activate_groups(groups, True)
    assert server.selected_groups == {"core"}
