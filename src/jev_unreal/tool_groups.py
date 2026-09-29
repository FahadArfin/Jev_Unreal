"""Startup-scoped MCP tool catalogs; discovery is separate from permissions."""

import hashlib
import json
from typing import Annotated, Any

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from pydantic import Field

from .errors import JevError

GROUPS = {
    "core": {
        "jev_status",
        "jev_provider_health",
        "jev_route_selective",
        "unreal_status",
        "unreal_read",
        "jev_tool_groups",
        "jev_tool_schema",
    },
    "inspection": {
        "unreal_context",
        "unreal_actor_details",
        "unreal_actors",
        "unreal_assets",
        "unreal_asset_details",
        "unreal_validate",
        "unreal_asset_dependencies",
        "unreal_asset_import_info",
        "jev_diagnostics",
        "jev_rank_assets",
    },
    "scene": {
        "unreal_snapshot",
        "unreal_diff",
        "unreal_verify",
        "unreal_preview",
        "unreal_apply",
        "unreal_layout_preview",
        "unreal_spatial_preview",
        "unreal_mesh_preview",
        "unreal_plan",
        "unreal_pending_plans",
        "unreal_capture",
        "unreal_frame",
    },
    "blueprints": {
        "unreal_blueprint_inspect",
        "unreal_blueprint_compile_targets",
        "unreal_blueprint_compile_preview",
        "unreal_blueprint_compile",
        "unreal_blueprint_pin_preview",
        "unreal_blueprint_graph_preview",
        "unreal_blueprint_compile_receipt",
    },
    "domains": {
        "unreal_workflow_inspect",
        "unreal_workflow_preview",
        "unreal_workflow_apply",
        "unreal_workflow_receipt",
        "unreal_surface_preview",
    },
    "validation": {
        "unreal_validation_rules",
        "unreal_validation_start",
        "unreal_validation_job",
        "unreal_validation_cancel",
    },
    "gameplay": {
        "unreal_functional_tests",
        "unreal_functional_start",
        "unreal_functional_job",
        "unreal_functional_cancel",
    },
    "performance": {
        "unreal_performance_start",
        "unreal_performance_job",
        "unreal_performance_cancel",
        "unreal_performance_compare",
    },
    "team": {
        "unreal_team_status",
        "unreal_durable_receipts",
        "unreal_durable_receipt",
        "unreal_durable_receipt_forget",
        "unreal_project_lease",
        "unreal_project_lease_release",
    },
    "jobs": {
        "unreal_named_job_preview",
        "unreal_named_job_start",
        "unreal_named_job",
        "unreal_named_job_cancel",
    },
    "decisions": {"jev_decide", "jev_route", "jev_triage"},
    "discovery": {
        "jev_catalog_refresh",
        "jev_catalog_search",
        "jev_catalog_get",
        "jev_catalog_rerank",
    },
    "handoff": {"unreal_handoff_verify"},
    "acceptance": {
        "unreal_acceptance_capture",
        "unreal_acceptance_compare",
        "unreal_acceptance_playtest_start",
        "unreal_acceptance_playtest_job",
        "unreal_functional_tests",
        "unreal_functional_cancel",
    },
}


def parse_groups(value: str) -> frozenset[str]:
    selected = value.split(",")
    if not value.strip() or any(not part.strip() for part in selected):
        raise JevError(
            "configuration", "JEV_TOOL_GROUPS must name all or known comma-separated groups."
        )
    groups = frozenset(part.strip() for part in selected)
    if groups == {"all"}:
        return frozenset(GROUPS)
    if not groups <= GROUPS.keys():
        raise JevError("configuration", "Unknown tool group, or all combined with other groups.")
    return groups | {"core"}


class GroupedMCP(FastMCP):
    def __init__(self, *args, tool_groups: str = "all", **kwargs):
        self.selected_groups = parse_groups(tool_groups)
        self.enabled_tools = frozenset().union(*(GROUPS[g] for g in self.selected_groups))
        super().__init__(*args, **kwargs)

    async def all_tools(self):
        return await super().list_tools()

    async def list_tools(self):
        return [t for t in await self.all_tools() if t.name in self.enabled_tools]

    async def call_tool(self, name, arguments):
        if name not in self.enabled_tools:
            raise ValueError(
                "Tool is outside the active catalog. Select its group and reconnect MCP."
            )
        return await super().call_tool(name, arguments)


def register_group_tools(server: GroupedMCP) -> None:
    read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)

    @server.tool(annotations=read)
    async def jev_tool_groups() -> dict[str, Any]:
        """List compact built-in tool groups and active catalog hash; no schemas or cloud calls.

        Set JEV_TOOL_GROUPS=core,scene (for example) before starting the server, then reconnect.
        Groups reduce advertised tools; they do not grant editor permissions or authorize work.
        """
        tools = await server.all_tools()
        known = {t.name for t in tools}
        active = [t for t in tools if t.name in server.enabled_tools]
        encoded = json.dumps(
            [
                t.model_dump(mode="json", exclude_none=True)
                for t in sorted(active, key=lambda t: t.name)
            ],
            sort_keys=True,
            separators=(",", ":"),
        ).encode()
        return {
            "ok": True,
            "result": {
                "active_groups": sorted(server.selected_groups),
                "advertised_tools": len(active),
                "total_tools": len(tools),
                "catalog_sha256": hashlib.sha256(encoded).hexdigest(),
                "schema_bytes": len(encoded),
                "token_count": None,
                "groups": {g: sorted(names & known) for g, names in GROUPS.items()},
                "activation": "Set JEV_TOOL_GROUPS and reconnect; core is always included.",
                "permissions": "Catalog selection grants no permissions or execution approval.",
            },
        }

    @server.tool(annotations=read)
    async def jev_tool_schema(
        name: Annotated[str, Field(min_length=1, max_length=128)],
    ) -> dict[str, Any]:
        """Retrieve one exact built-in tool schema without enabling or executing it.

        Use jev_tool_groups for names. A tool outside the active groups requires reconnection
        with its group selected. External catalog discovery is a separate feature.
        """
        for tool in await server.all_tools():
            if tool.name == name:
                return {
                    "ok": True,
                    "result": {
                        "tool": tool.model_dump(mode="json", exclude_none=True),
                        "active": name in server.enabled_tools,
                        "groups": sorted(g for g, names in GROUPS.items() if name in names),
                        "executed": False,
                    },
                }
        return JevError("unknown_tool", "No built-in tool has that exact name.").as_dict()
