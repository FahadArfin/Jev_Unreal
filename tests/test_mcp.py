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
            "JEV_PROFILE": "",
            "JEV_PROFILES_FILE": "",
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
                "unreal_actor_details",
                "unreal_snapshot",
                "unreal_diff",
                "unreal_verify",
                "unreal_spatial_preview",
                "unreal_mesh_preview",
                "unreal_plan",
                "unreal_pending_plans",
                "unreal_blueprint_inspect",
                "unreal_blueprint_compile_targets",
                "unreal_blueprint_compile_preview",
                "unreal_blueprint_compile",
                "unreal_blueprint_pin_preview",
                "unreal_workflow_inspect",
                "unreal_workflow_preview",
                "unreal_workflow_apply",
                "unreal_workflow_receipt",
                "unreal_surface_preview",
                "unreal_performance_start",
                "unreal_performance_job",
                "unreal_performance_cancel",
                "unreal_performance_compare",
                "unreal_blueprint_compile_receipt",
                "unreal_asset_dependencies",
                "unreal_asset_import_info",
                "unreal_validation_rules",
                "unreal_validation_start",
                "unreal_validation_job",
                "unreal_validation_cancel",
                "unreal_functional_tests",
                "unreal_functional_start",
                "unreal_functional_job",
                "unreal_functional_cancel",
            }
            assert by_name["unreal_apply"].annotations.readOnlyHint is False
            assert len(by_name) == 54
            assert by_name["unreal_frame"].annotations.readOnlyHint is False
            assert set(by_name["unreal_frame"].inputSchema["properties"]["view"]["enum"]) == {
                "current",
                "isometric",
                "top",
                "front",
                "right",
            }
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
            domains = await session.read_resource("jev://domain-workflows")
            assert "unreal_blueprint_pin_preview" in domains.contents[0].text
            assert "readback_verified" in domains.contents[0].text
            prompts = await session.list_prompts()
            assert {p.name for p in prompts.prompts} == {
                "blockout_workflow",
                "diagnostic_workflow",
                "verified_edit_workflow",
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
            details = await session.call_tool(
                "unreal_actor_details", {"actor_paths": ["/Temp/A.A"]}
            )
            assert details.structuredContent["error"]["code"] == "missing_bridge_token"
            snapshot = await session.call_tool("unreal_snapshot", {"actor_paths": ["/Temp/A.A"]})
            assert snapshot.structuredContent["error"]["code"] == "missing_bridge_token"
            diff = await session.call_tool("unreal_diff", {"snapshot_id": "missing"})
            assert diff.structuredContent["result"]["status"] == "unverifiable"
            verification = await session.call_tool(
                "unreal_verify",
                {"checks": [{"kind": "label", "actor_path": "/Temp/A.A", "expected": "A"}]},
            )
            assert verification.structuredContent["result"]["status"] == "unverifiable"
            invalid_checks = await session.call_tool("unreal_verify", {"checks": []})
            assert invalid_checks.isError
            spatial = await session.call_tool(
                "unreal_spatial_preview",
                {
                    "recipe": {
                        "kind": "ground",
                        "actor_paths": ["/Temp/A.A"],
                        "z_cm": 0,
                    }
                },
            )
            assert spatial.structuredContent["error"]["code"] == "missing_bridge_token"
            mesh = await session.call_tool(
                "unreal_mesh_preview",
                {"recipe": {"kind": "duplicate", "actor_paths": ["/Temp/A.A"]}},
            )
            assert mesh.structuredContent["error"]["code"] == "missing_bridge_token"
            missing_policy = await session.call_tool(
                "unreal_mesh_preview",
                {
                    "recipe": {
                        "kind": "replace",
                        "actor_paths": ["/Temp/A.A"],
                        "asset_path": "/Engine/BasicShapes/Cube.Cube",
                    }
                },
            )
            assert missing_policy.isError
            record = await session.call_tool("unreal_plan", {"plan_id": "missing"})
            assert record.structuredContent["error"]["code"] == "missing_bridge_token"
            checks_resource = await session.read_resource("jev://checks")
            assert "bottom_z" in checks_resource.contents[0].text
            for name in (
                "unreal_actor_details",
                "unreal_snapshot",
                "unreal_diff",
                "unreal_verify",
                "unreal_spatial_preview",
                "unreal_mesh_preview",
                "unreal_plan",
            ):
                assert by_name[name].annotations.readOnlyHint is True
                assert by_name[name].annotations.openWorldHint is False
