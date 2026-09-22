"""MCP stdio surface with a deliberately small tool vocabulary."""

from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from typing import Annotated, Any

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations
from pydantic import Field

from .bridge import UnrealBridge
from .config import Settings
from .decision import DecisionClient
from .errors import JevError
from .workflows import CATALOG, Candidate, Operation, route, triage


def create_server(settings: Settings | None = None) -> FastMCP:
    settings = settings or Settings.from_env()
    decisions = DecisionClient(settings)
    bridge = UnrealBridge(settings)

    @asynccontextmanager
    async def lifespan(server: FastMCP) -> AsyncIterator[dict]:
        try:
            yield {}
        finally:
            await decisions.close()
            await bridge.close()

    server = FastMCP(
        "Jev Unreal",
        instructions=(
            "Jev provides bounded judgments, not code generation or proof. Use deterministic "
            "Unreal tools directly when the next action is known. Decision tools send ONLY their "
            "explicit arguments to the configured cloud provider. Scene tools stay on loopback. "
            "Inspect the project first, preview edits, review the returned plan, then apply its ID "
            "only within the user's authorized scope. Never treat model confidence as permission."
        ),
        lifespan=lifespan,
    )
    read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
    cloud = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=True)

    async def safely(awaitable):
        try:
            return {"ok": True, "result": await awaitable}
        except JevError as exc:
            return exc.as_dict()

    @server.tool(annotations=read)
    async def jev_status() -> dict[str, Any]:
        """Show local provider counters and authenticated Unreal connection identity."""
        editor = await safely(bridge.call("status"))
        return {"decisions": decisions.metrics(), "editor": editor}

    @server.tool(annotations=cloud)
    async def jev_decide(
        state: str | dict | list,
        questions: dict,
    ) -> dict[str, Any]:
        """Send explicit state and 1–32 Choice/Score/Noul questions to Jev. No tools are executed.

        Question: {type:'choice',instructions:'...',criteria:{id:'description',other:'...'}};
        or {type:'noul',instructions:'...'};
        or {type:'score',instructions:'...',criteria:['low','high']}.
        Cloud request limit 64 KiB. Noul is a probability, not a severity score.
        """
        return await safely(decisions.decide(state, questions))

    @server.tool(annotations=cloud)
    async def jev_route(
        goal: Annotated[str, Field(min_length=1, max_length=12000)],
        candidates: Annotated[list[Candidate] | None, Field(max_length=64)] = None,
    ) -> dict[str, Any]:
        """Recommend one candidate tool or defer. Sends supplied goal/candidates to Jev.

        Omit candidates for built-in Unreal workflows. Supply a shortlist from another Unreal or
        future Blender MCP server to route its tools without loading every schema into the agent.
        Missing/low confidence defers. Thresholds are initial defaults, not accuracy claims.
        """
        return await safely(route(decisions, goal, candidates))

    @server.tool(annotations=cloud)
    async def jev_triage(
        log_excerpt: Annotated[str, Field(min_length=1, max_length=24000)],
    ) -> dict[str, Any]:
        """Classify a supplied Unreal log excerpt in one batched Jev call. Cloud data transfer.

        Only supply relevant log lines with credentials and private data removed. The result
        suggests a diagnostic category; it does not certify a build or alter the editor.
        """
        return await safely(triage(decisions, log_excerpt))

    @server.tool(annotations=read)
    async def unreal_status() -> dict[str, Any]:
        """Inspect actual editor engine version, project file, session and world revision."""
        return await safely(bridge.call("status"))

    @server.tool(annotations=read)
    async def unreal_actors(
        query: Annotated[str, Field(max_length=200)] = "",
        limit: Annotated[int, Field(ge=1, le=200)] = 100,
    ) -> dict[str, Any]:
        """Read a bounded actor snapshot with stable paths and transforms; no cloud call."""
        return await safely(bridge.call("actors", {"query": query, "limit": limit}))

    @server.tool(annotations=read)
    async def unreal_assets(
        query: Annotated[str, Field(max_length=200)] = "",
        path: Annotated[str, Field(max_length=200)] = "/Game",
        limit: Annotated[int, Field(ge=1, le=200)] = 100,
    ) -> dict[str, Any]:
        """Search the Unreal asset registry. Results are metadata, not visual acceptance."""
        return await safely(bridge.call("assets", {"query": query, "path": path, "limit": limit}))

    @server.tool(annotations=read)
    async def unreal_preview(
        operations: Annotated[list[Operation], Field(min_length=1, max_length=20)],
    ) -> dict[str, Any]:
        """Validate and preview primitive spawns/transforms. Returns a short-lived single-use plan.

        No scene changes occur. Changes to tracked actor state invalidate a plan. Units: cm,
        rotation [pitch,yaw,roll] degrees. Requires JEV_EXPECTED_PROJECT. Review before applying.
        Transforms support exact native, unattached StaticMeshActors only, not Blueprint actors.
        """
        return await safely(
            bridge.call(
                "preview", {"operations": [op.model_dump(exclude_none=True) for op in operations]}
            )
        )

    @server.tool(
        annotations=ToolAnnotations(
            readOnlyHint=False,
            destructiveHint=False,
            idempotentHint=False,
            openWorldHint=False,
        )
    )
    async def unreal_apply(
        plan_id: Annotated[str, Field(min_length=1, max_length=64)],
    ) -> dict[str, Any]:
        """Apply a reviewed preview plan once as an editor Undo transaction. Does not save to disk.

        Call only for edits the user authorized. A stale/expired plan requires a new preview.
        Never automatically retry this call after a timeout: inspect the actual scene first.
        """
        return await safely(bridge.call("apply", {"plan_id": plan_id}))

    @server.resource("jev://catalog")
    def catalog() -> dict[str, Any]:
        """Built-in descriptions; routing also accepts external tool shortlists."""
        return CATALOG

    return server
