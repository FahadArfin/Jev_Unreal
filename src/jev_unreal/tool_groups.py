"""Session-scoped MCP tool catalogs; discovery is separate from permissions."""

import asyncio
import hashlib
import json
import weakref
from dataclasses import dataclass, field
from typing import Annotated, Any

from mcp.server.fastmcp import FastMCP
from mcp.server.lowlevel import NotificationOptions
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
        "jev_tool_groups_activate",
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
        "unreal_workflow_run_preview",
        "unreal_workflow_run_apply",
        "unreal_workflow_run",
        "unreal_workflow_run_cancel",
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
        "unreal_runtime_status",
        "unreal_runtime_preview",
        "unreal_runtime_apply",
        "unreal_runtime_receipt",
        "unreal_runtime_capture",
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
        "unreal_project_vcs_status",
        "unreal_project_checkpoint",
        "unreal_project_checkpoint_compare",
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
    "handoff": {
        "unreal_handoff_verify", "unreal_handoff_bundles",
        "unreal_handoff_import_preview", "unreal_handoff_import_apply",
    },
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


@dataclass
class _SessionGroups:
    groups: frozenset[str]
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)


class GroupedMCP(FastMCP):
    def __init__(self, *args, tool_groups: str = "all", **kwargs):
        self._startup_groups = parse_groups(tool_groups)
        self._session_groups = weakref.WeakKeyDictionary()
        super().__init__(*args, **kwargs)
        # All FastMCP transports obtain their initialize result through this SDK
        # method. Preserve the SDK's other options while advertising notifications.
        original = self._mcp_server.create_initialization_options

        def initialization_options(notification_options=None, experimental_capabilities=None):
            options = notification_options or NotificationOptions()
            options.tools_changed = True
            return original(options, experimental_capabilities)

        self._mcp_server.create_initialization_options = initialization_options

    def current_session(self):
        try:
            return self.get_context().session
        except (ValueError, RuntimeError):
            return None

    @property
    def selected_groups(self):
        session = self.current_session()
        state = self._session_groups.get(session) if session is not None else None
        return state.groups if state else self._startup_groups

    @property
    def enabled_tools(self):
        return frozenset().union(*(GROUPS[g] for g in self.selected_groups))

    async def activate_groups(self, groups: list[str], client_supports_list_changed: bool):
        if (not groups or len(groups) > len(GROUPS) or len(set(groups)) != len(groups)
                or any(group not in GROUPS and group != "all" for group in groups)):
            raise JevError("invalid_groups", "Select unique documented groups, or all by itself.")
        selected = parse_groups(",".join(groups))
        session = self.current_session()
        if not client_supports_list_changed or session is None:
            return {
                "activated": False,
                "notification_sent": False,
                "reconnect_required": True,
                "requested_groups": sorted(selected),
                "active_groups": sorted(self.selected_groups),
                "reason": "Live activation requires an MCP session and explicit client support.",
                "instruction": "Set JEV_TOOL_GROUPS and reconnect this client.",
            }
        state = self._session_groups.setdefault(session, _SessionGroups(self._startup_groups))
        async with state.lock:
            previous = state.groups
            if previous == selected:
                return {"activated": True, "active_groups": sorted(selected),
                        "notification_sent": False, "changed": False,
                        "reconnect_required": False}
            state.groups = selected
            try:
                await session.send_tool_list_changed()
            except BaseException:
                # Includes cancellation: no half-finished catalog expansion.
                state.groups = previous
                raise
            return {
                "activated": True, "active_groups": sorted(selected), "changed": True,
                "notification_sent": True, "reconnect_required": False,
                "client_refresh_verified": False,
                "scope": "This MCP session only; reconnect restores startup groups.",
                "instruction": "Refresh tools/list. If the client ignores it, reconnect instead.",
            }

    async def all_tools(self):
        return await super().list_tools()

    async def list_tools(self):
        session = self.current_session()
        state = self._session_groups.get(session) if session is not None else None
        if state:
            async with state.lock:
                enabled = self.enabled_tools
        else:
            enabled = self.enabled_tools
        return [t for t in await self.all_tools() if t.name in enabled]

    async def call_tool(self, name, arguments):
        session = self.current_session()
        state = self._session_groups.get(session) if session is not None else None
        if state and name != "jev_tool_groups_activate":
            async with state.lock:
                enabled = self.enabled_tools
        else:
            enabled = self.enabled_tools
        if name not in enabled:
            raise ValueError(
                "Tool is outside the active catalog. Activate its group or reconnect MCP."
            )
        return await super().call_tool(name, arguments)


def register_group_tools(server: GroupedMCP) -> None:
    read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)

    @server.tool(annotations=read)
    async def jev_tool_groups() -> dict[str, Any]:
        """List compact built-in tool groups and active catalog hash; no schemas or cloud calls.

        Use jev_tool_groups_activate in compatible clients, or set JEV_TOOL_GROUPS and reconnect.
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
                "activation": "jev_tool_groups_activate or JEV_TOOL_GROUPS + reconnect.",
                "session_activation_available": server.current_session() is not None,
                "permissions": "Catalog selection grants no permissions or execution approval.",
            },
        }

    @server.tool(annotations=read)
    async def jev_tool_schema(
        name: Annotated[str, Field(min_length=1, max_length=128)],
    ) -> dict[str, Any]:
        """Retrieve one exact built-in tool schema without enabling or executing it.

        Use jev_tool_groups for names, then explicitly activate a group or reconnect.
        External catalog discovery is a separate feature.
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

    @server.tool(annotations=ToolAnnotations(
        readOnlyHint=False, destructiveHint=False, openWorldHint=False
    ))
    async def jev_tool_groups_activate(
        groups: Annotated[list[str], Field(min_length=1, max_length=len(GROUPS))],
        client_supports_list_changed: bool = False,
    ) -> dict[str, Any]:
        """Explicitly replace this session's advertised groups; core always remains.

        Set client_supports_list_changed=true only when the client refreshes tools/list on
        notifications/tools/list_changed. MCP has no standardized client capability for this.
        Otherwise returns startup configuration/reconnect guidance without changes.
        Changes only discovery: never grants permissions, approves edits, or stops in-flight calls.
        """
        try:
            return {"ok": True, "result": await server.activate_groups(
                groups, client_supports_list_changed
            )}
        except JevError as exc:
            return exc.as_dict()
        except Exception:
            return JevError(
                "catalog_notification_failed", "Notification failed; previous groups restored."
            ).as_dict()
