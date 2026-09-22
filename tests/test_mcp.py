"""Real stdio MCP handshake and tool calls, without any network credentials."""

import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def test_stdio_protocol_and_offline_failure():
    params = StdioServerParameters(
        command=sys.executable,
        args=["-m", "jev_unreal", "serve"],
        env={
            "OPENROUTER_API_KEY": "",
            "TYPESAFE_API_KEY": "",
            "JEV_BRIDGE_TOKEN": "",
            "JEV_BRIDGE_TOKEN_FILE": "",
            "JEV_PROVIDER": "openrouter",
        },
    )
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            initialized = await session.initialize()
            assert initialized.serverInfo.name == "Jev Unreal"
            tools = await session.list_tools()
            by_name = {tool.name: tool for tool in tools.tools}
            assert set(by_name) == {
                "jev_status",
                "jev_decide",
                "jev_route",
                "jev_triage",
                "unreal_status",
                "unreal_actors",
                "unreal_assets",
                "unreal_preview",
                "unreal_apply",
                "jev_catalog_refresh",
                "jev_catalog_search",
                "jev_catalog_get",
                "jev_catalog_rerank",
                "jev_rank_assets",
                "jev_diagnostics",
                "unreal_context",
                "unreal_asset_details",
                "unreal_validate",
                "unreal_capture",
                "unreal_frame",
                "unreal_layout_preview",
            }
            assert by_name["unreal_apply"].annotations.readOnlyHint is False
            assert by_name["unreal_frame"].annotations.readOnlyHint is False
            assert by_name["jev_route"].annotations.openWorldHint is True
            result = await session.call_tool(
                "jev_decide",
                {
                    "state": "test",
                    "questions": {"q": {"type": "noul", "instructions": "Ready?"}},
                },
            )
            assert result.structuredContent["error"]["code"] == "missing_api_key"
            status = await session.call_tool("unreal_status", {})
            assert status.structuredContent["error"]["code"] == "missing_bridge_token"
            invalid = await session.call_tool("unreal_preview", {"operations": []})
            assert invalid.isError
            resource = await session.read_resource("jev://catalog")
            assert "unreal_actors" in resource.contents[0].text
            layouts = await session.read_resource("jev://layouts")
            assert "stairs" in layouts.contents[0].text
            prompts = await session.list_prompts()
            assert {p.name for p in prompts.prompts} == {
                "blockout_workflow",
                "diagnostic_workflow",
            }
            recipe = await session.call_tool(
                "unreal_layout_preview",
                {
                    "layout": {
                        "kind": "grid",
                        "rows": 1,
                        "columns": 2,
                    }
                },
            )
            assert recipe.structuredContent["error"]["code"] == "missing_bridge_token"
            assets = await session.call_tool(
                "jev_rank_assets",
                {
                    "goal": "wooden door",
                    "candidates": [
                        {"id": "door", "path": "/Game/Door.Door", "description": "wooden door"},
                    ],
                },
            )
            assert assets.structuredContent["ok"]
            assert assets.structuredContent["result"]["cloud"]["requested"] is False
            diagnostics = await session.call_tool(
                "jev_diagnostics",
                {
                    "log_text": "LogTemp: Warning: Missing mesh\nLogTemp: Warning: Missing mesh",
                },
            )
            assert diagnostics.structuredContent["result"]["groups"][0]["count"] == 2
            refreshed = await session.call_tool("jev_catalog_refresh", {})
            assert refreshed.structuredContent["ok"]
            assert refreshed.structuredContent["result"]["tool_count"] == 0
            capture = await session.call_tool("unreal_capture", {})
            assert capture.isError
            assert capture.structuredContent["error"]["code"] == "missing_bridge_token"
