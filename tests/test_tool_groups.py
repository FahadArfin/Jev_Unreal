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
